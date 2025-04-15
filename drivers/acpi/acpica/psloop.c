// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: psloop - Main AML parse loop
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

/*
 * Parse the AML and build an operation tree as most interpreters, (such as
 * Perl) do. Parsing is done by hand rather than with a YACC generated parser
 * to tightly constrain stack and dynamic memory usage. Parsing is kept
 * flexible and the code fairly compact by parsing based on a list of AML
 * opcode templates in aml_op_info[].
 */

#include <acpi/acpi.h>
#include "accommon.h"
#include "acinterp.h"
#include "acparser.h"
#include "acdispat.h"
#include "amlcode.h"
#include "acconvert.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_PARSER
ACPI_MODULE_NAME("psloop")

/* Local prototypes */
static acpi_status
acpi_ps_get_arguments(struct acpi_walk_state *walk_state,
		      u8 * aml_op_start, union acpi_parse_object *op);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_get_arguments
 *
 * PARAMETERS:  walk_state          - Current state
 *              aml_op_start        - Op start in AML
 *              op                  - Current Op
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Get arguments for passed Op.
 *
 ******************************************************************************/

static acpi_status
acpi_ps_get_arguments(struct acpi_walk_state *walk_state,
		      u8 * aml_op_start, union acpi_parse_object *op)
{
	acpi_status status = AE_OK;
	union acpi_parse_object *arg = NULL;

	ACPI_FUNCTION_TRACE_PTR(ps_get_arguments, walk_state);

	ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
			  "Get arguments for opcode [%s]\n",
			  op->common.aml_op_name));

	switch (op->common.aml_opcode) {
	case AML_BYTE_OP:	/* AML_BYTEDATA_ARG */
	case AML_WORD_OP:	/* AML_WORDDATA_ARG */
	case AML_DWORD_OP:	/* AML_DWORDATA_ARG */
	case AML_QWORD_OP:	/* AML_QWORDATA_ARG */
	case AML_STRING_OP:	/* AML_ASCIICHARLIST_ARG */

		/* Fill in constant or string argument directly */

		acpi_ps_get_next_simple_arg(&(walk_state->parser_state),
					    GET_CURRENT_ARG_TYPE(walk_state->
								 arg_types),
					    op);
		break;

	case AML_INT_NAMEPATH_OP:	/* AML_NAMESTRING_ARG */

		status = acpi_ps_get_next_namepath(walk_state,
						   &(walk_state->parser_state),
						   op,
						   ACPI_POSSIBLE_METHOD_CALL);
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}

		walk_state->arg_types = 0;
		break;

	default:
		/*
		 * Op is not a constant or string, append each argument to the Op
		 */
		while (GET_CURRENT_ARG_TYPE(walk_state->arg_types) &&
		       !walk_state->arg_count) {
			walk_state->aml = walk_state->parser_state.aml;

			switch (op->common.aml_opcode) {
			case AML_METHOD_OP:
			case AML_BUFFER_OP:
			case AML_PACKAGE_OP:
			case AML_VARIABLE_PACKAGE_OP:
			case AML_WHILE_OP:

				break;

			default:

				ASL_CV_CAPTURE_COMMENTS(walk_state);
				break;
			}

			status =
			    acpi_ps_get_next_arg(walk_state,
						 &(walk_state->parser_state),
						 GET_CURRENT_ARG_TYPE
						 (walk_state->arg_types), &arg);
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}

			if (arg) {
				acpi_ps_append_arg(op, arg);
			}

			INCREMENT_ARG_LIST(walk_state->arg_types);
		}

		ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
				  "Final argument count: %8.8X pass %u\n",
				  walk_state->arg_count,
				  walk_state->pass_number));

		/* Special processing for certain opcodes */

		switch (op->common.aml_opcode) {
		case AML_METHOD_OP:
			/*
			 * Skip parsing of control method because we don't have enough
			 * info in the first pass to parse it correctly.
			 *
			 * Save the length and address of the body
			 */
			op->named.data = walk_state->parser_state.aml;
			op->named.length = (u32)
			    (walk_state->parser_state.pkg_end -
			     walk_state->parser_state.aml);

			/* Skip body of method */

			walk_state->parser_state.aml =
			    walk_state->parser_state.pkg_end;
			walk_state->arg_count = 0;
			break;

		case AML_BUFFER_OP:
		case AML_PACKAGE_OP:
		case AML_VARIABLE_PACKAGE_OP:

			if ((op->common.parent) &&
			    (op->common.parent->common.aml_opcode ==
			     AML_NAME_OP)
			    && (walk_state->pass_number <=
				ACPI_IMODE_LOAD_PASS2)) {
				ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
						  "Setup Package/Buffer: Pass %u, AML Ptr: %p\n",
						  walk_state->pass_number,
						  aml_op_start));

				/*
				 * Skip parsing of Buffers and Packages because we don't have
				 * enough info in the first pass to parse them correctly.
				 */
				op->named.data = aml_op_start;
				op->named.length = (u32)
				    (walk_state->parser_state.pkg_end -
				     aml_op_start);

				/* Skip body */

				walk_state->parser_state.aml =
				    walk_state->parser_state.pkg_end;
				walk_state->arg_count = 0;
			}
			break;

		case AML_WHILE_OP:

			if (walk_state->control_state) {
				walk_state->control_state->control.package_end =
				    walk_state->parser_state.pkg_end;
			}
			break;

		default:

			/* No action for all other opcodes */

			break;
		}

		break;
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_parse_loop
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Parse AML (pointed to by the current parser state) and return
 *              a tree of ops.
 *
 ******************************************************************************/

acpi_status acpi_ps_parse_loop(struct acpi_walk_state *walk_state)//核心解析循环函数，负责解析并执行AML字节码。该函数处理控制方法的重新启动（如被抢占后恢复）、条件分支判断、作用域管理等核心逻辑。
{
	acpi_status status = AE_OK;//函数返回状态码
	union acpi_parse_object *op = NULL;//当前解析树操作节点
	struct acpi_parse_state *parser_state;//解析器状态结构指针
	u8 *aml_op_start = NULL;//AML操作的起始字节（初始化为0）
	u8 opcode_length;// 当前操作码长度（用于解析）

	ACPI_FUNCTION_TRACE_PTR(ps_parse_loop, walk_state);

	if (walk_state->descending_callback == NULL) {//参数有效性检查：回调函数必须存在
		return_ACPI_STATUS(AE_BAD_PARAMETER);//返回无效参数错误
	}

	parser_state = &walk_state->parser_state;//获取解析器状态指针（指向walk_state的成员）
	walk_state->arg_types = 0;//初始化参数类型计数器

#ifndef ACPI_CONSTANT_EVAL_ONLY//非常量评估模式（默认启用）

	if (walk_state->walk_type & ACPI_WALK_METHOD_RESTART) {//如果方法需要重新启动（如被抢占后恢复）

		/* We are restarting a preempted control method */

		if (acpi_ps_has_completed_scope(parser_state)) {//当前作用域已解析完毕
			/*
			 * We must check if a predicate to an IF or WHILE statement
			 * was just completed
			 */
			if ((parser_state->scope->parse_scope.op) &&//如果当前作用域存在操作节点
			    ((parser_state->scope->parse_scope.op->common.
			      aml_opcode == AML_IF_OP)
			     || (parser_state->scope->parse_scope.op->common.
				 aml_opcode == AML_WHILE_OP))//并且操作类型为IF或WHILE
			    && (walk_state->control_state)//并且存在控制状态（如分支条件）
			    && (walk_state->control_state->common.state ==
				ACPI_CONTROL_PREDICATE_EXECUTING)) {//并且控制状态处于条件执行阶段
				/*
				 * A predicate was just completed, get the value of the
				 * predicate and branch based on that value
				 */
				walk_state->op = NULL;//重置当前操作指针
				status =
				    acpi_ds_get_predicate_value(walk_state,
								ACPI_TO_POINTER
								(TRUE));// 获取条件表达式结果
				if (ACPI_FAILURE(status)
				    && !ACPI_CNTL_EXCEPTION(status)) {//获取失败且非控制异常
					if (status == AE_AML_NO_RETURN_VALUE) {//如果条件表达式未返回值
						ACPI_EXCEPTION((AE_INFO, status,
								"Invoked method did not return a value"));
					}

					ACPI_EXCEPTION((AE_INFO, status,
							"GetPredicate Failed"));
					return_ACPI_STATUS(status);
				}

				status =
				    acpi_ps_next_parse_state(walk_state, op,
							     status);//根据条件值跳转到下一个解析状态
			}

			acpi_ps_pop_scope(parser_state, &op,
					  &walk_state->arg_types,
					  &walk_state->arg_count);//弹出当前作用域并更新参数信息
			ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
					  "Popped scope, Op=%p\n", op));//调试输出弹出的操作节点
		} else if (walk_state->prev_op) {//如果存在未完成的操作（如中断时的中间状态）

			/* We were in the middle of an op */

			op = walk_state->prev_op;//恢复前一个操作节点
			walk_state->arg_types = walk_state->prev_arg_types;// 恢复参数类型
		}
	}
#endif

	/* 迭代解析循环，只要还有 AML 代码需要处理，就持续解析 */
	while ((parser_state->aml < parser_state->aml_end) || (op)) {//循环条件：AML未解析完毕或存在未处理的操作节点
		ASL_CV_CAPTURE_COMMENTS(walk_state);

		aml_op_start = parser_state->aml;// 记录当前操作码的起始地址
		if (!op) {//如果op为空
			status =
			    acpi_ps_create_op(walk_state, aml_op_start, &op);//创建并初始化操作节点
			if (ACPI_FAILURE(status)) {//如果创建失败
				/*
				 * ACPI_PARSE_MODULE_LEVEL means that we are loading a table by
				 * executing it as a control method. However, if we encounter
				 * an error while loading the table, we need to keep trying to
				 * load the table rather than aborting the table load. Set the
				 * status to AE_OK to proceed with the table load.
				 */
				if ((walk_state->
				     parse_flags & ACPI_PARSE_MODULE_LEVEL)//如果是模块级方法（如DSDT加载）忽略某些错误继续执行
				    && ((status == AE_ALREADY_EXISTS)//并且允许继续加载（如命名冲突）
					|| (status == AE_NOT_FOUND))) {
					status = AE_OK;//强制设为成功，继续解析
				}
				if (status == AE_CTRL_PARSE_CONTINUE) {//如果需要跳过当前指令
					continue;
				}

				if (status == AE_CTRL_PARSE_PENDING) {//如果需要稍后处理
					status = AE_OK;
				}

				if (status == AE_CTRL_TERMINATE) {//如果需要终止解析
					return_ACPI_STATUS(status);
				}

				status =
				    acpi_ps_complete_op(walk_state, &op,
							status);//完成当前Op的解析处理并进行状态转换
				if (ACPI_FAILURE(status)) {
					return_ACPI_STATUS(status);// 返回最终错误码
				}
				if (acpi_ns_opens_scope
				    (acpi_ps_get_opcode_info
				     (walk_state->opcode)->object_type)) {//检查操作码是否为作用域开启指令
					/*
					 * If the scope/device op fails to parse, skip the body of
					 * the scope op because the parse failure indicates that
					 * the device may not exist.
					 */
					ACPI_INFO(("Skipping parse of AML opcode: %s (0x%4.4X)", acpi_ps_get_opcode_name(walk_state->opcode), walk_state->opcode));//记录跳过信息

					/*
					 * Determine the opcode length before skipping the opcode.
					 * An opcode can be 1 byte or 2 bytes in length.
					 * 计算操作码长度（1或2字节）
					 */
					opcode_length = 1;
					if ((walk_state->opcode & 0xFF00) ==
					    AML_EXTENDED_OPCODE) {
						opcode_length = 2;
					}
					/*  跳过当前操作码及其后续所有字节（如整个Scope块） */
					walk_state->parser_state.aml =
					    walk_state->aml + opcode_length;

					walk_state->parser_state.aml =
					    acpi_ps_get_next_package_end
					    (&walk_state->parser_state);//跳转到包结束位置
					walk_state->aml =
					    walk_state->parser_state.aml;
				}

				continue;//跳过当前操作码，继续循环
			}

			acpi_ex_start_trace_opcode(op, walk_state);//记录操作码执行开始（用于调试跟踪）
		}

		/*
		 * Start arg_count at zero because we don't know if there are
		 * any args yet
		 */
		walk_state->arg_count = 0;//初始化参数计数器为0

		switch (op->common.aml_opcode) {//根据当前指令类型处理不同情况
		case AML_BYTE_OP://处理1字节指令（如存储一个字节）
		case AML_WORD_OP://处理2字节指令（如存储一个字）
		case AML_DWORD_OP://处理4字节指令（如存储一个双字）
		case AML_QWORD_OP://处理8字节指令（如存储一个四字）

			break;

		default://其他指令（如条件判断、循环等）

			ASL_CV_CAPTURE_COMMENTS(walk_state);//捕获指令后的注释（比如程序员写的注释）
			break;
		}

		/* Are there any arguments that must be processed? */

		if (walk_state->arg_types) {//检查是否有参数需要处理（比如指令需要输入值）

			/* Get arguments */

			status =
			    acpi_ps_get_arguments(walk_state, aml_op_start, op);//获取参数（比如：从AML代码中读取参数值）
			if (ACPI_FAILURE(status)) {//如果获取参数失败
				status =
				    acpi_ps_complete_op(walk_state, &op,
							status);//完成当前Op的解析处理并进行状态转换
				if (ACPI_FAILURE(status)) {
					return_ACPI_STATUS(status);//失败返回错误码
				}
				if ((walk_state->control_state) &&
				    ((walk_state->control_state->control.
				      opcode == AML_IF_OP)
				     || (walk_state->control_state->control.
					 opcode == AML_WHILE_OP))) {//如果失败的指令是条件判断（如If/While）：
					/*
					 * If the if/while op fails to parse, we will skip parsing
					 * the body of the op.
					 * 跳过整个条件块（比如：跳过If的Then部分）
					 */
					parser_state->aml =
					    walk_state->control_state->control.
					    aml_predicate_start + 1;// 跳过条件块的起始位置 + 1
					parser_state->aml =
					    acpi_ps_get_next_package_end
					    (parser_state);//跳转到条件块结束位置
					walk_state->aml = parser_state->aml;

					ACPI_ERROR((AE_INFO,
						    "Skipping While/If block"));//记录错误信息（比如：跳过If块）
					if (*walk_state->aml == AML_ELSE_OP) {//如果是Else指令
						ACPI_ERROR((AE_INFO,
							    "Skipping Else block"));
						/* 跳过Else块 */
						walk_state->parser_state.aml =
						    walk_state->aml + 1;
						walk_state->parser_state.aml =
						    acpi_ps_get_next_package_end
						    (parser_state);
						walk_state->aml =
						    parser_state->aml;
					}
					ACPI_FREE(acpi_ut_pop_generic_state
						  (&walk_state->control_state));//释放控制状态资源（如If/While的控制信息）
				}
				op = NULL;
				continue;
			}
		}

		/* Check for arguments that need to be processed */

		ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
				  "Parseloop: argument count: %8.8X\n",
				  walk_state->arg_count));//调试输出：当前指令需要处理的参数数量

		if (walk_state->arg_count) {//如果有需要处理的参数（比如复杂的参数）
			/*
			 * There are arguments (complex ones), push Op and
			 * prepare for argument
			 */
			status = acpi_ps_push_scope(parser_state, op,
						    walk_state->arg_types,
						    walk_state->arg_count);// 将当前操作步骤压入“作用域栈”（类似保存当前任务状态）
			if (ACPI_FAILURE(status)) {
				status =
				    acpi_ps_complete_op(walk_state, &op,
							status);//完成当前Op的解析处理并进行状态转换
				if (ACPI_FAILURE(status)) {
					return_ACPI_STATUS(status);
				}

				continue;
			}

			op = NULL;
			continue;
		}

		/*
		 * All arguments have been processed -- Op is complete,
		 * prepare for next
		 */
		walk_state->op_info =
		    acpi_ps_get_opcode_info(op->common.aml_opcode);//获取当前操作码的元数据（如操作类型、参数数量等
		if (walk_state->op_info->flags & AML_NAMED) {//处理命名型操作码（如Device、Scope等）
			if (op->common.aml_opcode == AML_REGION_OP ||
			    op->common.aml_opcode == AML_DATA_REGION_OP) {//特殊处理OpRegion和DataRegion
				/*
				 * Skip parsing of control method or opregion body,
				 * because we don't have enough info in the first pass
				 * to parse them correctly.
				 *
				 * Completed parsing an op_region declaration, we now
				 * know the length.
				 * 跳过解析区域操作码的主体（如寄存器映射区域），因为第一遍解析时无法正确解析其内容
				 * 此时已知区域声明的长度，记录到op结构中
				 */
				op->named.length =
				    (u32) (parser_state->aml - op->named.data);//计算操作码数据长度
			}
		}

		if (walk_state->op_info->flags & AML_CREATE) {//处理创建型操作码（如CreateField）
			/*
			 * Backup to beginning of create_XXXfield declaration (1 for
			 * Opcode)
			 * 回退到CreateField声明的起始位置，记录长度（操作码主体长度未知，需后续解析）
			 *
			 * body_length is unknown until we parse the body
			 */
			op->named.length =
			    (u32) (parser_state->aml - op->named.data);//计算字段声明的总长度
		}

		if (op->common.aml_opcode == AML_BANK_FIELD_OP) {//处理BankField操作码
			/*
			 * Backup to beginning of bank_field declaration
			 *
			 * body_length is unknown until we parse the body
			 * 回退到BankField声明的起始位置，记录长度（需后续解析银行切换逻辑）
			 */
			op->named.length =
			    (u32) (parser_state->aml - op->named.data);//记录银行字段声明长度
		}

		/* This op complete, notify the dispatcher */

		if (walk_state->ascending_callback != NULL) {//触发解析器状态机回调，通知当前操作码解析完成
			walk_state->op = op;// 设置当前操作对象
			walk_state->opcode = op->common.aml_opcode;//设置操作码类型

			status = walk_state->ascending_callback(walk_state);// 执行回调函数
			status =
			    acpi_ps_next_parse_state(walk_state, op, status);//进入下一解析状态
			if (status == AE_CTRL_PENDING) {//处理控制状态（表示需要延迟处理）
				status = AE_OK;//重置状态为成功继续
			} else
			    if ((walk_state->
				 parse_flags & ACPI_PARSE_MODULE_LEVEL)
				&& (ACPI_AML_EXCEPTION(status)//模块级方法解析时的错误处理
				    || status == AE_ALREADY_EXISTS
				    || status == AE_NOT_FOUND)) {//忽略特定错误（如对象已存在）
				/*
				 * ACPI_PARSE_MODULE_LEVEL flag means that we
				 * are currently loading a table by executing
				 * it as a control method. However, if we
				 * encounter an error while loading the table,
				 * we need to keep trying to load the table
				 * rather than aborting the table load (setting
				 * the status to AE_OK continues the table
				 * load). If we get a failure at this point, it
				 * means that the dispatcher got an error while
				 * trying to execute the Op.
				 *  当处于模块级方法执行阶段（如加载DSDT表）时，
				 *  即使遇到命名冲突或未找到对象等错误，也要继续解析（避免中断表加载）
				 */
				status = AE_OK;//强制设置为成功状态继续执行
			}
		}

		status = acpi_ps_complete_op(walk_state, &op, status);//完成当前操作码的解析流程
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);//处理解析失败，返回错误码
		}

	}			/* while parser_state->Aml */

	status = acpi_ps_complete_final_op(walk_state, op, status);//完成最终操作码的收尾处理
	return_ACPI_STATUS(status);// 返回最终状态码
}
