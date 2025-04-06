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

acpi_status
acpi_ps_complete_this_op(struct acpi_walk_state *walk_state,
			 union acpi_parse_object *op)
{
	union acpi_parse_object *prev;
	union acpi_parse_object *next;
	const struct acpi_opcode_info *parent_info;
	union acpi_parse_object *replacement_op = NULL;
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE_PTR(ps_complete_this_op, op);

	/* Check for null Op, can happen if AML code is corrupt */

	if (!op) {
		return_ACPI_STATUS(AE_OK);	/* OK for now */
	}

	acpi_ex_stop_trace_opcode(op, walk_state);

	/* Delete this op and the subtree below it if asked to */

	if (((walk_state->parse_flags & ACPI_PARSE_TREE_MASK) !=
	     ACPI_PARSE_DELETE_TREE)
	    || (walk_state->op_info->class == AML_CLASS_ARGUMENT)) {
		return_ACPI_STATUS(AE_OK);
	}

	/* Make sure that we only delete this subtree */

	if (op->common.parent) {
		prev = op->common.parent->common.value.arg;
		if (!prev) {

			/* Nothing more to do */

			goto cleanup;
		}

		/*
		 * Check if we need to replace the operator and its subtree
		 * with a return value op (placeholder op)
		 */
		parent_info =
		    acpi_ps_get_opcode_info(op->common.parent->common.
					    aml_opcode);

		switch (parent_info->class) {
		case AML_CLASS_CONTROL:

			break;

		case AML_CLASS_CREATE:
			/*
			 * These opcodes contain term_arg operands. The current
			 * op must be replaced by a placeholder return op
			 */
			replacement_op =
			    acpi_ps_alloc_op(AML_INT_RETURN_VALUE_OP,
					     op->common.aml);
			if (!replacement_op) {
				status = AE_NO_MEMORY;
			}
			break;

		case AML_CLASS_NAMED_OBJECT:
			/*
			 * These opcodes contain term_arg operands. The current
			 * op must be replaced by a placeholder return op
			 */
			if ((op->common.parent->common.aml_opcode ==
			     AML_REGION_OP)
			    || (op->common.parent->common.aml_opcode ==
				AML_DATA_REGION_OP)
			    || (op->common.parent->common.aml_opcode ==
				AML_BUFFER_OP)
			    || (op->common.parent->common.aml_opcode ==
				AML_PACKAGE_OP)
			    || (op->common.parent->common.aml_opcode ==
				AML_BANK_FIELD_OP)
			    || (op->common.parent->common.aml_opcode ==
				AML_VARIABLE_PACKAGE_OP)) {
				replacement_op =
				    acpi_ps_alloc_op(AML_INT_RETURN_VALUE_OP,
						     op->common.aml);
				if (!replacement_op) {
					status = AE_NO_MEMORY;
				}
			} else
			    if ((op->common.parent->common.aml_opcode ==
				 AML_NAME_OP)
				&& (walk_state->pass_number <=
				    ACPI_IMODE_LOAD_PASS2)) {
				if ((op->common.aml_opcode == AML_BUFFER_OP)
				    || (op->common.aml_opcode == AML_PACKAGE_OP)
				    || (op->common.aml_opcode ==
					AML_VARIABLE_PACKAGE_OP)) {
					replacement_op =
					    acpi_ps_alloc_op(op->common.
							     aml_opcode,
							     op->common.aml);
					if (!replacement_op) {
						status = AE_NO_MEMORY;
					} else {
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
					     op->common.aml);
			if (!replacement_op) {
				status = AE_NO_MEMORY;
			}
		}

		/* We must unlink this op from the parent tree */

		if (prev == op) {

			/* This op is the first in the list */

			if (replacement_op) {
				replacement_op->common.parent =
				    op->common.parent;
				replacement_op->common.value.arg = NULL;
				replacement_op->common.node = op->common.node;
				op->common.parent->common.value.arg =
				    replacement_op;
				replacement_op->common.next = op->common.next;
			} else {
				op->common.parent->common.value.arg =
				    op->common.next;
			}
		}

		/* Search the parent list */

		else
			while (prev) {

				/* Traverse all siblings in the parent's argument list */

				next = prev->common.next;
				if (next == op) {
					if (replacement_op) {
						replacement_op->common.parent =
						    op->common.parent;
						replacement_op->common.value.
						    arg = NULL;
						replacement_op->common.node =
						    op->common.node;
						prev->common.next =
						    replacement_op;
						replacement_op->common.next =
						    op->common.next;
						next = NULL;
					} else {
						prev->common.next =
						    op->common.next;
						next = NULL;
					}
				}
				prev = next;
			}
	}

cleanup:

	/* Now we can actually delete the subtree rooted at Op */

	acpi_ps_delete_parse_tree(op);
	return_ACPI_STATUS(status);
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

acpi_status acpi_ps_parse_aml(struct acpi_walk_state *walk_state)
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
			 * The parse_loop executes AML until the method terminates
			 * or calls another method.
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
