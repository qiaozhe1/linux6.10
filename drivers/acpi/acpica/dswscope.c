// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: dswscope - Scope stack manipulation
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acdispat.h"

#define _COMPONENT          ACPI_DISPATCHER
ACPI_MODULE_NAME("dswscope")

/****************************************************************************
 *
 * FUNCTION:    acpi_ds_scope_stack_clear
 *
 * PARAMETERS:  walk_state      - Current state
 *
 * RETURN:      None
 *
 * DESCRIPTION: Pop (and free) everything on the scope stack except the
 *              root scope object (which remains at the stack top.)
 *
 ***************************************************************************/
void acpi_ds_scope_stack_clear(struct acpi_walk_state *walk_state)//清空walk_state对象的作用域栈
{
	union acpi_generic_state *scope_info;//临时变量，用于保存当前弹出的栈元素指针

	ACPI_FUNCTION_NAME(ds_scope_stack_clear);

	while (walk_state->scope_info) {//循环处理栈中所有元素，直到栈为空

		/* Pop a scope off the stack */

		scope_info = walk_state->scope_info;// 获取当前栈顶指针
		walk_state->scope_info = scope_info->scope.next;// 更新栈顶指针到下一个元素

		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "Popped object type (%s)\n",
				  acpi_ut_get_type_name(scope_info->common.
							value)));

		acpi_ut_delete_generic_state(scope_info);//释放通用状态对象内存
	}
}

/****************************************************************************
 *
 * FUNCTION:    acpi_ds_scope_stack_push
 *
 * PARAMETERS:  node            - Name to be made current
 *              type            - Type of frame being pushed
 *              walk_state      - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Push the current scope on the scope stack, and make the
 *              passed Node current.
 *
 ***************************************************************************/
/*
 * acpi_ds_scope_stack_push - 压入新的命名空间作用域到状态栈
 * @node: 关联的命名空间节点（不能为NULL）
 * @type: 作用域对象类型（ACPI_TYPE_DEVICE等）
 * @walk_state: 当前的遍历状态结构
 *
 * 功能说明：
 * 1. 创建新的作用域状态对象并初始化
 * 2. 验证节点和类型参数的有效性
 * 3. 更新作用域深度计数器
 * 4. 将新作用域压入栈顶
 * 5. 提供详细的调试输出（需开启ACPI_DEBUG_PRINT）
 *
 * 关键流程：
 * - 参数校验 → 内存分配 → 状态初始化 → 压栈操作
 * - 维护严格的嵌套深度计数（scope_depth）
 * - 支持完整的调试信息输出
 */
acpi_status
acpi_ds_scope_stack_push(struct acpi_namespace_node *node,
			 acpi_object_type type,
			 struct acpi_walk_state *walk_state)
{
	union acpi_generic_state *scope_info;//新作用域状态
	union acpi_generic_state *old_scope_info;//当前栈顶状态

	ACPI_FUNCTION_TRACE(ds_scope_stack_push);

	if (!node) {

		/* Invalid scope   */

		ACPI_ERROR((AE_INFO, "Null scope parameter"));
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* Make sure object type is valid */

	if (!acpi_ut_valid_object_type(type)) {//验证对象类型有效性
		ACPI_WARNING((AE_INFO, "Invalid object type: 0x%X", type));
	}

	/* Allocate a new scope object */

	scope_info = acpi_ut_create_generic_state();//分配作用域状态对象（从ACPI状态缓存）
	if (!scope_info) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Init new scope object */
	/* 初始化作用域状态 */
	scope_info->common.descriptor_type = ACPI_DESC_TYPE_STATE_WSCOPE;//作用域类型标记
	scope_info->scope.node = node;//绑定命名空间节点
	scope_info->common.value = (u16) type;//存储对象类型

	walk_state->scope_depth++;//增加嵌套深度

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "[%.2d] Pushed scope ",
			  (u32) walk_state->scope_depth));

	old_scope_info = walk_state->scope_info;//记录当前栈顶状态
	if (old_scope_info) {
		ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC,
				      "[%4.4s] (%s)",
				      acpi_ut_get_node_name(old_scope_info->
							    scope.node),
				      acpi_ut_get_type_name(old_scope_info->
							    common.value)));//打印当前栈顶作用域信息
	} else {
		ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC, ACPI_NAMESPACE_ROOT));//根作用域标记
	}

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC,
			      ", New scope -> [%4.4s] (%s)\n",
			      acpi_ut_get_node_name(scope_info->scope.node),
			      acpi_ut_get_type_name(scope_info->common.value)));//打印新作用域信息

	/* Push new scope object onto stack */
	/* 将新的作用域对象压入栈中 */
	acpi_ut_push_generic_state(&walk_state->scope_info, scope_info);//执行压栈操作,压入栈顶
	return_ACPI_STATUS(AE_OK);
}

/****************************************************************************
 *
 * FUNCTION:    acpi_ds_scope_stack_pop
 *
 * PARAMETERS:  walk_state      - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Pop the scope stack once.
 *
 ***************************************************************************/

acpi_status acpi_ds_scope_stack_pop(struct acpi_walk_state *walk_state)
{
	union acpi_generic_state *scope_info;
	union acpi_generic_state *new_scope_info;

	ACPI_FUNCTION_TRACE(ds_scope_stack_pop);

	/*
	 * Pop scope info object off the stack.
	 */
	scope_info = acpi_ut_pop_generic_state(&walk_state->scope_info);
	if (!scope_info) {
		return_ACPI_STATUS(AE_STACK_UNDERFLOW);
	}

	walk_state->scope_depth--;

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "[%.2d] Popped scope [%4.4s] (%s), New scope -> ",
			  (u32) walk_state->scope_depth,
			  acpi_ut_get_node_name(scope_info->scope.node),
			  acpi_ut_get_type_name(scope_info->common.value)));

	new_scope_info = walk_state->scope_info;
	if (new_scope_info) {
		ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC, "[%4.4s] (%s)\n",
				      acpi_ut_get_node_name(new_scope_info->
							    scope.node),
				      acpi_ut_get_type_name(new_scope_info->
							    common.value)));
	} else {
		ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC, "%s\n",
				      ACPI_NAMESPACE_ROOT));
	}

	acpi_ut_delete_generic_state(scope_info);
	return_ACPI_STATUS(AE_OK);
}
