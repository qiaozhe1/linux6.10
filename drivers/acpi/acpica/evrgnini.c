// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: evrgnini- ACPI address_space (op_region) init
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
ACPI_MODULE_NAME("evrgnini")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_system_memory_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a system_memory operation region
 *
 ******************************************************************************/
/* 系统内存区域的初始化和释放函数 
 * ACPI系统内存区域的核心管理函数，负责在区域激活时分配上下文并记录物理地址，在去激活时释放所有资源。
 * */
acpi_status
acpi_ev_system_memory_region_setup(acpi_handle handle,//指向ACPI命名空间节点的句柄
				   u32 function,//操作类型（ACPI_REGION_ACTIVATE或ACPI_REGION_DEACTIVATE）。
				   void *handler_context, void **region_context)//区域处理程序的上下文（未使用）;指向区域上下文的指针，用于存储内存映射信息
{
	union acpi_operand_object *region_desc =
	    (union acpi_operand_object *)handle;//将handle转换为区域对象
	struct acpi_mem_space_context *local_region_context;
	struct acpi_mem_mapping *mm;//内存映射关系

	ACPI_FUNCTION_TRACE(ev_system_memory_region_setup);

	if (function == ACPI_REGION_DEACTIVATE) {//如果是去激活
		if (*region_context) {
			local_region_context =
			    (struct acpi_mem_space_context *)*region_context;

			/* Delete memory mappings if present */

			while (local_region_context->first_mm) {//遍历first_mm链表，逐个解除映射并释放内存
				mm = local_region_context->first_mm;
				local_region_context->first_mm = mm->next_mm;
				acpi_os_unmap_memory(mm->logical_address,
						     mm->length);//解除虚拟地址映射
				ACPI_FREE(mm);//释放struct acpi_mem_mapping结构体
			}
			ACPI_FREE(local_region_context);//释放区域上下文
			*region_context = NULL;//标记为空，表示资源已释放
		}
		return_ACPI_STATUS(AE_OK);
	}

	/* Create a new context */
	/* 激活状态：分配上下文 */
	local_region_context =
	    ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_mem_space_context));//分配并初始化区域上下文
	if (!(local_region_context)) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Save the region length and address for use in the handler */
	/* 记录区域的长度和物理地址 */
	local_region_context->length = region_desc->region.length;
	local_region_context->address = region_desc->region.address;

	*region_context = local_region_context;//返回上下文
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_io_space_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a IO operation region
 *
 ******************************************************************************/
/* I/O 地址空间区域（OpRegion）的激活/停用处理 */
acpi_status
acpi_ev_io_space_region_setup(acpi_handle handle,
			      u32 function,
			      void *handler_context, void **region_context)
{
	ACPI_FUNCTION_TRACE(ev_io_space_region_setup);

	if (function == ACPI_REGION_DEACTIVATE) {//如果是区域停用操作
		*region_context = NULL;//清空区域上下文
	} else {//如果是区域激活操作
		*region_context = handler_context;//传递处理程序上下文到区域上下文
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_pci_config_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a PCI_Config operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/
/* 
 * acpi_ev_pci_config_region_setup - PCI配置空间操作区域(OpRegion)的初始化/释放
 * @handle:        ACPI操作区域对象句柄
 * @function:      操作类型（ACPI_REGION_ACTIVATE/DEACTIVATE）
 * @handler_context: 处理程序上下文（未使用）
 * @region_context:  区域上下文指针的指针（输入输出参数）
 * 
 * 返回值:
 *  AE_OK          - 操作成功
 *  AE_NO_MEMORY   - 内存分配失败
 *  AE_NOT_EXIST   - 未找到处理程序
 *  AE_AML_OPERAND_TYPE - 无效的操作数类型
 * 
 * 功能描述:
 *  1. 处理PCI配置空间的激活/停用操作
 *  2. 激活时构建PCI ID信息（段/总线/设备/功能号）
 *  3. 自动检测并关联PCI根桥处理程序
 */
acpi_status
acpi_ev_pci_config_region_setup(acpi_handle handle,
				u32 function,
				void *handler_context, void **region_context)
{
	acpi_status status = AE_OK;
	u64 pci_value;
	struct acpi_pci_id *pci_id = *region_context;//PCI ID结构指针
	union acpi_operand_object *handler_obj;//处理程序对象
	struct acpi_namespace_node *parent_node;//父节点指针
	struct acpi_namespace_node *pci_root_node;//PCI根桥节点
	struct acpi_namespace_node *pci_device_node;//PCI设备节点
	union acpi_operand_object *region_obj =
	    (union acpi_operand_object *)handle;//转换句柄为区域对象

	ACPI_FUNCTION_TRACE(ev_pci_config_region_setup);

	handler_obj = region_obj->region.handler;//获取区域对象关联的处理程序对象
	if (!handler_obj) {
		/*
		 * No installed handler. This shouldn't happen because the dispatch
		 * routine checks before we get here, but we check again just in case.
		 */
		ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
				  "Attempting to init a region %p, with no handler\n",
				  region_obj));
		return_ACPI_STATUS(AE_NOT_EXIST);
	}

	*region_context = NULL;//初始化上下文指针
	if (function == ACPI_REGION_DEACTIVATE) {//如果是区域停用请求
		if (pci_id) {//如果存在已分配的PCI ID结构
			ACPI_FREE(pci_id);//释放内存
		}
		return_ACPI_STATUS(status);
	}

	/* 激活处理开始 */

	parent_node = region_obj->region.node->parent;//获取区域对象的父节点

	/*
	 * 从安装了处理程序的设备上获取 _SEG 和 _BBN 的值。
	 * 我们需要获取相对于PCI总线设备 的 _SEG 和 _BBN 对象。这是处理程序已经注册
	 * 来处理的设备。
	 */

	/*
	 * 如果address_space.Node仍然指向根节点，我们需要向上扫描寻找PCI根桥，并
	 * 将op_region处理程序重新关联到该设备。
	 */
	if (handler_obj->address_space.node == acpi_gbl_root_node) {// 如果当前处理程序挂载在根节点

		/* Start search from the parent object */

		pci_root_node = parent_node;//从父对象开始搜索
		while (pci_root_node != acpi_gbl_root_node) {//向上遍历命名空间直到根节点

			/* Get the _HID/_CID in order to detect a root_bridge */

			if (acpi_ev_is_pci_root_bridge(pci_root_node)) {// 检测是否为PCI根桥节点（通过_HID/_CID）

				/* Install a handler for this PCI root bridge */

				status = acpi_install_address_space_handler((acpi_handle)pci_root_node, ACPI_ADR_SPACE_PCI_CONFIG, ACPI_DEFAULT_HANDLER, NULL, NULL);//为此根桥安装默认PCI配置空间处理程序
				if (ACPI_FAILURE(status)) {//处理安装结果
					if (status == AE_SAME_HANDLER) {
						/*
						 * It is OK if the handler is already installed on the
						 * root bridge. Still need to return a context object
						 * for the new PCI_Config operation region, however.
						 */
					} else {
						ACPI_EXCEPTION((AE_INFO, status,
								"Could not install PciConfig handler "
								"for Root Bridge %4.4s",
								acpi_ut_get_node_name
								(pci_root_node)));
					}
				}
				break;//找到根桥后退出循环
			}

			pci_root_node = pci_root_node->parent;//继续向上层搜索
		}

		/* PCI root bridge not found, use namespace root node */
	} else {//处理程序已关联到非根节点
		pci_root_node = handler_obj->address_space.node;//处理程序所在节点作为根节点
	}

	/*
	 * If this region is now initialized, we are done.
	 * (install_address_space_handler could have initialized it)
	 */
	if (region_obj->region.flags & AOPOBJ_SETUP_COMPLETE) {//检查区域是否已完成初始化
		return_ACPI_STATUS(AE_OK);
	}

	/* Region is still not initialized. Create a new context */

	pci_id = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_pci_id));//分配并清零PCI ID结构内存
	if (!pci_id) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/*
	 * For PCI_Config space access, we need the segment, bus, device and
	 * function numbers. Acquire them here.
	 *
	 * Find the parent device object. (This allows the operation region to be
	 * within a subscope under the device, such as a control method.)
	 */
	pci_device_node = region_obj->region.node;//获取当前区域的节点
	while (pci_device_node && (pci_device_node->type != ACPI_TYPE_DEVICE)) {
		pci_device_node = pci_device_node->parent;// 向上遍历直到设备节点
	}

	if (!pci_device_node) {//如果未找到设备节点
		ACPI_FREE(pci_id);//释放pci_id结构内存
		return_ACPI_STATUS(AE_AML_OPERAND_TYPE);//返回操作数类型错误
	}

	/*
	 * Get the PCI device and function numbers from the _ADR object
	 * contained in the parent's scope.
	 */
	status = acpi_ut_evaluate_numeric_object(METHOD_NAME__ADR,
						 pci_device_node, &pci_value);//从设备节点的_ADR对象获取设备/功能号

	/*
	 * The default is zero, and since the allocation above zeroed the data,
	 * just do nothing on failure.
	 */
	if (ACPI_SUCCESS(status)) {//如果成功获取
		pci_id->device = ACPI_HIWORD(ACPI_LODWORD(pci_value));//提取高16位为设备号
		pci_id->function = ACPI_LOWORD(ACPI_LODWORD(pci_value));//提取低16位为功能号
	}

	/* The PCI segment number comes from the _SEG method */

	status = acpi_ut_evaluate_numeric_object(METHOD_NAME__SEG,
						 pci_root_node, &pci_value);//从根桥节点获取段号(_SEG方法)
	if (ACPI_SUCCESS(status)) {//如果成功获取
		pci_id->segment = ACPI_LOWORD(pci_value);//取低16位作为段号
	}

	/* The PCI bus number comes from the _BBN method */

	status = acpi_ut_evaluate_numeric_object(METHOD_NAME__BBN,
						 pci_root_node, &pci_value);//从根桥节点获取总线号(_BBN方法)
	if (ACPI_SUCCESS(status)) {//如果成功获取
		pci_id->bus = ACPI_LOWORD(pci_value);//取低16位作为总线号
	}

	/* Complete/update the PCI ID for this device */

	status =
	    acpi_hw_derive_pci_id(pci_id, pci_root_node,
				  region_obj->region.node);//校验并完善PCI ID信息
	if (ACPI_FAILURE(status)) {//如果失败
		ACPI_FREE(pci_id);//释放已分配内存
		return_ACPI_STATUS(status);
	}

	*region_context = pci_id;//将PCI ID结构传递回调用者
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_is_pci_root_bridge
 *
 * PARAMETERS:  node            - Device node being examined
 *
 * RETURN:      TRUE if device is a PCI/PCI-Express Root Bridge
 *
 * DESCRIPTION: Determine if the input device represents a PCI Root Bridge by
 *              examining the _HID and _CID for the device.
 *
 ******************************************************************************/

u8 acpi_ev_is_pci_root_bridge(struct acpi_namespace_node *node)
{
	acpi_status status;
	struct acpi_pnp_device_id *hid;
	struct acpi_pnp_device_id_list *cid;
	u32 i;
	u8 match;

	/* Get the _HID and check for a PCI Root Bridge */

	status = acpi_ut_execute_HID(node, &hid);
	if (ACPI_FAILURE(status)) {
		return (FALSE);
	}

	match = acpi_ut_is_pci_root_bridge(hid->string);
	ACPI_FREE(hid);

	if (match) {
		return (TRUE);
	}

	/* The _HID did not match. Get the _CID and check for a PCI Root Bridge */

	status = acpi_ut_execute_CID(node, &cid);
	if (ACPI_FAILURE(status)) {
		return (FALSE);
	}

	/* Check all _CIDs in the returned list */

	for (i = 0; i < cid->count; i++) {
		if (acpi_ut_is_pci_root_bridge(cid->ids[i].string)) {
			ACPI_FREE(cid);
			return (TRUE);
		}
	}

	ACPI_FREE(cid);
	return (FALSE);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_pci_bar_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a pci_BAR operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/

acpi_status
acpi_ev_pci_bar_region_setup(acpi_handle handle,
			     u32 function,
			     void *handler_context, void **region_context)
{
	ACPI_FUNCTION_TRACE(ev_pci_bar_region_setup);

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_cmos_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a CMOS operation region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/

acpi_status
acpi_ev_cmos_region_setup(acpi_handle handle,
			  u32 function,
			  void *handler_context, void **region_context)
{
	ACPI_FUNCTION_TRACE(ev_cmos_region_setup);

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_data_table_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Setup a data_table_region
 *
 * MUTEX:       Assumes namespace is not locked
 *
 ******************************************************************************/
/* 
 * acpi_ev_data_table_region_setup - 数据表区域（Data Table Region）的激活/停用处理函数
 * @handle:        操作区域对象句柄（ACPI内部对象）
 * @function:      操作类型（ACPI_REGION_ACTIVATE 或 ACPI_REGION_DEACTIVATE）
 * @handler_context: 未使用（保留参数）
 * @region_context: 输入输出参数，传递区域上下文指针的指针
 *
 * 返回值:
 *  AE_OK           - 操作成功
 *  AE_NO_MEMORY    - 内存分配失败
 *
 * 功能描述:
 *  1. 处理数据表区域的激活与停用：
 *     - 激活时分配上下文并保存数据表指针
 *     - 停用时释放上下文资源
 *  2. 上下文结构 acpi_data_table_mapping 用于在访问区域时定位数据表
 */
acpi_status
acpi_ev_data_table_region_setup(acpi_handle handle,
				u32 function,
				void *handler_context, void **region_context)
{
	union acpi_operand_object *region_desc =
	    (union acpi_operand_object *)handle;//转换句柄为操作区域对象
	struct acpi_data_table_mapping *local_region_context;//本地上下文指针

	ACPI_FUNCTION_TRACE(ev_data_table_region_setup);

	if (function == ACPI_REGION_DEACTIVATE) {//如果是区域停用请求
		if (*region_context) {// 检查是否存在已分配的上下文
			ACPI_FREE(*region_context);//释放内存
			*region_context = NULL;
		}
		return_ACPI_STATUS(AE_OK);
	}

	/* Create a new context */
	/* 激活处理：分配并初始化上下文结构 */
	local_region_context =
	    ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_data_table_mapping));//分配并清零内存
	if (!(local_region_context)) {//检查内存分配是否成功
		return_ACPI_STATUS(AE_NO_MEMORY);//返回内存不足错误
	}

	/* Save the data table pointer for use in the handler */
	/* 保存数据表指针，以便在处理程序中使用 */
	local_region_context->pointer = region_desc->region.pointer;//从区域对象获取数据表地址

	*region_context = local_region_context;//将新上下文返回
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_default_region_setup
 *
 * PARAMETERS:  handle              - Region we are interested in
 *              function            - Start or stop
 *              handler_context     - Address space handler context
 *              region_context      - Region specific context
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Default region initialization
 *
 ******************************************************************************/
/* 
 * acpi_ev_default_region_setup - ACPI默认区域（OpRegion）激活/停用处理函数
 * @handle:        ACPI操作区域对象句柄（未使用）
 * @function:      操作类型（ACPI_REGION_ACTIVATE/DEACTIVATE）
 * @handler_context: 处理程序上下文（通常为区域访问方法指针）
 * @region_context: 输入输出参数，用于存储区域上下文指针
 *
 * 返回值:
 *  AE_OK           - 始终返回成功
 *
 * 功能描述:
 *  该函数为不需要特殊初始化/清理的ACPI区域提供默认设置逻辑：
 *   1. 激活时直接传递handler_context到region_context
 *   2. 停用时清空region_context指针
 *  适用于以下地址空间类型：
 *   - SystemMemory/SystemIO（已由更专用处理程序覆盖）
 *   - PCI_Config（独立处理程序）
 *   - EC（Embedded Controller）等需要简单上下文传递的区域
 *
 * 设计要点:
 *  - 不涉及内存分配/释放，依赖外部管理handler_context生命周期
 *  - 作为通用回退机制，适用于无特殊需求的区域类型
 */
acpi_status
acpi_ev_default_region_setup(acpi_handle handle,
			     u32 function,
			     void *handler_context, void **region_context)
{
	ACPI_FUNCTION_TRACE(ev_default_region_setup);

	if (function == ACPI_REGION_DEACTIVATE) {//如果是区域停用请求
		*region_context = NULL;//清空上下文指针
	} else {//区域激活请求
		*region_context = handler_context;//直接传递预定义上下文
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_initialize_region
 *
 * PARAMETERS:  region_obj      - Region we are initializing
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initializes the region, finds any _REG methods and saves them
 *              for execution at a later time
 *
 *              Get the appropriate address space handler for a newly
 *              created region.
 *
 *              This also performs address space specific initialization. For
 *              example, PCI regions must have an _ADR object that contains
 *              a PCI address in the scope of the definition. This address is
 *              required to perform an access to PCI config space.
 *
 * MUTEX:       Interpreter should be unlocked, because we may run the _REG
 *              method for this region.
 *
 * NOTE:        Possible incompliance:
 *              There is a behavior conflict in automatic _REG execution:
 *              1. When the interpreter is evaluating a method, we can only
 *                 automatically run _REG for the following case:
 *                   operation_region (OPR1, 0x80, 0x1000010, 0x4)
 *              2. When the interpreter is loading a table, we can also
 *                 automatically run _REG for the following case:
 *                   operation_region (OPR1, 0x80, 0x1000010, 0x4)
 *              Though this may not be compliant to the de-facto standard, the
 *              logic is kept in order not to trigger regressions. And keeping
 *              this logic should be taken care by the caller of this function.
 *
 ******************************************************************************/

acpi_status acpi_ev_initialize_region(union acpi_operand_object *region_obj)
{
	union acpi_operand_object *handler_obj;
	union acpi_operand_object *obj_desc;
	acpi_adr_space_type space_id;
	struct acpi_namespace_node *node;

	ACPI_FUNCTION_TRACE(ev_initialize_region);

	if (!region_obj) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	if (region_obj->common.flags & AOPOBJ_OBJECT_INITIALIZED) {
		return_ACPI_STATUS(AE_OK);
	}

	region_obj->common.flags |= AOPOBJ_OBJECT_INITIALIZED;

	node = region_obj->region.node->parent;
	space_id = region_obj->region.space_id;

	/*
	 * The following loop depends upon the root Node having no parent
	 * ie: acpi_gbl_root_node->Parent being set to NULL
	 */
	while (node) {

		/* Check to see if a handler exists */

		handler_obj = NULL;
		obj_desc = acpi_ns_get_attached_object(node);
		if (obj_desc) {

			/* Can only be a handler if the object exists */

			switch (node->type) {
			case ACPI_TYPE_DEVICE:
			case ACPI_TYPE_PROCESSOR:
			case ACPI_TYPE_THERMAL:

				handler_obj = obj_desc->common_notify.handler;
				break;

			default:

				/* Ignore other objects */

				break;
			}

			handler_obj =
			    acpi_ev_find_region_handler(space_id, handler_obj);
			if (handler_obj) {

				/* Found correct handler */

				ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
						  "Found handler %p for region %p in obj %p\n",
						  handler_obj, region_obj,
						  obj_desc));

				(void)acpi_ev_attach_region(handler_obj,
							    region_obj, FALSE);

				/*
				 * Tell all users that this region is usable by
				 * running the _REG method
				 */
				acpi_ex_exit_interpreter();
				(void)acpi_ev_execute_reg_method(region_obj,
								 ACPI_REG_CONNECT);
				acpi_ex_enter_interpreter();
				return_ACPI_STATUS(AE_OK);
			}
		}

		/* This node does not have the handler we need; Pop up one level */

		node = node->parent;
	}

	/*
	 * If we get here, there is no handler for this region. This is not
	 * fatal because many regions get created before a handler is installed
	 * for said region.
	 */
	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "No handler for RegionType %s(%X) (RegionObj %p)\n",
			  acpi_ut_get_region_name(space_id), space_id,
			  region_obj));

	return_ACPI_STATUS(AE_OK);
}
