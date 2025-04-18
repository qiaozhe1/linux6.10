// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: dsargs - Support for execution of dynamic arguments for static
 *                       objects (regions, fields, buffer fields, etc.)
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "amlcode.h"
#include "acdispat.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_DISPATCHER
ACPI_MODULE_NAME("dsargs")

/* Local prototypes */
static acpi_status
acpi_ds_execute_arguments(struct acpi_namespace_node *node,
			  struct acpi_namespace_node *scope_node,
			  u32 aml_length, u8 *aml_start);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_execute_arguments
 *
 * PARAMETERS:  node                - Object NS node
 *              scope_node          - Parent NS node
 *              aml_length          - Length of executable AML
 *              aml_start           - Pointer to the AML
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Late (deferred) execution of region or field arguments
 *
 ******************************************************************************/
/*
 * acpi_ds_execute_arguments - 执行ACPI方法的参数初始化
 *
 * 该函数处理ACPI方法的延迟参数初始化，分为两个阶段：
 * 1. 解析阶段：构建参数解析树
 * 2. 执行阶段：实际评估参数值
 *
 * @node:       目标命名空间节点(方法节点)
 * @scope_node: 父作用域节点
 * @aml_length: AML代码长度
 * @aml_start:  AML代码起始指针
 *
 * 返回值:
 *   AE_OK       - 成功
 *   AE_NO_MEMORY - 内存不足
 *   其他ACPI状态码
 */
static acpi_status
acpi_ds_execute_arguments(struct acpi_namespace_node *node,
			  struct acpi_namespace_node *scope_node,
			  u32 aml_length, u8 *aml_start)
{
	acpi_status status;
	union acpi_parse_object *op;//解析树操作节点
	struct acpi_walk_state *walk_state;

	ACPI_FUNCTION_TRACE_PTR(ds_execute_arguments, aml_start);

	/* 第一阶段：解析AML构建语法树 */
	op = acpi_ps_alloc_op(AML_INT_EVAL_SUBTREE_OP, aml_start);//创建根操作节点(AML_INT_EVAL_SUBTREE_OP类型)
	if (!op) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Save the Node for use in acpi_ps_parse_aml */

	op->common.node = scope_node;// 关联作用域节点到操作节点

	/* Create and initialize a new parser state */

	walk_state = acpi_ds_create_walk_state(0, NULL, NULL, NULL);//创建并初始化遍历状态机
	if (!walk_state) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	status = acpi_ds_init_aml_walk(walk_state, op, NULL, aml_start,
				       aml_length, NULL, ACPI_IMODE_LOAD_PASS1);//初始化AML遍历器(LOAD_PASS1模式)
	if (ACPI_FAILURE(status)) {
		acpi_ds_delete_walk_state(walk_state);//清理状态机
		goto cleanup;
	}

	/* Mark this parse as a deferred opcode */
	/* 标记为延迟操作解析 */
	walk_state->parse_flags = ACPI_PARSE_DEFERRED_OP;
	walk_state->deferred_node = node;//关联目标节点

	/* Pass1: Parse the entire declaration */

	status = acpi_ps_parse_aml(walk_state);//执行AML解析(构建语法树)
	if (ACPI_FAILURE(status)) {
		goto cleanup;
	}

	/* Get and init the Op created above */

	op->common.node = node;//更新操作节点
	acpi_ps_delete_parse_tree(op);//删除第一阶段的解析树

	/* Evaluate the deferred arguments */
	/* 第二阶段：实际执行参数评估 */
	op = acpi_ps_alloc_op(AML_INT_EVAL_SUBTREE_OP, aml_start);//重新创建根操作节点
	if (!op) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	op->common.node = scope_node;//再次关联作用域节点

	/* Create and initialize a new parser state */

	walk_state = acpi_ds_create_walk_state(0, NULL, NULL, NULL);//创建新的遍历状态机
	if (!walk_state) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Execute the opcode and arguments */

	status = acpi_ds_init_aml_walk(walk_state, op, NULL, aml_start,
				       aml_length, NULL, ACPI_IMODE_EXECUTE);//初始化AML遍历器(EXECUTE模式)
	if (ACPI_FAILURE(status)) {
		acpi_ds_delete_walk_state(walk_state);
		goto cleanup;
	}

	/* Mark this execution as a deferred opcode */

	walk_state->deferred_node = node;//标记延迟执行并关联目标节点
	status = acpi_ps_parse_aml(walk_state);//执行AML代码(实际评估参数)

cleanup:
	acpi_ps_delete_parse_tree(op);//最终清理：删除可能残留的解析树
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_buffer_field_arguments
 *
 * PARAMETERS:  obj_desc        - A valid buffer_field object
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Get buffer_field Buffer and Index. This implements the late
 *              evaluation of these field attributes.
 *
 ******************************************************************************/

acpi_status
acpi_ds_get_buffer_field_arguments(union acpi_operand_object *obj_desc)
{
	union acpi_operand_object *extra_desc;
	struct acpi_namespace_node *node;
	acpi_status status;

	ACPI_FUNCTION_TRACE_PTR(ds_get_buffer_field_arguments, obj_desc);

	if (obj_desc->common.flags & AOPOBJ_DATA_VALID) {
		return_ACPI_STATUS(AE_OK);
	}

	/* Get the AML pointer (method object) and buffer_field node */

	extra_desc = acpi_ns_get_secondary_object(obj_desc);
	node = obj_desc->buffer_field.node;

	ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
			(ACPI_TYPE_BUFFER_FIELD, node, NULL));

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "[%4.4s] BufferField Arg Init\n",
			  acpi_ut_get_node_name(node)));

	/* Execute the AML code for the term_arg arguments */

	status = acpi_ds_execute_arguments(node, node->parent,
					   extra_desc->extra.aml_length,
					   extra_desc->extra.aml_start);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_bank_field_arguments
 *
 * PARAMETERS:  obj_desc        - A valid bank_field object
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Get bank_field bank_value. This implements the late
 *              evaluation of these field attributes.
 *
 ******************************************************************************/
/*
 * acpi_ds_get_bank_field_arguments - 初始化Bank Field区域参数
 *
 * 功能：
 * 1. 处理Bank Field区域的延迟参数初始化
 * 2. 执行关联的AML参数代码
 * 3. 注册地址范围到ACPI命名空间
 *
 * @obj_desc: Bank Field对象描述符
 * 返回值：
 *   AE_OK - 成功
 *   其他状态码 - 错误情况
 */
acpi_status
acpi_ds_get_bank_field_arguments(union acpi_operand_object *obj_desc)
{
	union acpi_operand_object *extra_desc;//二级对象(存储AML信息)
	struct acpi_namespace_node *node;//关联的命名空间节点
	acpi_status status;

	ACPI_FUNCTION_TRACE_PTR(ds_get_bank_field_arguments, obj_desc);

	if (obj_desc->common.flags & AOPOBJ_DATA_VALID) {//检查是否已经初始化 
		return_ACPI_STATUS(AE_OK);//已初始化则直接返回
	}

	/* 获取二级对象和Bank Field节点 */
	extra_desc = acpi_ns_get_secondary_object(obj_desc);// 获取附加对象
	node = obj_desc->bank_field.node;//获取关联节点

	ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
			(ACPI_TYPE_LOCAL_BANK_FIELD, node, NULL));

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "[%4.4s] BankField Arg Init\n",
			  acpi_ut_get_node_name(node)));

	/* Execute the AML code for the term_arg arguments */

	status = acpi_ds_execute_arguments(node, node->parent,
					   extra_desc->extra.aml_length,
					   extra_desc->extra.aml_start);//执行AML参数初始化代码
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//执行失败直接返回
	}

	status = acpi_ut_add_address_range(obj_desc->region.space_id,
					   obj_desc->region.address,
					   obj_desc->region.length, node);//注册地址范围到ACPI命名空间
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_buffer_arguments
 *
 * PARAMETERS:  obj_desc        - A valid Buffer object
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Get Buffer length and initializer byte list. This implements
 *              the late evaluation of these attributes.
 *
 ******************************************************************************/
/*
 * acpi_ds_get_buffer_arguments - 解析并执行Buffer对象的初始化参数
 *
 * 该函数处理Buffer对象的延迟初始化，主要功能：
 * 1. 检查Buffer是否已初始化
 * 2. 验证关联的命名空间节点
 * 3. 执行Buffer的AML初始化代码
 *
 * @obj_desc: 要处理的Buffer操作对象
 * 返回值: 
 *   AE_OK            - 成功
 *   AE_AML_INTERNAL  - 内部错误(无命名节点)
 *   其他ACPI状态码   - 来自参数执行
 */
acpi_status acpi_ds_get_buffer_arguments(union acpi_operand_object *obj_desc)
{
	struct acpi_namespace_node *node;
	acpi_status status;

	ACPI_FUNCTION_TRACE_PTR(ds_get_buffer_arguments, obj_desc);

	if (obj_desc->common.flags & AOPOBJ_DATA_VALID) {//检查Buffer是否已经初始化(通过DATA_VALID标志)
		return_ACPI_STATUS(AE_OK);//已初始化则直接返回
	}

	/* Get the Buffer node */

	node = obj_desc->buffer.node;//从Buffer对象获取关联的空间节点指针
	if (!node) {
		ACPI_ERROR((AE_INFO,
			    "No pointer back to namespace node in buffer object %p",
			    obj_desc));
		return_ACPI_STATUS(AE_AML_INTERNAL);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Buffer Arg Init\n"));

	/* Execute the AML code for the term_arg arguments */

	status = acpi_ds_execute_arguments(node, node,
					   obj_desc->buffer.aml_length,
					   obj_desc->buffer.aml_start);//执行Buffer的AML初始化代码
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_package_arguments
 *
 * PARAMETERS:  obj_desc        - A valid Package object
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Get Package length and initializer byte list. This implements
 *              the late evaluation of these attributes.
 *
 ******************************************************************************/
/* 获取并初始化Package对象的参数列表 */
acpi_status acpi_ds_get_package_arguments(union acpi_operand_object *obj_desc)
{
	struct acpi_namespace_node *node;//关联的命名空间节点指针
	acpi_status status;//操作状态返回值

	ACPI_FUNCTION_TRACE_PTR(ds_get_package_arguments, obj_desc);

	if (obj_desc->common.flags & AOPOBJ_DATA_VALID) {//检查数据有效性标志（避免重复初始化）
		return_ACPI_STATUS(AE_OK);//已初始化直接返回成功
	}

	/* Get the Package node */

	node = obj_desc->package.node;//获取Package对象关联的命名空间节点
	if (!node) {//验证节点指针有效性
		ACPI_ERROR((AE_INFO,
			    "No pointer back to namespace node in package %p",
			    obj_desc));
		return_ACPI_STATUS(AE_AML_INTERNAL);//返回AML内部错误
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Package Argument Init, AML Ptr: %p\n",
			  obj_desc->package.aml_start));//调试输出：显示Package参数初始化的AML起始地址

	/* Execute the AML code for the term_arg arguments */

	/*
	 * 执行Package参数初始化：
	 * 参数1：scope_node - 当前命名空间节点（解析上下文）
	 * 参数2：arg_node   - 参数节点（此处复用当前节点）
	 * 参数3：aml_length - Package参数AML代码长度
	 * 参数4：aml_start  - Package参数AML代码起始位置
	 * */
	status = acpi_ds_execute_arguments(node, node,
					   obj_desc->package.aml_length,
					   obj_desc->package.aml_start);

	return_ACPI_STATUS(status);//返回执行状态
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_region_arguments
 *
 * PARAMETERS:  obj_desc        - A valid region object
 *
 * RETURN:      Status.
 *
 * DESCRIPTION: Get region address and length. This implements the late
 *              evaluation of these region attributes.
 *
 ******************************************************************************/

acpi_status acpi_ds_get_region_arguments(union acpi_operand_object *obj_desc)
{
	struct acpi_namespace_node *node;
	acpi_status status;
	union acpi_operand_object *extra_desc;

	ACPI_FUNCTION_TRACE_PTR(ds_get_region_arguments, obj_desc);

	if (obj_desc->region.flags & AOPOBJ_DATA_VALID) {
		return_ACPI_STATUS(AE_OK);
	}

	extra_desc = acpi_ns_get_secondary_object(obj_desc);
	if (!extra_desc) {
		return_ACPI_STATUS(AE_NOT_EXIST);
	}

	/* Get the Region node */

	node = obj_desc->region.node;

	ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
			(ACPI_TYPE_REGION, node, NULL));

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "[%4.4s] OpRegion Arg Init at AML %p\n",
			  acpi_ut_get_node_name(node),
			  extra_desc->extra.aml_start));

	/* Execute the argument AML */

	status = acpi_ds_execute_arguments(node, extra_desc->extra.scope_node,
					   extra_desc->extra.aml_length,
					   extra_desc->extra.aml_start);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	status = acpi_ut_add_address_range(obj_desc->region.space_id,
					   obj_desc->region.address,
					   obj_desc->region.length, node);
	return_ACPI_STATUS(status);
}
