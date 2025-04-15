// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: psparse - Parser top level AML parse routines
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

/*
 * Parse the AML and build an operation tree as most interpreters,
 * like Perl, do. Parsing is done by hand rather than with a YACC
 * generated parser to tightly constrain stack and dynamic memory
 * usage. At the same time, parsing is kept flexible and the code
 * fairly compact by parsing based on a list of AML opcode
 * templates in aml_op_info[]
 */

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "acdispat.h"
#include "amlcode.h"
#include "acinterp.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_PARSER
ACPI_MODULE_NAME("psparse")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_get_opcode_size
 *
 * PARAMETERS:  opcode          - An AML opcode
 *
 * RETURN:      Size of the opcode, in bytes (1 or 2)
 *
 * DESCRIPTION: Get the size of the current opcode.
 *
 ******************************************************************************/
u32 acpi_ps_get_opcode_size(u32 opcode)
{

	/* Extended (2-byte) opcode if > 255 */

	if (opcode > 0x00FF) {
		return (2);
	}

	/* Otherwise, just a single byte opcode */

	return (1);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_peek_opcode
 *
 * PARAMETERS:  parser_state        - A parser state object
 *
 * RETURN:      Next AML opcode
 *
 * DESCRIPTION: Get next AML opcode (without incrementing AML pointer)
 *
 ******************************************************************************/

u16 acpi_ps_peek_opcode(struct acpi_parse_state * parser_state)
{
	u8 *aml;
	u16 opcode;

	aml = parser_state->aml;
	opcode = (u16) ACPI_GET8(aml);

	if (opcode == AML_EXTENDED_PREFIX) {

		/* Extended opcode, get the second opcode byte */

		aml++;
		opcode = (u16) ((opcode << 8) | ACPI_GET8(aml));
	}

	return (opcode);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_complete_this_op
 *
 * PARAMETERS:  walk_state      - Current State
 *              op              - Op to complete
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Perform any cleanup at the completion of an Op.
 *
 ******************************************************************************/
/*
 * acpi_ps_complete_this_op - 完成并清理当前解析操作(Op)及其子树
 * @walk_state: ACPI解析状态机上下文
 * @op: 要完成的解析操作节点
 *
 * 功能说明：
 * 1. 检查并处理空Op情况(AML代码损坏时可能发生)
 * 2. 停止该Op的跟踪记录
 * 3. 根据解析标志决定是否删除Op及其子树
 * 4. 对需要保留的子树用占位符Op替换
 * 5. 从父节点链表中正确解除关联
 * 6. 最终删除Op及其子树
 *
 * 特殊处理：
 * - 控制类(AML_CLASS_CONTROL)Op保持原样
 * - 创建类(AML_CLASS_CREATE)Op用返回值占位符替换
 * - 命名对象类(AML_CLASS_NAMED_OBJECT)根据具体类型处理
 *
 * 返回值：
 * - AE_OK: 操作成功完成
 * - AE_NO_MEMORY: 内存分配失败
 */
acpi_status
acpi_ps_complete_this_op(struct acpi_walk_state *walk_state,
			 union acpi_parse_object *op)
{
	union acpi_parse_object *prev;//前一个同级Op指针
	union acpi_parse_object *next;//下一个同级Op指针
	const struct acpi_opcode_info *parent_info;//父节点操作码信息结构体指针
	union acpi_parse_object *replacement_op = NULL;//替换Op指针
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE_PTR(ps_complete_this_op, op);

	/* 检查空Op情况(可能由于AML代码损坏) */
	if (!op) {
		return_ACPI_STATUS(AE_OK);	/* 暂时返回OK */
	}

	acpi_ex_stop_trace_opcode(op, walk_state);//停止该Op的跟踪

        /*
         * 检查是否需要删除当前Op及其子树：
         * 1. 检查parse_flags中的树处理标志
         * 2. 排除参数类(ARGUMENT)Op
         */
	if (((walk_state->parse_flags & ACPI_PARSE_TREE_MASK) !=
	     ACPI_PARSE_DELETE_TREE)
	    || (walk_state->op_info->class == AML_CLASS_ARGUMENT)) {
		return_ACPI_STATUS(AE_OK);//不需要删除则直接返回成功
	}

	/* 确保我们只删除这个子树 */
	if (op->common.parent) {//如果当前op存在父节点
		prev = op->common.parent->common.value.arg;//获取父节点的第一个节点
		if (!prev) {//如果父节点没有子节点，直接跳转到清理阶段

			/* Nothing more to do */

			goto cleanup;
		}

		/*
		 * Check if we need to replace the operator and its subtree
		 * with a return value op (placeholder op)
		 */
		parent_info =
		    acpi_ps_get_opcode_info(op->common.parent->common.
					    aml_opcode);//获取父节点的操作码信息

		switch (parent_info->class) {//根据父节点类型决定替换策略
		case AML_CLASS_CONTROL://控制类Op不需要替换，直接跳出switch

			break;

		/*
                 * 创建类Op需要替换为返回值占位符：
                 * 1. 分配新的RETURN_VALUE_OP节点
                 * 2. 使用原Op的AML位置信息
                 */
		case AML_CLASS_CREATE:
			/*
			 * These opcodes contain term_arg operands. The current
			 * op must be replaced by a placeholder return op
			 */
			replacement_op =
			    acpi_ps_alloc_op(AML_INT_RETURN_VALUE_OP,
					     op->common.aml);//分配RETURN_VALUE_OP节点，用于替换
			if (!replacement_op) {
				status = AE_NO_MEMORY;
			}
			break;

		case AML_CLASS_NAMED_OBJECT://命名对象类操作码
                        /*
                         * 命名对象类操作码包含终止参数操作数(term_arg)，
                         * 需要将当前操作码替换为占位符返回操作码
                         * 
                         * 这类操作码通常用于定义ACPI命名空间中的对象，
                         * 需要特殊处理以确保命名空间一致性
                         */
                        
                        /* 
                         * 第一组条件检查：处理需要特殊替换的父操作码类型
                         * 这些操作码定义的区域/缓冲区/包等对象需要保留占位符，
                         * 即使删除子树也要保持对象引用有效性
                         */
			if ((op->common.parent->common.aml_opcode ==
			     AML_REGION_OP)//操作区定义
			    || (op->common.parent->common.aml_opcode ==
				AML_DATA_REGION_OP)//数据区定义
			    || (op->common.parent->common.aml_opcode ==
				AML_BUFFER_OP)//缓冲区定义
			    || (op->common.parent->common.aml_opcode ==
				AML_PACKAGE_OP)//固定长度包
			    || (op->common.parent->common.aml_opcode ==
				AML_BANK_FIELD_OP)//bank字段
			    || (op->common.parent->common.aml_opcode ==
				AML_VARIABLE_PACKAGE_OP)) {//可变长度包
				replacement_op =
				    acpi_ps_alloc_op(AML_INT_RETURN_VALUE_OP,
						     op->common.aml);//为这些类型创建返回值占位符
				if (!replacement_op) {
					status = AE_NO_MEMORY;
				}
			} else 
                        /* 
                         * 第二组条件检查：特殊处理NAME_OP在加载阶段的缓冲区和包
                         * 
                         * 在ACPI表加载过程中(PASS1/PASS2)，对于NAME操作码下的
                         * 缓冲区/包定义需要保留原操作码类型而非替换为RETURN_VALUE，
                         * 以确保正确的命名空间初始化
                         */
			    if ((op->common.parent->common.aml_opcode ==
				 AML_NAME_OP)//命名对象定义
				&& (walk_state->pass_number <=
				    ACPI_IMODE_LOAD_PASS2)) {//加载阶段检查
							
				/* 检查当前操作码是否为需要保留的类型 */
				if ((op->common.aml_opcode == AML_BUFFER_OP)//缓冲区操作码
				    || (op->common.aml_opcode == AML_PACKAGE_OP)//固定长度包
				    || (op->common.aml_opcode ==
					AML_VARIABLE_PACKAGE_OP)) {//可变长度包
					replacement_op =
					    acpi_ps_alloc_op(op->common.
							     aml_opcode,
							     op->common.aml);//分配相同类型的操作码(而非RETURN_VALUE),保持原操作码类型和AML位置
					if (!replacement_op) {
						status = AE_NO_MEMORY;
					} else {
                                                /*
                                                 * 成功分配后复制关键数据字段：
                                                 * - named.data: 存储对象初始化数据
                                                 * - named.length: 数据长度
                                                 * 
                                                 * 这些字段在加载阶段用于构建命名空间对象
                                                 */
						replacement_op->named.data =
						    op->named.data;
						replacement_op->named.length =
						    op->named.length;
					}
				}
			}
			break;

		default:

			replacement_op =
			    acpi_ps_alloc_op(AML_INT_RETURN_VALUE_OP,
					     op->common.aml);//默认情况创建返回值占位符
			if (!replacement_op) {
				status = AE_NO_MEMORY;
			}
		}

		/* 我们必须取消此 op 与父树的链接 */
		if (prev == op) {//prev是同级的第一个节点

			/* 情况1：当前Op是链表中第一个节点 */
			if (replacement_op) {
				replacement_op->common.parent =
				    op->common.parent;//设置替换节点的父指针
				replacement_op->common.value.arg = NULL;//初始化替换节点的参数列表
				replacement_op->common.node = op->common.node;//复制原节点的命名空间节点指针
				op->common.parent->common.value.arg =
				    replacement_op;//更新父节点的第一个参数指针
				replacement_op->common.next = op->common.next;//保持链表连续性
			} else {
				op->common.parent->common.value.arg =
				    op->common.next;//没有替换节点时直接跳过当前节点
			}
		}

		/* Search the parent list */

		else/* 情况2：当前Op在链表中间位置 */
			while (prev) {

				/* Traverse all siblings in the parent's argument list */

				next = prev->common.next;//获取下一个节点
				/* 找到当前Op的位置 */
				if (next == op) {
					if (replacement_op) {
						replacement_op->common.parent =
						    op->common.parent;//设置替换节点的父指针
						replacement_op->common.value.
						    arg = NULL;//初始化参数列表
						replacement_op->common.node =
						    op->common.node;//复制命名空间节点
						prev->common.next =
						    replacement_op;//在前驱节点后插入替换节点
						replacement_op->common.next =
						    op->common.next;//保持链表连续性
						next = NULL;//标记处理完成
					} else {
						prev->common.next =
						    op->common.next;//直接跳过当前节点
						next = NULL;
					}
				}
				prev = next;//移动到下一个节点
			}
	}

cleanup:

	/* Now we can actually delete the subtree rooted at Op */

	acpi_ps_delete_parse_tree(op);//删除当前Op及其子树
	return_ACPI_STATUS(status);//返回处理状态
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_next_parse_state
 *
 * PARAMETERS:  walk_state          - Current state
 *              op                  - Current parse op
 *              callback_status     - Status from previous operation
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Update the parser state based upon the return exception from
 *              the parser callback.
 *
 ******************************************************************************/

acpi_status
acpi_ps_next_parse_state(struct acpi_walk_state *walk_state,
			 union acpi_parse_object *op,
			 acpi_status callback_status)
{
	struct acpi_parse_state *parser_state = &walk_state->parser_state;
	acpi_status status = AE_CTRL_PENDING;

	ACPI_FUNCTION_TRACE_PTR(ps_next_parse_state, op);

	switch (callback_status) {
	case AE_CTRL_TERMINATE:
		/*
		 * A control method was terminated via a RETURN statement.
		 * The walk of this method is complete.
		 */
		parser_state->aml = parser_state->aml_end;
		status = AE_CTRL_TERMINATE;
		break;

	case AE_CTRL_BREAK:

		parser_state->aml = walk_state->aml_last_while;
		walk_state->control_state->common.value = FALSE;
		status = AE_CTRL_BREAK;
		break;

	case AE_CTRL_CONTINUE:

		parser_state->aml = walk_state->aml_last_while;
		status = AE_CTRL_CONTINUE;
		break;

	case AE_CTRL_PENDING:

		parser_state->aml = walk_state->aml_last_while;
		break;

#if 0
	case AE_CTRL_SKIP:

		parser_state->aml = parser_state->scope->parse_scope.pkg_end;
		status = AE_OK;
		break;
#endif

	case AE_CTRL_TRUE:
		/*
		 * Predicate of an IF was true, and we are at the matching ELSE.
		 * Just close out this package
		 */
		parser_state->aml = acpi_ps_get_next_package_end(parser_state);
		status = AE_CTRL_PENDING;
		break;

	case AE_CTRL_FALSE:
		/*
		 * Either an IF/WHILE Predicate was false or we encountered a BREAK
		 * opcode. In both cases, we do not execute the rest of the
		 * package;  We simply close out the parent (finishing the walk of
		 * this branch of the tree) and continue execution at the parent
		 * level.
		 */
		parser_state->aml = parser_state->scope->parse_scope.pkg_end;

		/* In the case of a BREAK, just force a predicate (if any) to FALSE */

		walk_state->control_state->common.value = FALSE;
		status = AE_CTRL_END;
		break;

	case AE_CTRL_TRANSFER:

		/* A method call (invocation) -- transfer control */

		status = AE_CTRL_TRANSFER;
		walk_state->prev_op = op;
		walk_state->method_call_op = op;
		walk_state->method_call_node =
		    (op->common.value.arg)->common.node;

		/* Will return value (if any) be used by the caller? */

		walk_state->return_used =
		    acpi_ds_is_result_used(op, walk_state);
		break;

	default:

		status = callback_status;
		if (ACPI_CNTL_EXCEPTION(callback_status)) {
			status = AE_OK;
		}
		break;
	}

	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_parse_aml
 *
 * PARAMETERS:  walk_state      - Current state
 *
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Parse raw AML and return a tree of ops
 *
 ******************************************************************************/

acpi_status acpi_ps_parse_aml(struct acpi_walk_state *walk_state)//解析并执行AML代码流
{
	acpi_status status;
	struct acpi_thread_state *thread;//新线程状态指针
	struct acpi_thread_state *prev_walk_list = acpi_gbl_current_walk_list;//保存原全局线程列表
	struct acpi_walk_state *previous_walk_state;//前一个walk状态缓存（未使用）

	ACPI_FUNCTION_TRACE(ps_parse_aml);

	ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
			  "Entered with WalkState=%p Aml=%p size=%X\n",
			  walk_state, walk_state->parser_state.aml,
			  walk_state->parser_state.aml_size));//调试输出当前解析状态

	/* AML指针有效性验证 */
	if (!walk_state->parser_state.aml) {//空指针检查
		return_ACPI_STATUS(AE_BAD_ADDRESS);//返回地址错误
	}

	/* Create and initialize a new thread state */
	/* 创建新的线程状态（用于方法执行上下文） */
	thread = acpi_ut_create_thread_state();//从对象缓存分配线程状态
	if (!thread) {//如果分配失败
		if (walk_state->method_desc) {//如果正在执行控制方法：

			/* Executing a control method - additional cleanup */
			/* 控制方法额外清理：释放方法相关资源 */
			acpi_ds_terminate_control_method(walk_state->
							 method_desc,
							 walk_state);
		}

		acpi_ds_delete_walk_state(walk_state);//删除walk状态对象
		return_ACPI_STATUS(AE_NO_MEMORY);//返回内存不足错误
	}

	walk_state->thread = thread;//关联线程状态到walk状态

	/*
	 * If executing a method, the starting sync_level is this method's
	 * sync_level
	 * 如果执行一个方法，起始的同步级别是这个方法本身的同步级别
	 */
	if (walk_state->method_desc) {//如果存在方法描述符
		walk_state->thread->current_sync_level =
		    walk_state->method_desc->method.sync_level;//设置初始同步级别
	}

	acpi_ds_push_walk_state(walk_state, thread);//将walk状态压入线程栈（支持嵌套方法调用）

	/*
	 * This global allows the AML debugger to get a handle to the currently
	 * executing control method.
	 * 这个全局变量允许AML调试器获取当前正在执行的控制方法的句柄。
	 */
	acpi_gbl_current_walk_list = thread;//设置全局当前线程

	/*
	 * Execute the walk loop as long as there is a valid Walk State. This
	 * handles nested control method invocations without recursion.
	 * 通过循环处理walk状态而非递归，避免栈溢出
	 */
	ACPI_DEBUG_PRINT((ACPI_DB_PARSE, "State=%p\n", walk_state));

	status = AE_OK;
	while (walk_state) {//循环条件：只要存在walk_state（方法执行上下文），就持续处理
		if (ACPI_SUCCESS(status)) {//如果上一步执行成功（status为AE_OK），则继续解析AML指令
			/*
                         * parse_loop 执行 AML（ACPI Machine Language）代码，
                         * 直到该方法终止或者调用另一个方法为止。
                         */
			status = acpi_ps_parse_loop(walk_state);//调用核心解析函数执行AML代码流
		}

		/* 打印调试信息，显示当前状态和返回状态码 */
		ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
				  "Completed one call to walk loop, %s State=%p\n",
				  acpi_format_exception(status), walk_state));

		if (walk_state->method_pathname && walk_state->method_is_nested) {//如果当前是嵌套方法且存在路径名，则执行清理

			/* Optional object evaluation log */

			ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
					      "%-26s:  %*s%s\n",
					      "   Exit nested method",
					      (walk_state->
					       method_nesting_depth + 1) * 3,
					      " ",
					      &walk_state->method_pathname[1]));

			ACPI_FREE(walk_state->method_pathname);//释放路径名字符串的内存
			walk_state->method_is_nested = FALSE;//标记退出嵌套状态
		}
		if (status == AE_CTRL_TRANSFER) {//如果需要转移控制到被调用方法
			/*
			 * A method call was detected.
			 * Transfer control to the called control method
			 */
			status =
			    acpi_ds_call_control_method(thread, walk_state,
							NULL);//创建新walk_state执行被调用方法，参数为当前线程和当前状态
			if (ACPI_FAILURE(status)) {//如果调用失败
				status =
				    acpi_ds_method_error(status, walk_state);// 处理方法错误（如参数无效或方法不存在）
			}

			/*
			 * If the transfer to the new method method call worked,
			 * a new walk state was created -- get it
			 */
			walk_state = acpi_ds_get_current_walk_state(thread);//获取新方法的walk_state（调用成功时创建新状态）
			continue;
		} else if (status == AE_CTRL_TERMINATE) {//如果方法正常终止
			status = AE_OK;//将终止状态重置为成功
		} else if ((status != AE_OK) && (walk_state->method_desc)) {//如果执行失败且存在方法描述符

			/* Either the method parse or actual execution failed */

			acpi_ex_exit_interpreter();//退出ACPI解释器临界区（解锁互斥锁）
			if (status == AE_ABORT_METHOD) {//如果是显式终止错误
				acpi_ns_print_node_pathname(walk_state->
							    method_node,
							    "Aborting method");//打印方法路径
				acpi_os_printf("\n");
			} else {//如果是其他错误
				ACPI_ERROR_METHOD("Aborting method",
						  walk_state->method_node, NULL,
						  status);//记录带方法路径的错误信息
			}
			acpi_ex_enter_interpreter();//重新进入解释器临界区

			/* Check for possible multi-thread reentrancy problem */

			if ((status == AE_ALREADY_EXISTS) &&
			    (!(walk_state->method_desc->method.info_flags &
			       ACPI_METHOD_SERIALIZED))) {//如果错误是对象已存在且方法未序列化
				/*
				 * Method is not serialized and tried to create an object
				 * twice. The probable cause is that the method cannot
				 * handle reentrancy. Mark as "pending serialized" now, and
				 * then mark "serialized" when the last thread exits.
				 */
				walk_state->method_desc->method.info_flags |=
				    ACPI_METHOD_SERIALIZED_PENDING;//标记方法需序列化（防止后续并发调用）
			}
		}

		/* We are done with this walk, move on to the parent if any */

		walk_state = acpi_ds_pop_walk_state(thread);//从线程状态栈弹出当前walk_state，返回父方法上下文

		/* Reset the current scope to the beginning of scope stack */

		acpi_ds_scope_stack_clear(walk_state);//清空作用域栈，重置到初始作用域

		/*
		 * If we just returned from the execution of a control method or if we
		 * encountered an error during the method parse phase, there's lots of
		 * cleanup to do
		 */
		if (((walk_state->parse_flags & ACPI_PARSE_MODE_MASK) ==
		     ACPI_PARSE_EXECUTE &&//检查当前是否处于执行模式
		     !(walk_state->parse_flags & ACPI_PARSE_MODULE_LEVEL)) ||//确保非模块级解析阶段
		    (ACPI_FAILURE(status))) {//或者方法执行过程中发生错误
			acpi_ds_terminate_control_method(walk_state->
							 method_desc,
							 walk_state);//清理方法资源（释放对象、解锁节点等）
		}

		/* Delete this walk state and all linked control states */

		acpi_ps_cleanup_scope(&walk_state->parser_state);//清理解析器状态（释放解析栈资源）
		previous_walk_state = walk_state;//保存当前状态供后续使用

		ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
				  "ReturnValue=%p, ImplicitValue=%p State=%p\n",
				  walk_state->return_desc,
				  walk_state->implicit_return_obj, walk_state));//打印方法返回值和隐式返回值的调试信息

		/* Check if we have restarted a preempted walk */
		/* 检查当前是否需要重启一个被抢占的walk_state（方法执行上下文）
		 * 例如，当方法执行被中断（如信号量等待）后需要恢复执行
		 */
		walk_state = acpi_ds_get_current_walk_state(thread);//获取当前线程的walk_state（当前正在执行的ACPI方法状态）
		if (walk_state) {//如果walk_state存在，说明有未完成的方法需要继续执行
			if (ACPI_SUCCESS(status)) {//如果上一步执行成功（status为AE_OK）
				/*
				 * There is another walk state, restart it.
				 * If the method return value is not used by the parent,
				 * The object is deleted
				 */
				if (!previous_walk_state->return_desc) {//如果父方法没有显式返回值（return_desc为空）
					/*
					 * In slack mode execution, if there is no return value
					 * we should implicitly return zero (0) as a default value.
					 */
					if (acpi_gbl_enable_interpreter_slack &&
					    !previous_walk_state->
					    implicit_return_obj) {// 如果启用了解释器松弛模式（允许默认返回值）且未设置隐式返回值
						previous_walk_state->
						    implicit_return_obj =
						    acpi_ut_create_integer_object
						    ((u64) 0);//创建默认返回值对象（0整数），作为隐式返回值
						if (!previous_walk_state->
						    implicit_return_obj) {//如果创建失败，返回内存不足错误
							return_ACPI_STATUS
							    (AE_NO_MEMORY);
						}
					}

					/* Restart the calling control method */

					status =
					    acpi_ds_restart_control_method
					    (walk_state,
					     previous_walk_state->
					     implicit_return_obj);//重启被抢占的方法，使用隐式返回值（如默认0）作为返回值
				} else {//如果父方法有显式返回值（return_desc存在）
					/*
					 * We have a valid return value, delete any implicit
					 * return value.
					 */
					acpi_ds_clear_implicit_return
					    (previous_walk_state);//清除隐式返回值（优先使用显式返回值）

					status =
					    acpi_ds_restart_control_method
					    (walk_state,
					     previous_walk_state->return_desc);//重启方法，使用显式返回值（return_desc）作为参数
				}
				if (ACPI_SUCCESS(status)) {//如果重启成功
					walk_state->walk_type |=
					    ACPI_WALK_METHOD_RESTART;//标记walk_state为重启状态（ACPI_WALK_METHOD_RESTART）
				}
			} else {//如果执行失败（status非AE_OK）
				/* On error, delete any return object or implicit return */

				acpi_ut_remove_reference(previous_walk_state->
							 return_desc);//释放显式返回对象的引用计数（自动释放内存）
				acpi_ds_clear_implicit_return
				    (previous_walk_state);//清除隐式返回值对象
			}
		}

		/*
		 * Just completed a 1st-level method, save the final internal return
		 * value (if any)
		 */
		else if (previous_walk_state->caller_return_desc) {//如果当前walk_state的父方法（caller）期望接收返回值（caller_return_desc非空），caller_return_desc是父方法用于存储返回值的指针
			if (previous_walk_state->implicit_return_obj) {//如果存在隐式返回值（如默认值或松弛模式生成的值）
				*(previous_walk_state->caller_return_desc) =
				    previous_walk_state->implicit_return_obj;//将隐式返回值赋给父方法的返回值指针
			} else {//否则，使用显式返回值（通过Return指令指定的值）
				/* NULL if no return value */

				*(previous_walk_state->caller_return_desc) =
				    previous_walk_state->return_desc;//将显式返回值赋给父方法的返回值指针
			}
		} else {//如果父方法不期望接收返回值（caller_return_desc为空）
			if (previous_walk_state->return_desc) {//如果存在显式返回值

				/* Caller doesn't want it, must delete it */

				acpi_ut_remove_reference(previous_walk_state->
							 return_desc);//释放显式返回值对象的引用计数，若计数为0则释放内存
			}
			if (previous_walk_state->implicit_return_obj) {//如果存在隐式返回值

				/* Caller doesn't want it, must delete it */

				acpi_ut_remove_reference(previous_walk_state->
							 implicit_return_obj);//释放隐式返回值对象的引用计数
			}
		}

		acpi_ds_delete_walk_state(previous_walk_state);//删除当前walk_state结构体，结束该方法的执行上下文
	}

	/* Normal exit */

	acpi_ex_release_all_mutexes(thread);//释放线程持有的所有互斥锁（如方法执行锁、命名空间锁等）
	acpi_ut_delete_generic_state(ACPI_CAST_PTR
				     (union acpi_generic_state, thread));// 释放线程的通用状态结构体（如参数、局部变量等）
	acpi_gbl_current_walk_list = prev_walk_list;//恢复线程之前的walk_state列表指针（可能在方法调用时被修改）
	return_ACPI_STATUS(status);//返回最终状态码（如AE_OK或错误码）
}
