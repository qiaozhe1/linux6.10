// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: dswstate - Dispatcher parse tree walk management routines
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "acdispat.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_DISPATCHER
ACPI_MODULE_NAME("dswstate")

  /* Local prototypes */
static acpi_status
acpi_ds_result_stack_push(struct acpi_walk_state *walk_state);
static acpi_status acpi_ds_result_stack_pop(struct acpi_walk_state *walk_state);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_result_pop
 *
 * PARAMETERS:  object              - Where to return the popped object
 *              walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Pop an object off the top of this walk's result stack
 *
 ******************************************************************************/

acpi_status
acpi_ds_result_pop(union acpi_operand_object **object,
		   struct acpi_walk_state *walk_state)
{
	u32 index;
	union acpi_generic_state *state;
	acpi_status status;

	ACPI_FUNCTION_NAME(ds_result_pop);

	state = walk_state->results;

	/* Incorrect state of result stack */

	if (state && !walk_state->result_count) {
		ACPI_ERROR((AE_INFO, "No results on result stack"));
		return (AE_AML_INTERNAL);
	}

	if (!state && walk_state->result_count) {
		ACPI_ERROR((AE_INFO, "No result state for result stack"));
		return (AE_AML_INTERNAL);
	}

	/* Empty result stack */

	if (!state) {
		ACPI_ERROR((AE_INFO, "Result stack is empty! State=%p",
			    walk_state));
		return (AE_AML_NO_RETURN_VALUE);
	}

	/* Return object of the top element and clean that top element result stack */

	walk_state->result_count--;
	index = (u32)walk_state->result_count % ACPI_RESULTS_FRAME_OBJ_NUM;

	*object = state->results.obj_desc[index];
	if (!*object) {
		ACPI_ERROR((AE_INFO,
			    "No result objects on result stack, State=%p",
			    walk_state));
		return (AE_AML_NO_RETURN_VALUE);
	}

	state->results.obj_desc[index] = NULL;
	if (index == 0) {
		status = acpi_ds_result_stack_pop(walk_state);
		if (ACPI_FAILURE(status)) {
			return (status);
		}
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "Obj=%p [%s] Index=%X State=%p Num=%X\n", *object,
			  acpi_ut_get_object_type_name(*object),
			  index, walk_state, walk_state->result_count));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_result_push
 *
 * PARAMETERS:  object              - Where to return the popped object
 *              walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Push an object onto the current result stack
 *
 ******************************************************************************/

acpi_status
acpi_ds_result_push(union acpi_operand_object *object,
		    struct acpi_walk_state *walk_state)
{
	union acpi_generic_state *state;
	acpi_status status;
	u32 index;

	ACPI_FUNCTION_NAME(ds_result_push);

	if (walk_state->result_count > walk_state->result_size) {
		ACPI_ERROR((AE_INFO, "Result stack is full"));
		return (AE_AML_INTERNAL);
	} else if (walk_state->result_count == walk_state->result_size) {

		/* Extend the result stack */

		status = acpi_ds_result_stack_push(walk_state);
		if (ACPI_FAILURE(status)) {
			ACPI_ERROR((AE_INFO,
				    "Failed to extend the result stack"));
			return (status);
		}
	}

	if (!(walk_state->result_count < walk_state->result_size)) {
		ACPI_ERROR((AE_INFO, "No free elements in result stack"));
		return (AE_AML_INTERNAL);
	}

	state = walk_state->results;
	if (!state) {
		ACPI_ERROR((AE_INFO, "No result stack frame during push"));
		return (AE_AML_INTERNAL);
	}

	if (!object) {
		ACPI_ERROR((AE_INFO,
			    "Null Object! State=%p Num=%u",
			    walk_state, walk_state->result_count));
		return (AE_BAD_PARAMETER);
	}

	/* Assign the address of object to the top free element of result stack */

	index = (u32)walk_state->result_count % ACPI_RESULTS_FRAME_OBJ_NUM;
	state->results.obj_desc[index] = object;
	walk_state->result_count++;

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Obj=%p [%s] State=%p Num=%X Cur=%X\n",
			  object,
			  acpi_ut_get_object_type_name((union
							acpi_operand_object *)
						       object), walk_state,
			  walk_state->result_count,
			  walk_state->current_result));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_result_stack_push
 *
 * PARAMETERS:  walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Push an object onto the walk_state result stack
 *
 ******************************************************************************/

static acpi_status acpi_ds_result_stack_push(struct acpi_walk_state *walk_state)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_NAME(ds_result_stack_push);

	/* Check for stack overflow */

	if (((u32) walk_state->result_size + ACPI_RESULTS_FRAME_OBJ_NUM) >
	    ACPI_RESULTS_OBJ_NUM_MAX) {
		ACPI_ERROR((AE_INFO, "Result stack overflow: State=%p Num=%u",
			    walk_state, walk_state->result_size));
		return (AE_STACK_OVERFLOW);
	}

	state = acpi_ut_create_generic_state();
	if (!state) {
		return (AE_NO_MEMORY);
	}

	state->common.descriptor_type = ACPI_DESC_TYPE_STATE_RESULT;
	acpi_ut_push_generic_state(&walk_state->results, state);

	/* Increase the length of the result stack by the length of frame */

	walk_state->result_size += ACPI_RESULTS_FRAME_OBJ_NUM;

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Results=%p State=%p\n",
			  state, walk_state));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_result_stack_pop
 *
 * PARAMETERS:  walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Pop an object off of the walk_state result stack
 *
 ******************************************************************************/

static acpi_status acpi_ds_result_stack_pop(struct acpi_walk_state *walk_state)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_NAME(ds_result_stack_pop);

	/* Check for stack underflow */

	if (walk_state->results == NULL) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "Result stack underflow - State=%p\n",
				  walk_state));
		return (AE_AML_NO_OPERAND);
	}

	if (walk_state->result_size < ACPI_RESULTS_FRAME_OBJ_NUM) {
		ACPI_ERROR((AE_INFO, "Insufficient result stack size"));
		return (AE_AML_INTERNAL);
	}

	state = acpi_ut_pop_generic_state(&walk_state->results);
	acpi_ut_delete_generic_state(state);

	/* Decrease the length of result stack by the length of frame */

	walk_state->result_size -= ACPI_RESULTS_FRAME_OBJ_NUM;

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "Result=%p RemainingResults=%X State=%p\n",
			  state, walk_state->result_count, walk_state));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_obj_stack_push
 *
 * PARAMETERS:  object              - Object to push
 *              walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Push an object onto this walk's object/operand stack
 *
 ******************************************************************************/

acpi_status
acpi_ds_obj_stack_push(void *object, struct acpi_walk_state *walk_state)
{
	ACPI_FUNCTION_NAME(ds_obj_stack_push);

	/* Check for stack overflow */

	if (walk_state->num_operands >= ACPI_OBJ_NUM_OPERANDS) {
		ACPI_ERROR((AE_INFO,
			    "Object stack overflow! Obj=%p State=%p #Ops=%u",
			    object, walk_state, walk_state->num_operands));
		return (AE_STACK_OVERFLOW);
	}

	/* Put the object onto the stack */

	walk_state->operands[walk_state->operand_index] = object;
	walk_state->num_operands++;

	/* For the usual order of filling the operand stack */

	walk_state->operand_index++;

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Obj=%p [%s] State=%p #Ops=%X\n",
			  object,
			  acpi_ut_get_object_type_name((union
							acpi_operand_object *)
						       object), walk_state,
			  walk_state->num_operands));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_obj_stack_pop
 *
 * PARAMETERS:  pop_count           - Number of objects/entries to pop
 *              walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Pop this walk's object stack. Objects on the stack are NOT
 *              deleted by this routine.
 *
 ******************************************************************************/

acpi_status
acpi_ds_obj_stack_pop(u32 pop_count, struct acpi_walk_state *walk_state)
{
	u32 i;

	ACPI_FUNCTION_NAME(ds_obj_stack_pop);

	for (i = 0; i < pop_count; i++) {

		/* Check for stack underflow */

		if (walk_state->num_operands == 0) {
			ACPI_ERROR((AE_INFO,
				    "Object stack underflow! Count=%X State=%p #Ops=%u",
				    pop_count, walk_state,
				    walk_state->num_operands));
			return (AE_STACK_UNDERFLOW);
		}

		/* Just set the stack entry to null */

		walk_state->num_operands--;
		walk_state->operands[walk_state->num_operands] = NULL;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Count=%X State=%p #Ops=%u\n",
			  pop_count, walk_state, walk_state->num_operands));

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_obj_stack_pop_and_delete
 *
 * PARAMETERS:  pop_count           - Number of objects/entries to pop
 *              walk_state          - Current Walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Pop this walk's object stack and delete each object that is
 *              popped off.
 *
 ******************************************************************************/

void
acpi_ds_obj_stack_pop_and_delete(u32 pop_count,
				 struct acpi_walk_state *walk_state)
{
	s32 i;
	union acpi_operand_object *obj_desc;

	ACPI_FUNCTION_NAME(ds_obj_stack_pop_and_delete);

	if (pop_count == 0) {
		return;
	}

	for (i = (s32)pop_count - 1; i >= 0; i--) {
		if (walk_state->num_operands == 0) {
			return;
		}

		/* Pop the stack and delete an object if present in this stack entry */

		walk_state->num_operands--;
		obj_desc = walk_state->operands[i];
		if (obj_desc) {
			acpi_ut_remove_reference(walk_state->operands[i]);
			walk_state->operands[i] = NULL;
		}
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Count=%X State=%p #Ops=%X\n",
			  pop_count, walk_state, walk_state->num_operands));
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_get_current_walk_state
 *
 * PARAMETERS:  thread          - Get current active state for this Thread
 *
 * RETURN:      Pointer to the current walk state
 *
 * DESCRIPTION: Get the walk state that is at the head of the list (the "current"
 *              walk state.)
 *
 ******************************************************************************/

struct acpi_walk_state *acpi_ds_get_current_walk_state(struct acpi_thread_state
						       *thread)
{
	ACPI_FUNCTION_NAME(ds_get_current_walk_state);

	if (!thread) {
		return (NULL);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_PARSE, "Current WalkState %p\n",
			  thread->walk_state_list));

	return (thread->walk_state_list);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_push_walk_state
 *
 * PARAMETERS:  walk_state      - State to push
 *              thread          - Thread state object
 *
 * RETURN:      None
 *
 * DESCRIPTION: Place the Thread state at the head of the state list
 *
 ******************************************************************************/

void
acpi_ds_push_walk_state(struct acpi_walk_state *walk_state,
			struct acpi_thread_state *thread)
{
	ACPI_FUNCTION_TRACE(ds_push_walk_state);

	walk_state->next = thread->walk_state_list;
	thread->walk_state_list = walk_state;

	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_pop_walk_state
 *
 * PARAMETERS:  thread      - Current thread state
 *
 * RETURN:      A walk_state object popped from the thread's stack
 *
 * DESCRIPTION: Remove and return the walkstate object that is at the head of
 *              the walk stack for the given walk list. NULL indicates that
 *              the list is empty.
 *
 ******************************************************************************/

struct acpi_walk_state *acpi_ds_pop_walk_state(struct acpi_thread_state *thread)
{
	struct acpi_walk_state *walk_state;

	ACPI_FUNCTION_TRACE(ds_pop_walk_state);

	walk_state = thread->walk_state_list;

	if (walk_state) {

		/* Next walk state becomes the current walk state */

		thread->walk_state_list = walk_state->next;

		/*
		 * Don't clear the NEXT field, this serves as an indicator
		 * that there is a parent WALK STATE
		 * Do Not: walk_state->Next = NULL;
		 */
	}

	return_PTR(walk_state);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_create_walk_state
 *
 * PARAMETERS:  owner_id        - ID for object creation
 *              origin          - Starting point for this walk
 *              method_desc     - Method object
 *              thread          - Current thread state
 *
 * RETURN:      Pointer to the new walk state.
 *
 * DESCRIPTION: Allocate and initialize a new walk state. The current walk
 *              state is set to this new state.
 *
 ******************************************************************************/
/*
 * acpi_ds_create_walk_state - 创建并初始化一个新的AML遍历状态结构
 * @owner_id: 资源所有者ID（用于对象生命周期管理）
 * @origin: 起始解析节点（通常为MethodOp或ScopeOp）
 * @method_desc: 关联的方法描述符（控制方法执行时非NULL）
 * @thread: 关联的线程状态（多线程执行时使用）
 *
 * 功能说明：
 * 1. 分配并初始化walk_state结构体（ACPI解释器的核心上下文）
 * 2. 设置方法参数/局部变量的初始状态
 * 3. 将新状态压入线程状态栈（如果存在线程上下文）
 * 4. 维护所有者ID与创建对象的关联关系
 *
 * 内存管理：
 * - 使用ACPI_ALLOCATE_ZEROED分配归零内存
 * - 必须通过acpi_ds_delete_walk_state释放
 */
struct acpi_walk_state *acpi_ds_create_walk_state(acpi_owner_id owner_id,
						  union acpi_parse_object
						  *origin,
						  union acpi_operand_object
						  *method_desc,
						  struct acpi_thread_state
						  *thread)
{
	struct acpi_walk_state *walk_state;

	ACPI_FUNCTION_TRACE(ds_create_walk_state);

	walk_state = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_walk_state));//分配并清零内存 
	if (!walk_state) {
		return_PTR(NULL);
	}

	/* 基础字段初始化 */
	walk_state->descriptor_type = ACPI_DESC_TYPE_WALK;//设置描述符类型(0x8)
	walk_state->method_desc = method_desc;//绑定方法对象
	walk_state->owner_id = owner_id;// 设置所有者ID
	walk_state->origin = origin;//记录起始解析节点
	walk_state->thread = thread;//关联线程上下文

	walk_state->parser_state.start_op = origin;//设置起始操作符

	/* Init the method args/local */

#ifndef ACPI_CONSTANT_EVAL_ONLY//非常量求值模式下才初始化
	acpi_ds_method_data_init(walk_state);//初始化Arg0-Arg6/Local0-Local7
#endif

	/* Put the new state at the head of the walk list */

	if (thread) {//如果存在线程
		acpi_ds_push_walk_state(walk_state, thread);//将新状态加入线程状态栈顶
	}

	return_PTR(walk_state);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_init_aml_walk
 *
 * PARAMETERS:  walk_state      - New state to be initialized
 *              op              - Current parse op
 *              method_node     - Control method NS node, if any
 *              aml_start       - Start of AML
 *              aml_length      - Length of AML
 *              info            - Method info block (params, etc.)
 *              pass_number     - 1, 2, or 3
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initialize a walk state for a pass 1 or 2 parse tree walk
 *
 ******************************************************************************/
/*
 * acpi_ds_init_aml_walk - 初始化AML解析/执行的遍历状态
 * @walk_state:   待初始化的遍历状态结构
 * @op:           起始解析节点（通常为ScopeOp）
 * @method_node:  关联的命名空间方法节点（控制方法执行时非NULL）
 * @aml_start:    AML字节码起始地址
 * @aml_length:   AML字节码长度
 * @info:         评估信息结构（含参数/返回值指针）
 * @pass_number:  解析阶段编号（ACPI_IMODE_LOAD_PASS1等）
 *
 * 核心功能：
 * 1. 设置AML解析器的初始状态（代码范围/当前位置等）
 * 2. 为控制方法初始化参数和局部变量
 * 3. 建立初始作用域栈
 * 4. 配置解析回调函数
 *
 * 设计要点：
 * - 区分方法执行和普通表解析两种模式
 * - 严格校验AML长度防止越界访问
 * - 维护完整的作用域链（Scope Stack）
 */
acpi_status
acpi_ds_init_aml_walk(struct acpi_walk_state *walk_state,
		      union acpi_parse_object *op,
		      struct acpi_namespace_node *method_node,
		      u8 * aml_start,
		      u32 aml_length,
		      struct acpi_evaluate_info *info, u8 pass_number)
{
	acpi_status status;
	struct acpi_parse_state *parser_state = &walk_state->parser_state;
	union acpi_parse_object *extra_op;

	ACPI_FUNCTION_TRACE(ds_init_aml_walk);

	/* 初始化AML解析范围 */
	walk_state->parser_state.aml =			//当前解析位置
	    walk_state->parser_state.aml_start =	//AML起始位置
	    walk_state->parser_state.aml_end =		//AML结束位置（初始值）
	    walk_state->parser_state.pkg_end = aml_start;//当前包结束位置
	
	/* 安全设置AML结束边界（避免对NULL指针做偏移） */
	if (aml_length != 0) {
		walk_state->parser_state.aml_end += aml_length;//计算实际结束地址
		walk_state->parser_state.pkg_end += aml_length;
	}

	/* The next_op of the next_walk will be the beginning of the method */

	/* 初始化遍历控制字段 */
	walk_state->next_op = NULL;//下一个待处理操作符初始为空
	walk_state->pass_number = pass_number;//记录解析阶段（加载/执行等）

	/* 处理调用参数（如果是方法调用） */
	if (info) {
		walk_state->params = info->parameters;//方法参数指针数组
		walk_state->caller_return_desc = &info->return_object;//返回值存储位置
	}

	/* 初始化解析器作用域 */
	status = acpi_ps_init_scope(&walk_state->parser_state, op);//建立初始解析作用域
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//作用域初始化失败直接返回
	}

	/* 方法调用路径处理 */
	if (method_node) {
		walk_state->parser_state.start_node = method_node;//设置起始节点
		walk_state->walk_type = ACPI_WALK_METHOD;//标记为方法执行模式
		walk_state->method_node = method_node;//记录方法节点
		walk_state->method_desc =
		    acpi_ns_get_attached_object(method_node);//获取方法描述符

		/* Push start scope on scope stack and make it current  */

		status =
		    acpi_ds_scope_stack_push(method_node, ACPI_TYPE_METHOD,
					     walk_state);//压入方法作用域到状态作用域栈
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);//栈操作失败返回
		}

		/* Init the method arguments */
		/* 初始化方法参数（Arg0-Arg6） */
		status = acpi_ds_method_data_init_args(walk_state->params,
						       ACPI_METHOD_NUM_ARGS,
						       walk_state);
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	} else {//非方法路径处理（如模块级代码执行）
		
		/*
                 * 设置当前作用域（scope）。
                 * 寻找一个带有命名空间节点（namespace node）的 Named Op。
                 * 从当前的 Op 向上查找。当前作用域即为第一个拥有命名空间
		 * 节点的 Op。
                 */
		extra_op = parser_state->start_op;//从当前Op开始查找
		while (extra_op && !extra_op->common.node) {//向上查找最近的命名空间节点作为作用域起点
			extra_op = extra_op->common.parent;//向父节点回溯
		}

		/* 设置起始节点（可能为NULL表示全局作用域） */
		if (!extra_op) {
			parser_state->start_node = NULL;
		} else {
			parser_state->start_node = extra_op->common.node;
		}

		if (parser_state->start_node) {//如果找到有效节点，压入作用域栈

			/* 将启动范围推入范围栈并使其成为当前范围 */
			status =
			    acpi_ds_scope_stack_push(parser_state->start_node,
						     parser_state->start_node->
						     type, walk_state);
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}
		}
	}

	status = acpi_ds_init_callbacks(walk_state, pass_number);//初始化解析回调函数（根据pass_number配置不同处理函数）
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_delete_walk_state
 *
 * PARAMETERS:  walk_state      - State to delete
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Delete a walk state including all internal data structures
 *
 ******************************************************************************/

void acpi_ds_delete_walk_state(struct acpi_walk_state *walk_state)// 释放walk_state对象及其关联资源
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_TRACE_PTR(ds_delete_walk_state, walk_state);

	if (!walk_state) {//参数有效性检查：walk_state为空则直接返回
		return_VOID;
	}

	if (walk_state->descriptor_type != ACPI_DESC_TYPE_WALK) {//验证对象类型是否为walk_state类型
		ACPI_ERROR((AE_INFO, "%p is not a valid walk state",
			    walk_state));
		return_VOID;//类型检查失败直接返回，避免后续操作无效对象
	}

	/* There should not be any open scopes */

	if (walk_state->parser_state.scope) {//检查并清理解析器状态中的作用域栈
		ACPI_ERROR((AE_INFO, "%p walk still has a scope list",
			    walk_state));
		acpi_ps_cleanup_scope(&walk_state->parser_state);//调用函数清空作用域栈
	}

	/* Always must free any linked control states */

	while (walk_state->control_state) {//遍历控制状态链表,释放控制状态链表（保存控制流信息）
		state = walk_state->control_state;//获取当前节点指针
		walk_state->control_state = state->common.next;//更新链表头到下一个节点

		acpi_ut_delete_generic_state(state);//释放当前节点内存
	}

	/* Always must free any linked parse states */

	while (walk_state->scope_info) {//遍历作用域信息链表,释放作用域信息链表（保存作用域栈状态）
		state = walk_state->scope_info;//获取当前节点指针
		walk_state->scope_info = state->common.next;//更新链表头到下一个节点

		acpi_ut_delete_generic_state(state);//释放当前节点内存
	}

	/* Always must free any stacked result states */

	while (walk_state->results) {//遍历结果状态链表, 释放结果状态链表（保存操作数栈结果）
		state = walk_state->results;//获取当前节点指针
		walk_state->results = state->common.next;//更新链表头到下一个节点

		acpi_ut_delete_generic_state(state);//释放当前节点内存
	}

	ACPI_FREE(walk_state);//释放walk_state对象本身内存（调用系统内存释放函数）
	return_VOID;
}
