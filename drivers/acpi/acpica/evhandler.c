// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: evhandler - Support for Address Space handlers
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acevents.h"
#include "acnamesp.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_EVENTS
ACPI_MODULE_NAME("evhandler")

/* Local prototypes */
static acpi_status
acpi_ev_install_handler(acpi_handle obj_handle,
			u32 level, void *context, void **return_value);

/* These are the address spaces that will get default handlers */

u8 acpi_gbl_default_address_spaces[ACPI_NUM_DEFAULT_SPACES] = {//存储ACPI默认支持的地址空间类型
	ACPI_ADR_SPACE_SYSTEM_MEMORY,//系统内存地址空间,允许ACPI访问物理内存
	ACPI_ADR_SPACE_SYSTEM_IO,//系统I/O端口地址空间,访问I/O端口（如传统PCI设备或ISA设备的寄存器）
	ACPI_ADR_SPACE_PCI_CONFIG,//PCI配置空间,访问PCI设备的配置寄存器（如BAR、中断线等）
	ACPI_ADR_SPACE_DATA_TABLE//数据表地址空间,访问ACPI表中的静态数据区域（如DSDT或SSDT中的预定义数据）
};

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_install_region_handlers
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Installs the core subsystem default address space handlers.
 *
 ******************************************************************************/

acpi_status acpi_ev_install_region_handlers(void)//安装ACPI默认的地址空间处理程序（如内存、I/O、PCI配置空间）
{
	acpi_status status;
	u32 i;

	ACPI_FUNCTION_TRACE(ev_install_region_handlers);

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);//命名空间互斥锁，确保在安装处理程序时没有其他线程修改命名空间。
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/*
	 * All address spaces (PCI Config, EC, SMBus) are scope dependent and
	 * registration must occur for a specific device.
	 *
	 * In the case of the system memory and IO address spaces there is
	 * currently no device associated with the address space. For these we
	 * use the root.
	 *
	 * We install the default PCI config space handler at the root so that
	 * this space is immediately available even though the we have not
	 * enumerated all the PCI Root Buses yet. This is to conform to the ACPI
	 * specification which states that the PCI config space must be always
	 * available -- even though we are nowhere near ready to find the PCI root
	 * buses at this point.
	 *
	 * NOTE: We ignore AE_ALREADY_EXISTS because this means that a handler
	 * has already been installed (via acpi_install_address_space_handler).
	 * Similar for AE_SAME_HANDLER.
	 */
	for (i = 0; i < ACPI_NUM_DEFAULT_SPACES; i++) {//循环安装所有默认地址空间处理程序
		status = acpi_ev_install_space_handler(acpi_gbl_root_node,//命名空间根节点（\_SB）
						       acpi_gbl_default_address_spaces
						       [i],//默认地址空间类型数组
						       ACPI_DEFAULT_HANDLER,//表明安装默认处理函数指针
						       NULL, NULL);//
		switch (status) {//处理状态码
		case AE_OK://安装成功
		case AE_SAME_HANDLER://已存在相同处理程序，无需重复安装
		case AE_ALREADY_EXISTS://已有其他处理程序（可能由用户自定义安装），忽略错误。

			/* These exceptions are all OK */

			status = AE_OK;
			break;

		default:

			goto unlock_and_exit;//致命错误：如内存不足或无效地址空间类型，跳转到unlock_and_exit释放锁并返回错误。
		}
	}

unlock_and_exit:
	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);//释放互斥锁
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_has_default_handler
 *
 * PARAMETERS:  node                - Namespace node for the device
 *              space_id            - The address space ID
 *
 * RETURN:      TRUE if default handler is installed, FALSE otherwise
 *
 * DESCRIPTION: Check if the default handler is installed for the requested
 *              space ID.
 *
 ******************************************************************************/

u8
acpi_ev_has_default_handler(struct acpi_namespace_node *node,
			    acpi_adr_space_type space_id)
{
	union acpi_operand_object *obj_desc;
	union acpi_operand_object *handler_obj;

	/* Must have an existing internal object */

	obj_desc = acpi_ns_get_attached_object(node);
	if (obj_desc) {
		handler_obj = obj_desc->common_notify.handler;

		/* Walk the linked list of handlers for this object */

		while (handler_obj) {
			if (handler_obj->address_space.space_id == space_id) {
				if (handler_obj->address_space.handler_flags &
				    ACPI_ADDR_HANDLER_DEFAULT_INSTALLED) {
					return (TRUE);
				}
			}

			handler_obj = handler_obj->address_space.next;
		}
	}

	return (FALSE);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_install_handler
 *
 * PARAMETERS:  walk_namespace callback
 *
 * DESCRIPTION: This routine installs an address handler into objects that are
 *              of type Region or Device.
 *
 *              If the Object is a Device, and the device has a handler of
 *              the same type then the search is terminated in that branch.
 *
 *              This is because the existing handler is closer in proximity
 *              to any more regions than the one we are trying to install.
 *
 ******************************************************************************/
/* 在命名空间中安装地址空间处理程序的回调函数 */
static acpi_status
acpi_ev_install_handler(acpi_handle obj_handle,
			u32 level, void *context, void **return_value)
{
	union acpi_operand_object *handler_obj;//用户提供的处理程序对象
	union acpi_operand_object *next_handler_obj;//用于检查现有处理程序
	union acpi_operand_object *obj_desc;//节点的内部对象描述符
	struct acpi_namespace_node *node;//当前命名空间节点
	acpi_status status;//函数返回状态

	ACPI_FUNCTION_NAME(ev_install_handler);

	handler_obj = (union acpi_operand_object *)context;//从上下文中获取处理程序对象

	/* Parameter validation */

	if (!handler_obj) {//无处理程序对象 → 无需操作
		return (AE_OK);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_validate_handle(obj_handle);//将句柄转换为命名空间节点指针
	if (!node) {
		return (AE_BAD_PARAMETER);//无效句柄 → 返回错误
	}

	/*
	 * We only care about regions and objects that are allowed to have
	 * address space handlers
	 */
	/* 只处理特定类型节点 */
	if ((node->type != ACPI_TYPE_DEVICE) &&//如果不是设备节点
	    (node->type != ACPI_TYPE_REGION) && (node != acpi_gbl_root_node)) {// 也不是区域节点和根节点
		return (AE_OK);// 其他类型直接跳过
	}

	/* Check for an existing internal object */
	/* 获取节点的内部对象 */
	obj_desc = acpi_ns_get_attached_object(node);//获取节点的ACPI对象
	if (!obj_desc) {

		/* No object, just exit */

		return (AE_OK);
	}

	/* 处理设备节点 */
	if (obj_desc->common.type == ACPI_TYPE_DEVICE) {

		/* Check if this Device already has a handler for this address space */
		/* 检查设备是否已有同类型地址空间处理程序 */
		next_handler_obj =
		    acpi_ev_find_region_handler(handler_obj->address_space.space_id,//当前处理程序的地址空间类型
						obj_desc->common_notify.handler);//对象现有的通知处理程序
		if (next_handler_obj) {//已存在同类型处理程序

			/* Found a handler, is it for the same address space? */

			ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
					  "Found handler for region [%s] in device %p(%p) handler %p\n",
					  acpi_ut_get_region_name(handler_obj->
								  address_space.
								  space_id),
					  obj_desc, next_handler_obj,
					  handler_obj));

			/*
			 * Since the object we found it on was a device, then it means
			 * that someone has already installed a handler for the branch
			 * of the namespace from this device on. Just bail out telling
			 * the walk routine to not traverse this branch. This preserves
			 * the scoping rule for handlers.a
			 * 根据ACPI命名空间规则，设备层级已存在相同地址空间的处理程序，
			 * 后续子节点无需处理 → 返回AE_CTRL_DEPTH阻止继续遍历该分支
			 */
			return (AE_CTRL_DEPTH);
		}

		/*
		 * As long as the device didn't have a handler for this space we
		 * don't care about it. We just ignore it and proceed.
		 */
		return (AE_OK);//未找到处理程序 → 直接返回成功，继续遍历
	}

	/* Object is a Region */
	/* 处理区域节点 */
	if (obj_desc->region.space_id != handler_obj->address_space.space_id) {//区域类型不匹配 → 跳过

		/* This region is for a different address space, just ignore it */

		return (AE_OK);
	}

	/*
	 * Now we have a region and it is for the handler's address space type.
	 *
	 * First disconnect region for any previous handler (if any)
	 *
	 * 匹配区域 → 断开原有处理程序并连接新处理程序
	 */
	acpi_ev_detach_region(obj_desc, FALSE);//断开现有关联

	/* Connect the region to the new handler */

	status = acpi_ev_attach_region(handler_obj, obj_desc, FALSE);//连接新处理程序
	return (status);//返回操作结果
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_find_region_handler
 *
 * PARAMETERS:  space_id        - The address space ID
 *              handler_obj     - Head of the handler object list
 *
 * RETURN:      Matching handler object. NULL if space ID not matched
 *
 * DESCRIPTION: Search a handler object list for a match on the address
 *              space ID.
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ev_find_region_handler(acpi_adr_space_type
						       space_id,
						       union acpi_operand_object
						       *handler_obj)
{

	/* Walk the handler list for this device */

	while (handler_obj) {

		/* Same space_id indicates a handler is installed */

		if (handler_obj->address_space.space_id == space_id) {
			return (handler_obj);
		}

		/* Next handler object */

		handler_obj = handler_obj->address_space.next;
	}

	return (NULL);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_install_space_handler
 *
 * PARAMETERS:  node            - Namespace node for the device
 *              space_id        - The address space ID
 *              handler         - Address of the handler
 *              setup           - Address of the setup function
 *              context         - Value passed to the handler on each access
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install a handler for all op_regions of a given space_id.
 *              Assumes namespace is locked
 *
 ******************************************************************************/
/**
 * acpi_ev_install_space_handler - 安装ACPI地址空间处理程序
 * @node: 命名空间节点(设备/处理器/热区/根节点)
 * @space_id: 地址空间类型(ACPI_ADR_SPACE_*)
 * @handler: 地址空间访问处理函数
 * @setup: 区域初始化函数
 * @context: 传递给处理程序的上下文数据
 *
 * 返回值:
 *   AE_OK - 安装成功
 *   AE_BAD_PARAMETER - 参数无效
 *   AE_ALREADY_EXISTS - 已存在不同处理程序
 *   AE_SAME_HANDLER - 已存在相同处理程序
 *   AE_NO_MEMORY - 内存分配失败
 *
 * 功能说明:
 * 1. 验证节点类型有效性
 * 2. 处理默认处理程序情况
 * 3. 创建/获取设备对象
 * 4. 检查处理程序是否已存在
 * 5. 创建并初始化处理程序对象
 * 6. 遍历命名空间安装处理程序
 */
acpi_status
acpi_ev_install_space_handler(struct acpi_namespace_node *node,
			      acpi_adr_space_type space_id,
			      acpi_adr_space_handler handler,
			      acpi_adr_space_setup setup, void *context)
{
	union acpi_operand_object *obj_desc;//设备对象描述符
	union acpi_operand_object *handler_obj;//地址空间处理程序对象
	acpi_status status = AE_OK;
	acpi_object_type type;//对象类型
	u8 flags = 0;//处理程序标志

	ACPI_FUNCTION_TRACE(ev_install_space_handler);

	/*
	 * 此注册仅对以下类型及根节点有效。根节点是默认处理程序安装的位置。
	 */
	if ((node->type != ACPI_TYPE_DEVICE) &&//不是设备节点
	    (node->type != ACPI_TYPE_PROCESSOR) &&//不是处理器节点
	    (node->type != ACPI_TYPE_THERMAL) && (node != acpi_gbl_root_node)) {//不是热区节点也不是根节点
		status = AE_BAD_PARAMETER;//设置错误状态为无效参数
		goto unlock_and_exit;//跳转到统一的清理退出点
	}

	/* 当handler参数为ACPI_DEFAULT_HANDLER时，根据地址空间类型(space_id)
	 * 设置对应的默认处理函数和区域初始化函数。
	 * */
	if (handler == ACPI_DEFAULT_HANDLER) {
		flags = ACPI_ADDR_HANDLER_DEFAULT_INSTALLED;//标志表示这是默认处理程序

		switch (space_id) {//根据空间类型设置默认处理程序
		case ACPI_ADR_SPACE_SYSTEM_MEMORY://系统内存空间处理

			handler = acpi_ex_system_memory_space_handler;//系统内存访问处理函数
			setup = acpi_ev_system_memory_region_setup;//内存区域的激活/停用处理
			break;

		case ACPI_ADR_SPACE_SYSTEM_IO://系统I/O空间处理

			handler = acpi_ex_system_io_space_handler;//I/O端口访问处理函数
			setup = acpi_ev_io_space_region_setup;//I/O 地址空间区域（OpRegion）的激活/停用处理
			break;
#ifdef ACPI_PCI_CONFIGURED
		case ACPI_ADR_SPACE_PCI_CONFIG://PCI配置空间处理

			handler = acpi_ex_pci_config_space_handler;// PCI配置空间处理函数
			setup = acpi_ev_pci_config_region_setup;//PCI区域初始化函数
			break;
#endif
		case ACPI_ADR_SPACE_CMOS:// CMOS空间处理

			handler = acpi_ex_cmos_space_handler;//CMOS访问处理函数
			setup = acpi_ev_cmos_region_setup;//CMOS区域初始化函数
			break;
#ifdef ACPI_PCI_CONFIGURED
		case ACPI_ADR_SPACE_PCI_BAR_TARGET://PCI BAR目标空间处理

			handler = acpi_ex_pci_bar_space_handler;//PCI BAR空间处理函数
			setup = acpi_ev_pci_bar_region_setup;//PCI BAR区域初始化函数
			break;
#endif
		case ACPI_ADR_SPACE_DATA_TABLE:// ACPI数据表空间处理

			handler = acpi_ex_data_table_space_handler;//数据表访问处理函数
			setup = acpi_ev_data_table_region_setup;//数据表区域初始化函数
			break;

		default://不支持的地址空间类型

			status = AE_BAD_PARAMETER;//设置无效参数错误状态
			goto unlock_and_exit;//跳转到错误处理路径
		}
	}

	/* 如果调用者未指定设置程序，则使用默认设置
	 * 当调用者未提供setup函数时，使用默认的acpi_ev_default_region_setup
	 * 作为兜底。这个默认函数会执行基本的区域初始化工作。
	 * */

	if (!setup) {
		setup = acpi_ev_default_region_setup;//设置为默认区域初始化回调
	}

	/* 
	 * 检查并处理ACPI内部对象
	 * 这部分代码处理与命名空间节点关联的内部对象(Operand Object)，
	 * 确保后续可以安全地安装地址空间处理程序。
	 */

	obj_desc = acpi_ns_get_attached_object(node);//获取节点关联的内部对象
	if (obj_desc) {
		/*
		 * 情况1：内部对象已存在，需要检查是否已安装相同space_id的处理程序
		 */
		handler_obj = acpi_ev_find_region_handler(space_id,
							  obj_desc->
							  common_notify.
							  handler);//从公共通知区域获取当前处理程序链

		if (handler_obj) {
			if (handler_obj->address_space.handler == handler) {//检查是否尝试安装完全相同的处理程序
				/*
				 * It is (relatively) OK to attempt to install the SAME
				 * handler twice. This can easily happen with the
				 * PCI_Config space.
				 * 特殊情况：重复安装相同处理程序,这在PCI配置空间等场景
				 * 可能发生，不算错误但需要特殊处理
				 */
				status = AE_SAME_HANDLER;//设置特殊状态码
				goto unlock_and_exit;//跳转到统一的清理退出点
			} else {
				/* 常规情况：已存在不同处理程序 */

				status = AE_ALREADY_EXISTS;//设置冲突错误码
			}

			goto unlock_and_exit;//跳转到清理退出点
		}
	} else {//情况2：内部对象不存在,需要创建新的内部对象并附加到节点
		ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
				  "Creating object on Device %p while installing handler\n",
				  node));

		/* obj_desc 不存在，创建一个 */

		if (node->type == ACPI_TYPE_ANY) {
			type = ACPI_TYPE_DEVICE;//将ANY类型默认为DEVICE
		} else {
			type = node->type;
		}

		obj_desc = acpi_ut_create_internal_object(type);//创建新的内部对象
		if (!obj_desc) {
			status = AE_NO_MEMORY;//内存分配失败
			goto unlock_and_exit;//跳转到错误处理
		}

		/* Init new descriptor */

		obj_desc->common.type = (u8)type;//初始化对象类型字段

		/* Attach the new object to the Node */

		status = acpi_ns_attach_object(node, obj_desc, type);//将对象附加到命名空间节点

		/* 移除对象的本地表引用 */

		acpi_ut_remove_reference(obj_desc);

		if (ACPI_FAILURE(status)) {
			goto unlock_and_exit;//附加失败时跳转
		}
	}

	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "Installing address handler for region %s(%X) "
			  "on Device %4.4s %p(%p)\n",
			  acpi_ut_get_region_name(space_id), space_id,
			  acpi_ut_get_node_name(node), node, obj_desc));

	/*
	 * Install the handler
	 *
	 * At this point there is no existing handler. Just allocate the object
	 * for the handler and link it into the list.
	 */
	/*
 	 * 创建地址空间处理程序对象
	 *
	 * 使用ACPI_TYPE_LOCAL_ADDRESS_HANDLER类型创建专用对象，
	 * 用于管理该地址空间的所有区域访问。
	 */
	handler_obj =
	    acpi_ut_create_internal_object(ACPI_TYPE_LOCAL_ADDRESS_HANDLER);
	if (!handler_obj) {
		status = AE_NO_MEMORY;//内存分配失败
		goto unlock_and_exit;//跳转到清理退出点
	}

	/* 初始化处理程序对象 */
	/* 创建上下文互斥锁 - 保护处理程序并发访问 */
	status =
	    acpi_os_create_mutex(&handler_obj->address_space.context_mutex);
	if (ACPI_FAILURE(status)) {
		acpi_ut_remove_reference(handler_obj);//释放处理程序对象
		goto unlock_and_exit;
	}

	/* 设置处理程序对象属性 */
	handler_obj->address_space.space_id = (u8)space_id;//地址空间类型
	handler_obj->address_space.handler_flags = flags;//处理程序标志
	handler_obj->address_space.region_list = NULL;//初始化区域链表
	handler_obj->address_space.node = node;//关联的命名空间节点
	handler_obj->address_space.handler = handler;//访问处理函数
	handler_obj->address_space.context = context;//处理程序上下文
	handler_obj->address_space.setup = setup;//区域初始化函数

	/* 将处理程序插入设备处理程序链表头部 */

	handler_obj->address_space.next = obj_desc->common_notify.handler;//链表中下一个

	/*
	 * _obj设备对象是handler_obj上的第一个引用。每个使用处理器的区域
	 * 都会添加一个引用。
	 */
	obj_desc->common_notify.handler = handler_obj;//更新链表头指针

	/*
	 * Walk the namespace finding all of the regions this handler will
	 * manage.
	 *
	 * Start at the device and search the branch toward the leaf nodes
	 * until either the leaf is encountered or a device is detected that
	 * has an address handler of the same type.
	 *
	 * In either case, back up and search down the remainder of the branch
	 */
	/*
	 * 遍历命名空间关联区域
	 *
	 * 从当前节点开始深度优先搜索，找到所有需要该处理程序管理的区域。
	 * acpi_ev_install_handler回调函数会将区域添加到处理程序的region_list。
	 */
	status = acpi_ns_walk_namespace(ACPI_TYPE_ANY, node,
					ACPI_UINT32_MAX, ACPI_NS_WALK_UNLOCK,
					acpi_ev_install_handler, NULL,
					handler_obj, NULL);

unlock_and_exit://统一的清理退出点
	return_ACPI_STATUS(status);//返回操作状态
}
