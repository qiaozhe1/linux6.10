// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: evxfregn - External Interfaces, ACPI Operation Regions and
 *                         Address Spaces.
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#define EXPORT_ACPI_INTERFACES

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acevents.h"

#define _COMPONENT          ACPI_EVENTS
ACPI_MODULE_NAME("evxfregn")

/*******************************************************************************
 *
 * FUNCTION:    acpi_install_address_space_handler_internal
 *
 * PARAMETERS:  device          - Handle for the device
 *              space_id        - The address space ID
 *              handler         - Address of the handler
 *              setup           - Address of the setup function
 *              context         - Value passed to the handler on each access
 *              Run_reg         - Run _REG methods for this address space?
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Install a handler for all op_regions of a given space_id.
 *
 * NOTE: This function should only be called after acpi_enable_subsystem has
 * been called. This is because any _REG methods associated with the Space ID
 * are executed here, and these methods can only be safely executed after
 * the default handlers have been installed and the hardware has been
 * initialized (via acpi_enable_subsystem.)
 * To avoid this problem pass FALSE for Run_Reg and later on call
 * acpi_execute_reg_methods() to execute _REG.
 *
 ******************************************************************************/
/**
 * acpi_install_address_space_handler_internal - 安装ACPI地址空间处理程序(内部实现)
 * @device: ACPI命名空间节点句柄
 * @space_id: 地址空间类型(ACPI_ADR_SPACE_SYSTEM_MEMORY等)
 * @handler: 地址空间处理函数指针
 * @setup: 初始化设置函数指针
 * @context: 传递给处理程序的上下文数据
 * @run_reg: 是否执行_REG方法的标志
 *
 * 功能说明:
 * 1. 参数验证和句柄转换
 * 2. 获取命名空间互斥锁
 * 3. 安装地址空间处理程序
 * 4. 可选执行_REG方法
 * 5. 错误处理和资源清理
 */
static acpi_status
acpi_install_address_space_handler_internal(acpi_handle device,
					    acpi_adr_space_type space_id,
					    acpi_adr_space_handler handler,
					    acpi_adr_space_setup setup,
					    void *context, u8 run_reg)
{
	struct acpi_namespace_node *node;//定义命名空间节点指针
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_install_address_space_handler);

	/* Parameter validation */
	if (!device) {//检查设备句柄是否有效
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);//获取ACPI命名空间互斥锁
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//返回锁获取失败状态
	}

	/* 转换并验证设备句柄 */
	node = acpi_ns_validate_handle(device);//将ACPI句柄转换为命名空间节点
	if (!node) {// 检查节点是否有效
		status = AE_BAD_PARAMETER;//设置参数错误状态
		goto unlock_and_exit;
	}

	/* 为指定Space ID安装Region处理程序 */
	status =
	    acpi_ev_install_space_handler(node, space_id, handler, setup,
					  context);//调用内部函数安装空间处理程序
	if (ACPI_FAILURE(status)) {
		goto unlock_and_exit;//跳转到解锁并退出标签
	}

	/* 执行该地址空间的所有_REG方法 */
	if (run_reg) {//检查是否需要执行_REG方法
		acpi_ev_execute_reg_methods(node, space_id, ACPI_REG_CONNECT);//执行_REG方法，参数表示连接操作
	}

unlock_and_exit://解锁并退出标签
	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);//释放命名空间互斥锁(忽略返回值)
	return_ACPI_STATUS(status);//返回最终状态
}

acpi_status
acpi_install_address_space_handler(acpi_handle device,
				   acpi_adr_space_type space_id,
				   acpi_adr_space_handler handler,
				   acpi_adr_space_setup setup, void *context)
{
	return acpi_install_address_space_handler_internal(device, space_id,
							   handler, setup,
							   context, TRUE);
}

ACPI_EXPORT_SYMBOL(acpi_install_address_space_handler)
acpi_status
acpi_install_address_space_handler_no_reg(acpi_handle device,
					  acpi_adr_space_type space_id,
					  acpi_adr_space_handler handler,
					  acpi_adr_space_setup setup,
					  void *context)
{
	return acpi_install_address_space_handler_internal(device, space_id,
							   handler, setup,
							   context, FALSE);
}

ACPI_EXPORT_SYMBOL(acpi_install_address_space_handler_no_reg)

/*******************************************************************************
 *
 * FUNCTION:    acpi_remove_address_space_handler
 *
 * PARAMETERS:  device          - Handle for the device
 *              space_id        - The address space ID
 *              handler         - Address of the handler
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Remove a previously installed handler.
 *
 ******************************************************************************/
acpi_status
acpi_remove_address_space_handler(acpi_handle device,
				  acpi_adr_space_type space_id,
				  acpi_adr_space_handler handler)
{
	union acpi_operand_object *obj_desc;
	union acpi_operand_object *handler_obj;
	union acpi_operand_object *region_obj;
	union acpi_operand_object **last_obj_ptr;
	struct acpi_namespace_node *node;
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_remove_address_space_handler);

	/* Parameter validation */

	if (!device) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_validate_handle(device);
	if (!node ||
	    ((node->type != ACPI_TYPE_DEVICE) &&
	     (node->type != ACPI_TYPE_PROCESSOR) &&
	     (node->type != ACPI_TYPE_THERMAL) &&
	     (node != acpi_gbl_root_node))) {
		status = AE_BAD_PARAMETER;
		goto unlock_and_exit;
	}

	/* Make sure the internal object exists */

	obj_desc = acpi_ns_get_attached_object(node);
	if (!obj_desc) {
		status = AE_NOT_EXIST;
		goto unlock_and_exit;
	}

	/* Find the address handler the user requested */

	handler_obj = obj_desc->common_notify.handler;
	last_obj_ptr = &obj_desc->common_notify.handler;
	while (handler_obj) {

		/* We have a handler, see if user requested this one */

		if (handler_obj->address_space.space_id == space_id) {

			/* Handler must be the same as the installed handler */

			if (handler_obj->address_space.handler != handler) {
				status = AE_BAD_PARAMETER;
				goto unlock_and_exit;
			}

			/* Matched space_id, first dereference this in the Regions */

			ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
					  "Removing address handler %p(%p) for region %s "
					  "on Device %p(%p)\n",
					  handler_obj, handler,
					  acpi_ut_get_region_name(space_id),
					  node, obj_desc));

			region_obj = handler_obj->address_space.region_list;

			/* Walk the handler's region list */

			while (region_obj) {
				/*
				 * First disassociate the handler from the region.
				 *
				 * NOTE: this doesn't mean that the region goes away
				 * The region is just inaccessible as indicated to
				 * the _REG method
				 */
				acpi_ev_detach_region(region_obj, TRUE);

				/*
				 * Walk the list: Just grab the head because the
				 * detach_region removed the previous head.
				 */
				region_obj =
				    handler_obj->address_space.region_list;
			}

			/* Remove this Handler object from the list */

			*last_obj_ptr = handler_obj->address_space.next;

			/* Now we can delete the handler object */

			acpi_os_release_mutex(handler_obj->address_space.
					      context_mutex);
			acpi_ut_remove_reference(handler_obj);
			goto unlock_and_exit;
		}

		/* Walk the linked list of handlers */

		last_obj_ptr = &handler_obj->address_space.next;
		handler_obj = handler_obj->address_space.next;
	}

	/* The handler does not exist */

	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "Unable to remove address handler %p for %s(%X), DevNode %p, obj %p\n",
			  handler, acpi_ut_get_region_name(space_id), space_id,
			  node, obj_desc));

	status = AE_NOT_EXIST;

unlock_and_exit:
	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_remove_address_space_handler)
/*******************************************************************************
 *
 * FUNCTION:    acpi_execute_reg_methods
 *
 * PARAMETERS:  device          - Handle for the device
 *              space_id        - The address space ID
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute _REG for all op_regions of a given space_id.
 *
 ******************************************************************************/
acpi_status
acpi_execute_reg_methods(acpi_handle device, acpi_adr_space_type space_id)
{
	struct acpi_namespace_node *node;
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_execute_reg_methods);

	/* Parameter validation */

	if (!device) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_validate_handle(device);
	if (node) {

		/* Run all _REG methods for this address space */

		acpi_ev_execute_reg_methods(node, space_id, ACPI_REG_CONNECT);
	} else {
		status = AE_BAD_PARAMETER;
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_execute_reg_methods)

/*******************************************************************************
 *
 * FUNCTION:    acpi_execute_orphan_reg_method
 *
 * PARAMETERS:  device          - Handle for the device
 *              space_id        - The address space ID
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute an "orphan" _REG method that appears under an ACPI
 *              device. This is a _REG method that has no corresponding region
 *              within the device's scope.
 *
 ******************************************************************************/
acpi_status
acpi_execute_orphan_reg_method(acpi_handle device, acpi_adr_space_type space_id)
{
	struct acpi_namespace_node *node;
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_execute_orphan_reg_method);

	/* Parameter validation */

	if (!device) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Convert and validate the device handle */

	node = acpi_ns_validate_handle(device);
	if (node) {

		/*
		 * If an "orphan" _REG method is present in the device's scope
		 * for the given address space ID, run it.
		 */

		acpi_ev_execute_orphan_reg_method(node, space_id);
	} else {
		status = AE_BAD_PARAMETER;
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_execute_orphan_reg_method)
