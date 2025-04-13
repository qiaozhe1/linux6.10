// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: psxface - Parser external interfaces
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "acdispat.h"
#include "acinterp.h"
#include "actables.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_PARSER
ACPI_MODULE_NAME("psxface")

/* Local Prototypes */
static void
acpi_ps_update_parameter_list(struct acpi_evaluate_info *info, u16 action);

/*******************************************************************************
 *
 * FUNCTION:    acpi_debug_trace
 *
 * PARAMETERS:  method_name     - Valid ACPI name string
 *              debug_level     - Optional level mask. 0 to use default
 *              debug_layer     - Optional layer mask. 0 to use default
 *              flags           - bit 1: one shot(1) or persistent(0)
 *
 * RETURN:      Status
 *
 * DESCRIPTION: External interface to enable debug tracing during control
 *              method execution
 *
 ******************************************************************************/

acpi_status
acpi_debug_trace(const char *name, u32 debug_level, u32 debug_layer, u32 flags)
{
	acpi_status status;

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return (status);
	}

	acpi_gbl_trace_method_name = name;
	acpi_gbl_trace_flags = flags;
	acpi_gbl_trace_dbg_level = debug_level;
	acpi_gbl_trace_dbg_layer = debug_layer;
	status = AE_OK;

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return (status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_execute_method
 *
 * PARAMETERS:  info            - Method info block, contains:
 *                  node            - Method Node to execute
 *                  obj_desc        - Method object
 *                  parameters      - List of parameters to pass to the method,
 *                                    terminated by NULL. Params itself may be
 *                                    NULL if no parameters are being passed.
 *                  return_object   - Where to put method's return value (if
 *                                    any). If NULL, no value is returned.
 *                  parameter_type  - Type of Parameter list
 *                  return_object   - Where to put method's return value (if
 *                                    any). If NULL, no value is returned.
 *                  pass_number     - Parse or execute pass
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute a control method
 *
 ******************************************************************************/
/* 执行ACPI控制方法的主函数入口 */
acpi_status acpi_ps_execute_method(struct acpi_evaluate_info *info)
{
	acpi_status status;
	union acpi_parse_object *op;//AML解析树根节点（存储解析后的操作树）
	struct acpi_walk_state *walk_state;//解释器执行状态机（保存执行上下文）

	ACPI_FUNCTION_TRACE(ps_execute_method);

	/* Quick validation of DSDT header */

	acpi_tb_check_dsdt_header();//快速验证DSDT头（系统描述表核心结构）,确保系统表未损坏

	/* Validate the Info and method Node */

	if (!info || !info->node) {//如果输入参数info或命名空间节点无效
		return_ACPI_STATUS(AE_NULL_ENTRY);//返回空入口错误
	}

	/* Init for new method, wait on concurrency semaphore */
	/* 初始化新方法，等待并发信号量 */
	status =
	    acpi_ds_begin_method_execution(info->node, info->obj_desc, NULL);//初始化方法执行状态
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//初始化失败，直接返回错误码
	}

	/*
	 * The caller "owns" the parameters, so give each one an extra reference
	 */
	acpi_ps_update_parameter_list(info, REF_INCREMENT);//增加参数引用计数（防止执行期间参数被释放）。REF_INCREMENT表示增加引用

	/*
	 * Execute the method. Performs parse simultaneously
	 */
	ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
			  "**** Begin Method Parse/Execute [%4.4s] **** Node=%p Obj=%p\n",
			  info->node->name.ascii, info->node, info->obj_desc));

	/* Create and init a Root Node */

	op = acpi_ps_create_scope_op(info->obj_desc->method.aml_start);//创建并初始化AML解析根节点
	if (!op) {
		status = AE_NO_MEMORY;//内存分配失败，返回内存不足错误
		goto cleanup;
	}

	/* Create and initialize a new walk state */
	/* 创建并初始化一个新的方法执行状态机 */
	info->pass_number = ACPI_IMODE_EXECUTE;//设置为执行模式
	walk_state =
	    acpi_ds_create_walk_state(info->obj_desc->method.owner_id, NULL,
				      NULL, NULL);//创建walk_state对象
	if (!walk_state) {
		status = AE_NO_MEMORY;//内存分配失败，内存不足错误
		goto cleanup;
	}

	/* 初始化AML解释器执行环境 */
	status = acpi_ds_init_aml_walk(walk_state, op, info->node,
				       info->obj_desc->method.aml_start,
				       info->obj_desc->method.aml_length, info,
				       info->pass_number);//配置解释器执行上下文
	if (ACPI_FAILURE(status)) {
		acpi_ds_delete_walk_state(walk_state);//初始化失败，释放walk_state资源
		goto cleanup;
	}

	walk_state->method_pathname = info->full_pathname;//设置方法路径名用于调试和错误报告
	walk_state->method_is_nested = FALSE;//标记当前方法为非嵌套调用

	if (info->obj_desc->method.info_flags & ACPI_METHOD_MODULE_LEVEL) {//检查是否为模块级代码（在ACPI表加载时执行的代码）
		walk_state->parse_flags |= ACPI_PARSE_MODULE_LEVEL;//设置模块级解析标志
	}

	/* 如有必要，调用一个内部c方法 */
	if (info->obj_desc->method.info_flags & ACPI_METHOD_INTERNAL_ONLY) {//检查方法是否为内部C函数实现（非AML代码）
		status =
		    info->obj_desc->method.dispatch.implementation(walk_state);//调用内置C函数实现（如\_GPE等核心方法）
		info->return_object = walk_state->return_desc;//获取方法返回值（存储在walk_state的return_desc字段）

		/* Cleanup states */

		acpi_ds_scope_stack_clear(walk_state);//清空方法执行过程中堆栈的作用域信息（如Scope()块）
		acpi_ps_cleanup_scope(&walk_state->parser_state);//清理解析器状态（如当前解析位置、操作数栈）
		acpi_ds_terminate_control_method(walk_state->method_desc,
						 walk_state);//终止方法执行（释放局部变量等资源）
		acpi_ds_delete_walk_state(walk_state);// 释放walk_state对象占用的内存
		goto cleanup;//跳转到统一清理流程
	}

	/*
	 * Start method evaluation with an implicit return of zero.
	 * This is done for Windows compatibility.
	 * Windows兼容性处理：AML代码若未显式Return，则默认返回0（Windows系统要求）
	 */
	if (acpi_gbl_enable_interpreter_slack) {// 全局开关启用兼容性模式
		walk_state->implicit_return_obj =
		    acpi_ut_create_integer_object((u64) 0);//创建值为0的整数对象作为默认返回值
		if (!walk_state->implicit_return_obj) { //如果内存分配失败
			status = AE_NO_MEMORY;//返回内存不足错误
			acpi_ds_delete_walk_state(walk_state);//释放walk_state资源
			goto cleanup;// 跳转到清理流程
		}
	}

	/* Parse the AML */

	status = acpi_ps_parse_aml(walk_state);// 核心函数：解析并执行AML代码流（如执行Store、If等操作）

	/* walk_state was deleted by parse_aml */

cleanup:
	acpi_ps_delete_parse_tree(op);//释放解析树（OP树）

	/* Take away the extra reference that we gave the parameters above */

	acpi_ps_update_parameter_list(info, REF_DECREMENT);//平衡参数引用计数（与入口处的REF_INCREMENT对应）

	/* Exit now if error above */

	if (ACPI_FAILURE(status)) {//错误处理：直接返回状态码
		return_ACPI_STATUS(status);
	}

	/*
	 * If the method has returned an object, signal this to the caller with
	 * a control exception code
	 * 返回值处理：
	 * 如果有返回对象，使用特殊状态码AE_CTRL_RETURN_VALUE标记
	 */
	if (info->return_object) {
		ACPI_DEBUG_PRINT((ACPI_DB_PARSE, "Method returned ObjDesc=%p\n",
				  info->return_object));
		ACPI_DUMP_STACK_ENTRY(info->return_object);

		status = AE_CTRL_RETURN_VALUE;//特殊状态码表示有返回值
	}

	return_ACPI_STATUS(status);//最终状态返回
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_execute_table
 *
 * PARAMETERS:  info            - Method info block, contains:
 *              node            - Node to where the is entered into the
 *                                namespace
 *              obj_desc        - Pseudo method object describing the AML
 *                                code of the entire table
 *              pass_number     - Parse or execute pass
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute a table
 *
 ******************************************************************************/
/*
 * acpi_ps_execute_table - 执行AML解析的核心控制函数
 * @info: 包含表执行上下文信息的评估信息结构
 * 
 * 核心流程：
 * 1. 创建解析树根节点
 * 2. 初始化AML遍历状态结构
 * 3. 配置模块级代码执行环境
 * 4. 进入解释器执行AML解析
 * 5. 清理临时资源
 *
 * 关键机制：
 * - 使用walk_state管理解析执行上下文
 * - 通过scope_stack处理命名空间作用域
 * - 支持嵌套方法执行（通过method_is_nested标志）
 */
acpi_status acpi_ps_execute_table(struct acpi_evaluate_info *info)
{
	acpi_status status;
	union acpi_parse_object *op = NULL;// 解析树根节点指针
	struct acpi_walk_state *walk_state = NULL;//AML遍历状态控制结构

	ACPI_FUNCTION_TRACE(ps_execute_table);

	/* Create and init a Root Node */

	op = acpi_ps_create_scope_op(info->obj_desc->method.aml_start);//创建解析树根节点（作用域节点）,基于AML起始地址创建
	if (!op) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Create and initialize a new walk state */

	walk_state =
	    acpi_ds_create_walk_state(info->obj_desc->method.owner_id, NULL,
				      NULL, NULL);//分配并初始化walk_state结构
	if (!walk_state) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	status = acpi_ds_init_aml_walk(walk_state, op, info->node,
				       info->obj_desc->method.aml_start,
				       info->obj_desc->method.aml_length, info,
				       info->pass_number);//初始化AML遍历器
	if (ACPI_FAILURE(status)) {
		goto cleanup;
	}

	walk_state->method_pathname = info->full_pathname;// 设置方法路径名
	walk_state->method_is_nested = FALSE;//标记非嵌套执行

	if (info->obj_desc->method.info_flags & ACPI_METHOD_MODULE_LEVEL) {//处理模块级代码标志
		walk_state->parse_flags |= ACPI_PARSE_MODULE_LEVEL;//设置模块级解析标志
	}

	/* Info->Node是加载表格的默认位置  */
	/* 非根节点时需要推入作用域栈 */
	if (info->node && info->node != acpi_gbl_root_node) {// 检查起始节点有效性
		status =
		    acpi_ds_scope_stack_push(info->node, ACPI_TYPE_METHOD,
					     walk_state);//压入作用域栈
		if (ACPI_FAILURE(status)) {
			goto cleanup;
		}
	}

	/*
	 * Parse the AML, walk_state will be deleted by parse_aml
	 */
	acpi_ex_enter_interpreter();// 获取解释器互斥锁
	status = acpi_ps_parse_aml(walk_state);// 核心AML解析函数
	acpi_ex_exit_interpreter();//释放解释器互斥锁
	walk_state = NULL;//防止悬空指针

cleanup://资源清理标签
	if (walk_state) {
		acpi_ds_delete_walk_state(walk_state);// 释放walk_state内存
	}
	if (op) {
		acpi_ps_delete_parse_tree(op);//递归删除解析树
	}
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_update_parameter_list
 *
 * PARAMETERS:  info            - See struct acpi_evaluate_info
 *                                (Used: parameter_type and Parameters)
 *              action          - Add or Remove reference
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Update reference count on all method parameter objects
 *
 ******************************************************************************/

static void
acpi_ps_update_parameter_list(struct acpi_evaluate_info *info, u16 action)
{
	u32 i;

	if (info->parameters) {

		/* Update reference count for each parameter */

		for (i = 0; info->parameters[i]; i++) {

			/* Ignore errors, just do them all */

			(void)acpi_ut_update_object_reference(info->
							      parameters[i],
							      action);
		}
	}
}
