// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: excreate - Named object creation
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acinterp.h"
#include "amlcode.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_EXECUTER
ACPI_MODULE_NAME("excreate")
/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_alias
 *
 * PARAMETERS:  walk_state           - Current state, contains operands
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new named alias
 *
 ******************************************************************************/
acpi_status acpi_ex_create_alias(struct acpi_walk_state *walk_state)
{
	struct acpi_namespace_node *target_node;
	struct acpi_namespace_node *alias_node;
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE(ex_create_alias);

	/* Get the source/alias operands (both namespace nodes) */

	alias_node = (struct acpi_namespace_node *)walk_state->operands[0];
	target_node = (struct acpi_namespace_node *)walk_state->operands[1];

	if ((target_node->type == ACPI_TYPE_LOCAL_ALIAS) ||
	    (target_node->type == ACPI_TYPE_LOCAL_METHOD_ALIAS)) {
		/*
		 * Dereference an existing alias so that we don't create a chain
		 * of aliases. With this code, we guarantee that an alias is
		 * always exactly one level of indirection away from the
		 * actual aliased name.
		 */
		target_node =
		    ACPI_CAST_PTR(struct acpi_namespace_node,
				  target_node->object);
	}

	/* Ensure that the target node is valid */

	if (!target_node) {
		return_ACPI_STATUS(AE_NULL_OBJECT);
	}

	/* Construct the alias object (a namespace node) */

	switch (target_node->type) {
	case ACPI_TYPE_METHOD:
		/*
		 * Control method aliases need to be differentiated with
		 * a special type
		 */
		alias_node->type = ACPI_TYPE_LOCAL_METHOD_ALIAS;
		break;

	default:
		/*
		 * All other object types.
		 *
		 * The new alias has the type ALIAS and points to the original
		 * NS node, not the object itself.
		 */
		alias_node->type = ACPI_TYPE_LOCAL_ALIAS;
		alias_node->object =
		    ACPI_CAST_PTR(union acpi_operand_object, target_node);
		break;
	}

	/* Since both operands are Nodes, we don't need to delete them */

	alias_node->object =
	    ACPI_CAST_PTR(union acpi_operand_object, target_node);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_event
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new event object
 *
 ******************************************************************************/

acpi_status acpi_ex_create_event(struct acpi_walk_state *walk_state)
{
	acpi_status status;
	union acpi_operand_object *obj_desc;

	ACPI_FUNCTION_TRACE(ex_create_event);

	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_EVENT);
	if (!obj_desc) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/*
	 * Create the actual OS semaphore, with zero initial units -- meaning
	 * that the event is created in an unsignalled state
	 */
	status = acpi_os_create_semaphore(ACPI_NO_UNIT_LIMIT, 0,
					  &obj_desc->event.os_semaphore);
	if (ACPI_FAILURE(status)) {
		goto cleanup;
	}

	/* Attach object to the Node */

	status = acpi_ns_attach_object((struct acpi_namespace_node *)
				       walk_state->operands[0], obj_desc,
				       ACPI_TYPE_EVENT);

cleanup:
	/*
	 * Remove local reference to the object (on error, will cause deletion
	 * of both object and semaphore if present.)
	 */
	acpi_ut_remove_reference(obj_desc);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_mutex
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new mutex object
 *
 *              Mutex (Name[0], sync_level[1])
 *
 ******************************************************************************/

acpi_status acpi_ex_create_mutex(struct acpi_walk_state *walk_state)
{
	acpi_status status = AE_OK;
	union acpi_operand_object *obj_desc;

	ACPI_FUNCTION_TRACE_PTR(ex_create_mutex, ACPI_WALK_OPERANDS);

	/* Create the new mutex object */

	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_MUTEX);
	if (!obj_desc) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Create the actual OS Mutex */

	status = acpi_os_create_mutex(&obj_desc->mutex.os_mutex);
	if (ACPI_FAILURE(status)) {
		goto cleanup;
	}

	/* Init object and attach to NS node */

	obj_desc->mutex.sync_level = (u8)walk_state->operands[1]->integer.value;
	obj_desc->mutex.node =
	    (struct acpi_namespace_node *)walk_state->operands[0];

	status =
	    acpi_ns_attach_object(obj_desc->mutex.node, obj_desc,
				  ACPI_TYPE_MUTEX);

cleanup:
	/*
	 * Remove local reference to the object (on error, will cause deletion
	 * of both object and semaphore if present.)
	 */
	acpi_ut_remove_reference(obj_desc);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_region
 *
 * PARAMETERS:  aml_start           - Pointer to the region declaration AML
 *              aml_length          - Max length of the declaration AML
 *              space_id            - Address space ID for the region
 *              walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new operation region object
 *
 ******************************************************************************/
/*
 * acpi_ex_create_region - 创建ACPI区域对象
 * @aml_start: AML字节码起始地址指针
 * @aml_length: AML字节码长度
 * @space_id: 地址空间ID(如系统内存、IO空间等)
 * @walk_state: 当前walk状态结构体
 *
 * 功能：
 * 1. 创建并初始化新的区域对象
 * 2. 验证地址空间ID有效性
 * 3. 将区域对象附加到命名空间节点
 * 4. 设置区域操作相关参数
 */
acpi_status
acpi_ex_create_region(u8 * aml_start,
		      u32 aml_length,
		      u8 space_id, struct acpi_walk_state *walk_state)
{
	acpi_status status;
	union acpi_operand_object *obj_desc;//区域对象描述符
	struct acpi_namespace_node *node;//关联的命名空间节点
	union acpi_operand_object *region_obj2;//区域辅助对象

	ACPI_FUNCTION_TRACE(ex_create_region);

	/* Get the Namespace Node */

	node = walk_state->op->common.node;//从当前操作获取关联的命名空间节点

        /*
         * 检查是否已有区域对象附加到此节点
         * 如果已附加则直接返回成功(避免重复创建)
         */
	if (acpi_ns_get_attached_object(node)) {
		return_ACPI_STATUS(AE_OK);//已存在则直接返回
	}

        /*
         * 验证地址空间ID有效性,必须是预定义ID或在用户定义范围内
         */
	if (!acpi_is_valid_space_id(space_id)) {
		/*
		 * Print an error message, but continue. We don't want to abort
		 * a table load for this exception. Instead, if the region is
		 * actually used at runtime, abort the executing method.
		 */
		ACPI_ERROR((AE_INFO,
			    "Invalid/unknown Address Space ID: 0x%2.2X",
			    space_id));
	}

	ACPI_DEBUG_PRINT((ACPI_DB_LOAD, "Region Type - %s (0x%X)\n",
			  acpi_ut_get_region_name(space_id), space_id));

	/* Create the region descriptor */

	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_REGION);//创建区域描述符对象
	if (!obj_desc) {//对象创建失败
		status = AE_NO_MEMORY;
		goto cleanup;//跳转到清理环节
	}

	/*
	 * Remember location in AML stream of address & length
	 * operands since they need to be evaluated at run time.
	 */
        /*
         * 设置区域辅助信息
         * 记录地址和长度操作数在AML流中的位置，
         * 因为它们需要在运行时被求值。
         */
	region_obj2 = acpi_ns_get_secondary_object(obj_desc);//获取辅助对象
	region_obj2->extra.aml_start = aml_start;//保存AML起始位置
	region_obj2->extra.aml_length = aml_length;//保存AML长度
	region_obj2->extra.method_REG = NULL;//初始化方法指针
	
	/* 设置作用域节点(优先使用当前作用域，否则使用区域节点) */
	if (walk_state->scope_info) {
		region_obj2->extra.scope_node =
		    walk_state->scope_info->scope.node;
	} else {
		region_obj2->extra.scope_node = node;
	}

	/* Init the region from the operands */
	/* 初始化区域对象字段 */
	obj_desc->region.space_id = space_id;//设置地址空间ID
	obj_desc->region.address = 0;//初始化地址为0
	obj_desc->region.length = 0;//初始化长度为0
	obj_desc->region.pointer = NULL;//初始化指针为NULL
	obj_desc->region.node = node;//设置关联节点
	obj_desc->region.handler = NULL;//初始化处理器为NULL
	obj_desc->common.flags &=
	    ~(AOPOBJ_SETUP_COMPLETE | AOPOBJ_REG_CONNECTED |
	      AOPOBJ_OBJECT_INITIALIZED);//清除对象状态标志位

	/* Install the new region object in the parent Node */

	status = acpi_ns_attach_object(node, obj_desc, ACPI_TYPE_REGION);//将区域对象附加到父节点

cleanup:

	/* Remove local reference to the object */

	acpi_ut_remove_reference(obj_desc);//减少引用计数
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_processor
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new processor object and populate the fields
 *
 *              Processor (Name[0], cpu_ID[1], pblock_addr[2], pblock_length[3])
 *
 ******************************************************************************/

acpi_status acpi_ex_create_processor(struct acpi_walk_state *walk_state)
{
	union acpi_operand_object **operand = &walk_state->operands[0];
	union acpi_operand_object *obj_desc;
	acpi_status status;

	ACPI_FUNCTION_TRACE_PTR(ex_create_processor, walk_state);

	/* Create the processor object */

	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_PROCESSOR);
	if (!obj_desc) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Initialize the processor object from the operands */

	obj_desc->processor.proc_id = (u8) operand[1]->integer.value;
	obj_desc->processor.length = (u8) operand[3]->integer.value;
	obj_desc->processor.address =
	    (acpi_io_address)operand[2]->integer.value;

	/* Install the processor object in the parent Node */

	status = acpi_ns_attach_object((struct acpi_namespace_node *)operand[0],
				       obj_desc, ACPI_TYPE_PROCESSOR);

	/* Remove local reference to the object */

	acpi_ut_remove_reference(obj_desc);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_power_resource
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new power_resource object and populate the fields
 *
 *              power_resource (Name[0], system_level[1], resource_order[2])
 *
 ******************************************************************************/

acpi_status acpi_ex_create_power_resource(struct acpi_walk_state *walk_state)
{
	union acpi_operand_object **operand = &walk_state->operands[0];
	acpi_status status;
	union acpi_operand_object *obj_desc;

	ACPI_FUNCTION_TRACE_PTR(ex_create_power_resource, walk_state);

	/* Create the power resource object */

	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_POWER);
	if (!obj_desc) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Initialize the power object from the operands */

	obj_desc->power_resource.system_level = (u8) operand[1]->integer.value;
	obj_desc->power_resource.resource_order =
	    (u16) operand[2]->integer.value;

	/* Install the  power resource object in the parent Node */

	status = acpi_ns_attach_object((struct acpi_namespace_node *)operand[0],
				       obj_desc, ACPI_TYPE_POWER);

	/* Remove local reference to the object */

	acpi_ut_remove_reference(obj_desc);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_create_method
 *
 * PARAMETERS:  aml_start       - First byte of the method's AML
 *              aml_length      - AML byte count for this method
 *              walk_state      - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new method object
 *
 ******************************************************************************/
/*
 * acpi_ex_create_method - 创建ACPI方法对象
 * @aml_start: 方法AML代码的起始地址指针
 * @aml_length: 方法AML代码的长度
 * @walk_state: 当前walk状态结构体
 *
 * 功能：
 * 1. 创建新的方法对象(ACPI_TYPE_METHOD)
 * 2. 设置方法的基本属性(AML指针、长度、关联节点)
 * 3. 解析方法标志位(参数计数、串行化、同步级别)
 * 4. 将方法对象附加到命名空间节点
 * 5. 管理对象引用计数
 *
 * 返回值：
 * AE_OK - 方法创建成功
 * AE_NO_MEMORY - 内存分配失败
 * AE_BAD_PARAMETER - 无效参数
 */
acpi_status
acpi_ex_create_method(u8 * aml_start,
		      u32 aml_length, struct acpi_walk_state *walk_state)
{
	union acpi_operand_object **operand = &walk_state->operands[0];//操作数数组
	union acpi_operand_object *obj_desc;//新方法对象
	acpi_status status;//操作状态
	u8 method_flags;

	ACPI_FUNCTION_TRACE_PTR(ex_create_method, walk_state);

	/* 创建方法对象 */
	obj_desc = acpi_ut_create_internal_object(ACPI_TYPE_METHOD);//创建方法类型对象
	if (!obj_desc) {
		status = AE_NO_MEMORY;
		goto exit;
	}

	/* Save the method's AML pointer and length  */

	obj_desc->method.aml_start = aml_start;//保存AML代码起始位置
	obj_desc->method.aml_length = aml_length;//保存AML代码长度
	obj_desc->method.node = operand[0];//关联命名空间节点

	/*
	 * Disassemble the method flags. Split off the arg_count, Serialized
	 * flag, and sync_level for efficiency.
	 */
	method_flags = (u8)operand[1]->integer.value;//从操作数获取标志位
	obj_desc->method.param_count = (u8)
	    (method_flags & AML_METHOD_ARG_COUNT);//提取参数计数(低3位)

	/*
	 * Get the sync_level. If method is serialized, a mutex will be
	 * created for this method when it is parsed.
	 */
	if (method_flags & AML_METHOD_SERIALIZED) {//检查串行化标志
		obj_desc->method.info_flags = ACPI_METHOD_SERIALIZED;//设置串行化标志

		/*
		 * ACPI 1.0: sync_level = 0
		 * ACPI 2.0: sync_level = sync_level in method declaration
		 */
		obj_desc->method.sync_level = (u8)
		    ((method_flags & AML_METHOD_SYNC_LEVEL) >> 4);//提取同步级别(ACPI 2.0特性),同步级别存储在标志位的高4位
	}

	/* Attach the new object to the method Node */

	status = acpi_ns_attach_object((struct acpi_namespace_node *)operand[0],
				       obj_desc, ACPI_TYPE_METHOD);//将方法对象附加到命名空间节点

	/* Remove local reference to the object */

	acpi_ut_remove_reference(obj_desc);//减少本地对象引用计数

exit:
	/* Remove a reference to the operand */

	acpi_ut_remove_reference(operand[1]);//减少标志位操作数的引用
	return_ACPI_STATUS(status);// 返回最终状态
}
