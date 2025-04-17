// SPDX-License-Identifier: GPL-2.0-only
/*
 * Author: Erik Kaneda <erik.kaneda@intel.com>
 * Copyright 2020 Intel Corporation
 *
 * prmt.c
 *
 * Each PRM service is an executable that is run in a restricted environment
 * that is invoked by writing to the PlatformRtMechanism OperationRegion from
 * AML bytecode.
 *
 * init_prmt initializes the Platform Runtime Mechanism (PRM) services by
 * processing data in the PRMT as well as registering an ACPI OperationRegion
 * handler for the PlatformRtMechanism subtype.
 *
 */
#include <linux/kernel.h>
#include <linux/efi.h>
#include <linux/acpi.h>
#include <linux/prmt.h>
#include <asm/efi.h>

#pragma pack(1)
struct prm_mmio_addr_range {
	u64 phys_addr;
	u64 virt_addr;
	u32 length;
};
/**
 * struct prm_mmio_info - PRM模块的MMIO(内存映射I/O)信息结构体
 * 
 * 描述平台运行时机制模块使用的内存映射I/O区域信息，
 * 包含多个地址范围定义。
 */
struct prm_mmio_info {
	u64 mmio_count;//MMIO地址范围数量,确定addr_ranges数组的大小
	struct prm_mmio_addr_range addr_ranges[];//MMIO地址范围数组,每个元素描述一个连续的地址范围
};

struct prm_buffer {
	u8 prm_status;
	u64 efi_status;
	u8 prm_cmd;
	guid_t handler_guid;
};

struct prm_context_buffer {
	char signature[ACPI_NAMESEG_SIZE];
	u16 revision;
	u16 reserved;
	guid_t identifier;
	u64 static_data_buffer;
	struct prm_mmio_info *mmio_ranges;
};
#pragma pack()

static LIST_HEAD(prm_module_list);

/**
 * struct prm_handler_info - 内核PRM处理程序信息结构体
 * 
 * 描述内核中管理的平台运行时机制(PRM)处理程序信息，
 * 用于在运行时调用平台特定的处理函数。
 */
struct prm_handler_info {
	guid_t guid;//处理程序的全局唯一标识符(GUID)
	efi_status_t (__efiapi *handler_addr)(u64, void *);//处理程序函数指针
	u64 static_data_buffer_addr;//静态数据缓冲区地址(虚拟地址)
	u64 acpi_param_buffer_addr;// ACPI参数缓冲区地址(虚拟地址)

	struct list_head handler_list;//处理程序链表节点
};
/**
 * struct prm_module_info - 内核PRM模块信息结构体
 * 
 * 描述内核中管理的平台运行时机制(PRM)模块信息，
 * 包含模块元数据和关联的处理程序数组。
 */
struct prm_module_info {
	guid_t guid;//模块的全局唯一标识符(GUID)
	u16 major_rev;//模块主版本号
	u16 minor_rev;//模块次版本号
	u16 handler_count;// 处理程序数量
	struct prm_mmio_info *mmio_info;// MMIO信息指针
	bool updatable;//可更新标志

	struct list_head module_list;//模块链表节点
	struct prm_handler_info handlers[] __counted_by(handler_count);//处理程序数组(柔性数组)
};

static u64 efi_pa_va_lookup(u64 pa)
{
	efi_memory_desc_t *md;
	u64 pa_offset = pa & ~PAGE_MASK;
	u64 page = pa & PAGE_MASK;

	for_each_efi_memory_desc(md) {
		if (md->phys_addr < pa && pa < md->phys_addr + PAGE_SIZE * md->num_pages)
			return pa_offset + md->virt_addr + page - md->phys_addr;
	}

	return 0;
}

#define get_first_handler(a) ((struct acpi_prmt_handler_info *) ((char *) (a) + a->handler_info_offset))
#define get_next_handler(a) ((struct acpi_prmt_handler_info *) (sizeof(struct acpi_prmt_handler_info) + (char *) a))

/**
 * acpi_parse_prmt - 解析ACPI PRMT(Platform Runtime Mechanism Table)模块条目信息
 * @header: ACPI子表头指针
 * @end: 表结束地址(用于边界检查)
 *
 * 返回值:
 *   0 - 解析成功
 *   -ENOMEM - 内存分配失败
 */
static int __init
acpi_parse_prmt(union acpi_subtable_headers *header, const unsigned long end)
{
	struct acpi_prmt_module_info *module_info;//定义原始ACPI PRMT模块信息指针
	struct acpi_prmt_handler_info *handler_info;//定义原始ACPI PRMT处理程序信息指针
	struct prm_handler_info *th;//定义内核PRM处理程序信息指针
	struct prm_module_info *tm;//定义内核PRM模块信息指针
	u64 *mmio_count;//MMIO范围计数指针
	u64 cur_handler = 0;//当前处理程序索引，初始化为0
	u32 module_info_size = 0;//模块信息结构大小，初始化为0
	u64 mmio_range_size = 0;//MMIO范围信息大小，初始化为0
	void *temp_mmio;//临时MMIO映射指针

	module_info = (struct acpi_prmt_module_info *) header;//将ACPI表头指针转换为PRMT模块信息指针
	module_info_size = struct_size(tm, handlers, module_info->handler_info_count);//计算模块信息结构总大小 = 基础结构大小 + 处理程序数组大小
	tm = kmalloc(module_info_size, GFP_KERNEL);//分配内核内存用于存储模块信息
	if (!tm)
		goto parse_prmt_out1;//内存分配失败，跳转到错误处理标签1

	guid_copy(&tm->guid, (guid_t *) module_info->module_guid);//复制模块GUID到内核结构
	tm->major_rev = module_info->major_rev;//设置模块主版本号
	tm->minor_rev = module_info->minor_rev;//设置模块次版本号
	tm->handler_count = module_info->handler_info_count;//设置模块处理程序数量
	tm->updatable = true;// 默认设置模块为可更新状态

	if (module_info->mmio_list_pointer) {//检查是否存在MMIO列表指针
		/*
		 * Each module is associated with a list of addr
		 * ranges that it can use during the service
		 */
		mmio_count = (u64 *) memremap(module_info->mmio_list_pointer, 8, MEMREMAP_WB);//映射MMIO计数(前8字节)到内核地址空间（MEMREMAP_WB表示使用回写缓存模式）
		if (!mmio_count)//检查映射是否成功
			goto parse_prmt_out2;//跳转到错误处理标签2

		mmio_range_size = struct_size(tm->mmio_info, addr_ranges, *mmio_count);//计算MMIO范围信息结构总大小 = 基础结构大小 + 地址范围数组大小
		tm->mmio_info = kmalloc(mmio_range_size, GFP_KERNEL);//分配内核内存用于存储MMIO范围信息
		if (!tm->mmio_info)//检查内存是否分配成功
			goto parse_prmt_out3;// 跳转到错误处理标签3

		temp_mmio = memremap(module_info->mmio_list_pointer, mmio_range_size, MEMREMAP_WB);//映射完整的MMIO范围信息到内核地址空间
		if (!temp_mmio)
			goto parse_prmt_out4;//映射失败，跳转到错误处理标签4
		memmove(tm->mmio_info, temp_mmio, mmio_range_size);//将映射的MMIO数据复制到分配的内核结构中
	} else {
		tm->mmio_info = kmalloc(sizeof(*tm->mmio_info), GFP_KERNEL);//没有MMIO信息的情况，就分配基础MMIO信息结构
		if (!tm->mmio_info)
			goto parse_prmt_out2;//分配失败，跳转到错误处理标签2

		tm->mmio_info->mmio_count = 0;//设置MMIO计数为0
	}

	INIT_LIST_HEAD(&tm->module_list);//初始化模块链表头
	list_add(&tm->module_list, &prm_module_list);//将当前模块添加到全局PRM模块链表

	handler_info = get_first_handler(module_info);//获取第一个处理程序信息对象
	do {//循环复制module_info结构中的所有处理程序到th结构中
		th = &tm->handlers[cur_handler];//获取当前处理程序信息的内核结构指针

		guid_copy(&th->guid, (guid_t *)handler_info->handler_guid);// 复制处理程序GUID到内核结构
		th->handler_addr = (void *)efi_pa_va_lookup(handler_info->handler_address);//转换处理程序地址从物理地址到虚拟地址
		th->static_data_buffer_addr = efi_pa_va_lookup(handler_info->static_data_buffer_address);//转换静态数据缓冲区地址从物理地址到虚拟地址
		th->acpi_param_buffer_addr = efi_pa_va_lookup(handler_info->acpi_param_buffer_address);//转换ACPI参数缓冲区地址从物理地址到虚拟地址
	} while (++cur_handler < tm->handler_count && (handler_info = get_next_handler(handler_info)));

	return 0;//成功返回0

parse_prmt_out4:
	kfree(tm->mmio_info);//释放MMIO信息结构
parse_prmt_out3:
	memunmap(mmio_count);//解除MMIO计数映射
parse_prmt_out2:
	kfree(tm);//释放模块信息结构
parse_prmt_out1://返回内存不足错误
	return -ENOMEM;
}

#define GET_MODULE	0
#define GET_HANDLER	1

static void *find_guid_info(const guid_t *guid, u8 mode)
{
	struct prm_handler_info *cur_handler;
	struct prm_module_info *cur_module;
	int i = 0;

	list_for_each_entry(cur_module, &prm_module_list, module_list) {
		for (i = 0; i < cur_module->handler_count; ++i) {
			cur_handler = &cur_module->handlers[i];
			if (guid_equal(guid, &cur_handler->guid)) {
				if (mode == GET_MODULE)
					return (void *)cur_module;
				else
					return (void *)cur_handler;
			}
		}
	}

	return NULL;
}

static struct prm_module_info *find_prm_module(const guid_t *guid)
{
	return (struct prm_module_info *)find_guid_info(guid, GET_MODULE);
}

static struct prm_handler_info *find_prm_handler(const guid_t *guid)
{
	return (struct prm_handler_info *) find_guid_info(guid, GET_HANDLER);
}

/* In-coming PRM commands */

#define PRM_CMD_RUN_SERVICE		0
#define PRM_CMD_START_TRANSACTION	1
#define PRM_CMD_END_TRANSACTION		2

/* statuses that can be passed back to ASL */

#define PRM_HANDLER_SUCCESS 		0
#define PRM_HANDLER_ERROR 		1
#define INVALID_PRM_COMMAND 		2
#define PRM_HANDLER_GUID_NOT_FOUND 	3
#define UPDATE_LOCK_ALREADY_HELD 	4
#define UPDATE_UNLOCK_WITHOUT_LOCK 	5

/*
 * This is the PlatformRtMechanism opregion space handler.
 * @function: indicates the read/write. In fact as the PlatformRtMechanism
 * message is driven by command, only write is meaningful.
 *
 * @addr   : not used
 * @bits   : not used.
 * @value  : it is an in/out parameter. It points to the PRM message buffer.
 * @handler_context: not used
 */
static acpi_status acpi_platformrt_space_handler(u32 function,//处理ACPI平台运行时空间的回调函数，用于与PRM（Platform Runtime Management）交互
						 acpi_physical_address addr,
						 u32 bits, acpi_integer *value,
						 void *handler_context,
						 void *region_context)
{
	struct prm_buffer *buffer = ACPI_CAST_PTR(struct prm_buffer, value);//将value指针转换为prm_buffer结构体指针
	struct prm_handler_info *handler;//定义PRM处理程序信息指针
	struct prm_module_info *module;//定义PRM模块信息指针
	efi_status_t status;//定义EFI状态码变量
	struct prm_context_buffer context;//定义PRM上下文缓冲区结构体

	if (!efi_enabled(EFI_RUNTIME_SERVICES)) {//如果EFI运行时服务不可用
		pr_err_ratelimited("PRM: EFI runtime services no longer available\n");
		return AE_NO_HANDLER;
	}

	/*
	 * The returned acpi_status will always be AE_OK. Error values will be
	 * saved in the first byte of the PRM message buffer to be used by ASL.
	 */
	switch (buffer->prm_cmd) {//根据PRM命令类型执行不同分支
	case PRM_CMD_RUN_SERVICE://如果是执行服务命令

		handler = find_prm_handler(&buffer->handler_guid);//查找对应的PRM处理程序
		module = find_prm_module(&buffer->handler_guid);// 查找对应的PRM模块信息
		if (!handler || !module)//如果处理程序或模块信息不存在
			goto invalid_guid;//跳转到无效GUID处理标签

		ACPI_COPY_NAMESEG(context.signature, "PRMC");//将"PRMC"签名写入上下文结构体
		context.revision = 0x0;//设置上下文版本为0
		context.reserved = 0x0;//设置保留字段为0
		context.identifier = handler->guid;//设置处理程序唯一标识符
		context.static_data_buffer = handler->static_data_buffer_addr;//设置静态数据缓冲区地址
		context.mmio_ranges = module->mmio_info;//设置MMIO区域信息

		status = efi_call_acpi_prm_handler(handler->handler_addr,
						   handler->acpi_param_buffer_addr,
						   &context);// 调用EFI运行时处理程序
		if (status == EFI_SUCCESS) {//如果EFI调用成功
			buffer->prm_status = PRM_HANDLER_SUCCESS;//设置成功状态码
		} else {// 如果EFI调用失败
			buffer->prm_status = PRM_HANDLER_ERROR;//设置错误状态码
			buffer->efi_status = status;//保存原始EFI错误码
		}
		break;// 退出当前case分支

	case PRM_CMD_START_TRANSACTION://如果是开始事务命令

		module = find_prm_module(&buffer->handler_guid);//查找对应的PRM模块信息
		if (!module)//如果模块信息不存在
			goto invalid_guid;//跳转到无效GUID处理标签

		if (module->updatable)//如果模块处于可更新状态
			module->updatable = false;//锁定模块（设置为不可更新）
		else// 如果模块已被锁定
			buffer->prm_status = UPDATE_LOCK_ALREADY_HELD;// 设置锁已持有错误码
		break;

	case PRM_CMD_END_TRANSACTION://如果是结束事务命令

		module = find_prm_module(&buffer->handler_guid);//查找对应的PRM模块信息
		if (!module)
			goto invalid_guid;//如果模块信息不存在,跳转到无效GUID处理标签

		if (module->updatable)//如果模块未被锁定
			buffer->prm_status = UPDATE_UNLOCK_WITHOUT_LOCK;//设置未加锁解锁错误码
		else// 如果模块已被锁定
			module->updatable = true;//解锁模块（设置为可更新）
		break;

	default://如果是未知命令

		buffer->prm_status = INVALID_PRM_COMMAND;// 设置无效命令错误码
		break;
	}

	return AE_OK;//所有错误状态通过缓冲区返回，函数始终返回AE_OK

invalid_guid://无效GUID处理标签
	buffer->prm_status = PRM_HANDLER_GUID_NOT_FOUND;//设置GUID未找到错误码
	return AE_OK;//继续返回AE_OK但设置错误状态码
}
/**
 * init_prmt - 初始化平台运行时机制(PRMT)模块
 * 
 * 功能说明:
 * 1. 获取并解析ACPI PRMT表
 * 2. 检查EFI运行时服务可用性
 * 3. 安装ACPI地址空间处理程序
 * 
 * 注意:
 * - PRMT(Platform Runtime Mechanism Table)是ACPI规范的一部分
 */
void __init init_prmt(void)
{
	struct acpi_table_header *tbl;//存储ACPI表头指针
	acpi_status status;//ACPI操作状态
	int mc;//模块计数(Module Count)

	status = acpi_get_table(ACPI_SIG_PRMT, 0, &tbl);//获取PRMT ACPI表
	if (ACPI_FAILURE(status))//表不存在则直接返回
		return;

	/*
	 * 解析PRMT表中的子表条目:
	 * - 从表头之后开始解析(跳过PRMT表头和模块头),解析出来的所有子表都链接到prm_module_list链表
	 * - 对每个子表调用acpi_parse_prmt:解析子表信息到内核结构，包括子表处理程序
	 * - 返回找到的模块数量
	 * */
	mc = acpi_table_parse_entries(ACPI_SIG_PRMT, sizeof(struct acpi_table_prmt) +
					  sizeof (struct acpi_table_prmt_header),
					  0, acpi_parse_prmt, 0);
	acpi_put_table(tbl);//释放ACPI表引用
	/*
	 * Return immediately if PRMT table is not present or no PRM module found.
	 */
	if (mc <= 0)//如果没有找到任何模块则直接返回
		return;

	pr_info("PRM: found %u modules\n", mc);//打印找到的模块数量

	if (!efi_enabled(EFI_RUNTIME_SERVICES)) {// 检查EFI运行时服务是否可用
		pr_err("PRM: EFI runtime services unavailable\n");
		return;
	}

	/*
	 * 安装ACPI平台运行时地址空间处理程序:
	 *  - 处理ACPI_ADR_SPACE_PLATFORM_RT操作区域
	 *  - 使用acpi_platformrt_space_handler作为默认处理程序
	 * */
	status = acpi_install_address_space_handler(ACPI_ROOT_OBJECT,
						    ACPI_ADR_SPACE_PLATFORM_RT,
						    &acpi_platformrt_space_handler,
						    NULL, NULL);
	if (ACPI_FAILURE(status))
		pr_alert("PRM: OperationRegion handler could not be installed\n");
}
