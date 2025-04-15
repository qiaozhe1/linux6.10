// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: psobject - Support for parse objects
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "amlcode.h"
#include "acconvert.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_PARSER
ACPI_MODULE_NAME("psobject")

/* Local prototypes */
static acpi_status acpi_ps_get_aml_opcode(struct acpi_walk_state *walk_state);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_get_aml_opcode
 *
 * PARAMETERS:  walk_state          - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Extract the next AML opcode from the input stream.
 *
 ******************************************************************************/

static acpi_status acpi_ps_get_aml_opcode(struct acpi_walk_state *walk_state)
{
	ACPI_ERROR_ONLY(u32 aml_offset);

	ACPI_FUNCTION_TRACE_PTR(ps_get_aml_opcode, walk_state);

	walk_state->aml = walk_state->parser_state.aml;
	walk_state->opcode = acpi_ps_peek_opcode(&(walk_state->parser_state));

	/*
	 * First cut to determine what we have found:
	 * 1) A valid AML opcode
	 * 2) A name string
	 * 3) An unknown/invalid opcode
	 */
	walk_state->op_info = acpi_ps_get_opcode_info(walk_state->opcode);

	switch (walk_state->op_info->class) {
	case AML_CLASS_ASCII:
	case AML_CLASS_PREFIX:
		/*
		 * Starts with a valid prefix or ASCII char, this is a name
		 * string. Convert the bare name string to a namepath.
		 */
		walk_state->opcode = AML_INT_NAMEPATH_OP;
		walk_state->arg_types = ARGP_NAMESTRING;
		break;

	case AML_CLASS_UNKNOWN:

		/* The opcode is unrecognized. Complain and skip unknown opcodes */

		if (walk_state->pass_number == 2) {
			ACPI_ERROR_ONLY(aml_offset =
					(u32)ACPI_PTR_DIFF(walk_state->aml,
							   walk_state->
							   parser_state.
							   aml_start));

			ACPI_ERROR((AE_INFO,
				    "Unknown opcode 0x%.2X at table offset 0x%.4X, ignoring",
				    walk_state->opcode,
				    (u32)(aml_offset +
					  sizeof(struct acpi_table_header))));

			ACPI_DUMP_BUFFER((walk_state->parser_state.aml - 16),
					 48);

#ifdef ACPI_ASL_COMPILER
			/*
			 * This is executed for the disassembler only. Output goes
			 * to the disassembled ASL output file.
			 */
			acpi_os_printf
			    ("/*\nError: Unknown opcode 0x%.2X at table offset 0x%.4X, context:\n",
			     walk_state->opcode,
			     (u32)(aml_offset +
				   sizeof(struct acpi_table_header)));

			ACPI_ERROR((AE_INFO,
				    "Aborting disassembly, AML byte code is corrupt"));

			/* Dump the context surrounding the invalid opcode */

			acpi_ut_dump_buffer(((u8 *)walk_state->parser_state.
					     aml - 16), 48, DB_BYTE_DISPLAY,
					    (aml_offset +
					     sizeof(struct acpi_table_header) -
					     16));
			acpi_os_printf(" */\n");

			/*
			 * Just abort the disassembly, cannot continue because the
			 * parser is essentially lost. The disassembler can then
			 * randomly fail because an ill-constructed parse tree
			 * can result.
			 */
			return_ACPI_STATUS(AE_AML_BAD_OPCODE);
#endif
		}

		/* Increment past one-byte or two-byte opcode */

		walk_state->parser_state.aml++;
		if (walk_state->opcode > 0xFF) {	/* Can only happen if first byte is 0x5B */
			walk_state->parser_state.aml++;
		}

		return_ACPI_STATUS(AE_CTRL_PARSE_CONTINUE);

	default:

		/* Found opcode info, this is a normal opcode */

		walk_state->parser_state.aml +=
		    acpi_ps_get_opcode_size(walk_state->opcode);
		walk_state->arg_types = walk_state->op_info->parse_args;
		break;
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_build_named_op
 *
 * PARAMETERS:  walk_state          - Current state
 *              aml_op_start        - Begin of named Op in AML
 *              unnamed_op          - Early Op (not a named Op)
 *              op                  - Returned Op
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Parse a named Op
 *
 ******************************************************************************/

acpi_status
acpi_ps_build_named_op(struct acpi_walk_state *walk_state,
		       u8 *aml_op_start,
		       union acpi_parse_object *unnamed_op,
		       union acpi_parse_object **op)
{
	acpi_status status = AE_OK;
	union acpi_parse_object *arg = NULL;

	ACPI_FUNCTION_TRACE_PTR(ps_build_named_op, walk_state);

	unnamed_op->common.value.arg = NULL;
	unnamed_op->common.arg_list_length = 0;
	unnamed_op->common.aml_opcode = walk_state->opcode;

	/*
	 * Get and append arguments until we find the node that contains
	 * the name (the type ARGP_NAME).
	 */
	while (GET_CURRENT_ARG_TYPE(walk_state->arg_types) &&
	       (GET_CURRENT_ARG_TYPE(walk_state->arg_types) != ARGP_NAME)) {
		ASL_CV_CAPTURE_COMMENTS(walk_state);
		status =
		    acpi_ps_get_next_arg(walk_state,
					 &(walk_state->parser_state),
					 GET_CURRENT_ARG_TYPE(walk_state->
							      arg_types), &arg);
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}

		acpi_ps_append_arg(unnamed_op, arg);
		INCREMENT_ARG_LIST(walk_state->arg_types);
	}

	/* are there any inline comments associated with the name_seg?? If so, save this. */

	ASL_CV_CAPTURE_COMMENTS(walk_state);

#ifdef ACPI_ASL_COMPILER
	if (acpi_gbl_current_inline_comment != NULL) {
		unnamed_op->common.name_comment =
		    acpi_gbl_current_inline_comment;
		acpi_gbl_current_inline_comment = NULL;
	}
#endif

	/*
	 * Make sure that we found a NAME and didn't run out of arguments
	 */
	if (!GET_CURRENT_ARG_TYPE(walk_state->arg_types)) {
		return_ACPI_STATUS(AE_AML_NO_OPERAND);
	}

	/* We know that this arg is a name, move to next arg */

	INCREMENT_ARG_LIST(walk_state->arg_types);

	/*
	 * Find the object. This will either insert the object into
	 * the namespace or simply look it up
	 */
	walk_state->op = NULL;

	status = walk_state->descending_callback(walk_state, op);
	if (ACPI_FAILURE(status)) {
		if (status != AE_CTRL_TERMINATE) {
			ACPI_EXCEPTION((AE_INFO, status,
					"During name lookup/catalog"));
		}
		return_ACPI_STATUS(status);
	}

	if (!*op) {
		return_ACPI_STATUS(AE_CTRL_PARSE_CONTINUE);
	}

	status = acpi_ps_next_parse_state(walk_state, *op, status);
	if (ACPI_FAILURE(status)) {
		if (status == AE_CTRL_PENDING) {
			status = AE_CTRL_PARSE_PENDING;
		}
		return_ACPI_STATUS(status);
	}

	acpi_ps_append_arg(*op, unnamed_op->common.value.arg);

#ifdef ACPI_ASL_COMPILER

	/* save any comments that might be associated with unnamed_op. */

	(*op)->common.inline_comment = unnamed_op->common.inline_comment;
	(*op)->common.end_node_comment = unnamed_op->common.end_node_comment;
	(*op)->common.close_brace_comment =
	    unnamed_op->common.close_brace_comment;
	(*op)->common.name_comment = unnamed_op->common.name_comment;
	(*op)->common.comment_list = unnamed_op->common.comment_list;
	(*op)->common.end_blk_comment = unnamed_op->common.end_blk_comment;
	(*op)->common.cv_filename = unnamed_op->common.cv_filename;
	(*op)->common.cv_parent_filename =
	    unnamed_op->common.cv_parent_filename;
	(*op)->named.aml = unnamed_op->common.aml;

	unnamed_op->common.inline_comment = NULL;
	unnamed_op->common.end_node_comment = NULL;
	unnamed_op->common.close_brace_comment = NULL;
	unnamed_op->common.name_comment = NULL;
	unnamed_op->common.comment_list = NULL;
	unnamed_op->common.end_blk_comment = NULL;
#endif

	if ((*op)->common.aml_opcode == AML_REGION_OP ||
	    (*op)->common.aml_opcode == AML_DATA_REGION_OP) {
		/*
		 * Defer final parsing of an operation_region body, because we don't
		 * have enough info in the first pass to parse it correctly (i.e.,
		 * there may be method calls within the term_arg elements of the body.)
		 *
		 * However, we must continue parsing because the opregion is not a
		 * standalone package -- we don't know where the end is at this point.
		 *
		 * (Length is unknown until parse of the body complete)
		 */
		(*op)->named.data = aml_op_start;
		(*op)->named.length = 0;
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_create_op
 *
 * PARAMETERS:  walk_state          - Current state
 *              aml_op_start        - Op start in AML
 *              new_op              - Returned Op
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Get Op from AML
 *
 ******************************************************************************/
/*
 * acpi_ps_create_op - 创建并初始化一个新的ACPI解析操作节点(Op)
 * @walk_state: ACPI解析状态机上下文
 * @aml_op_start: 指向AML字节码中操作码起始位置的指针
 * @new_op: 输出参数，返回新创建的Op节点指针
 *
 * 功能：
 * 1. 从AML字节码中解析操作码
 * 2. 根据操作码类型创建对应的Op节点
 * 3. 处理命名操作码的特殊情况
 * 4. 将新节点附加到父节点的参数列表
 * 5. 处理创建类操作码和银行字段的特殊情况
 * 6. 执行下降回调函数(如果存在)
 *
 * 返回值：
 * - AE_OK: 操作成功完成
 * - AE_NO_MEMORY: 内存分配失败
 * - AE_CTRL_PARSE_CONTINUE: 需要继续解析
 * - 其他ACPI状态码表示错误
 */
acpi_status
acpi_ps_create_op(struct acpi_walk_state *walk_state,
		  u8 *aml_op_start, union acpi_parse_object **new_op)
{
	acpi_status status = AE_OK;
	union acpi_parse_object *op;//新创建的Op节点指针
	union acpi_parse_object *named_op = NULL;//命名操作节点指针(初始为NULL)
	union acpi_parse_object *parent_scope;//父作用域节点指针
	u8 argument_count;//参数计数器
	const struct acpi_opcode_info *op_info;//操作码信息结构体指针

	ACPI_FUNCTION_TRACE_PTR(ps_create_op, walk_state);

	/* 第一步：从AML字节码中解析操作码 */
	status = acpi_ps_get_aml_opcode(walk_state);
	if (status == AE_CTRL_PARSE_CONTINUE) {
		return_ACPI_STATUS(AE_CTRL_PARSE_CONTINUE);
	}
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//操作码解析失败，直接返回错误状态
	}

	/* Create Op structure and append to parent's argument list */

	/* 第二步：创建基础Op节点结构 */
	walk_state->op_info = acpi_ps_get_opcode_info(walk_state->opcode);//获取当前操作码的详细信息
	op = acpi_ps_alloc_op(walk_state->opcode, aml_op_start);//分配新的Op节点内存空间
	if (!op) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* 第三步：处理命名操作码的特殊情况 */
	if (walk_state->op_info->flags & AML_NAMED) {
		status =
		    acpi_ps_build_named_op(walk_state, aml_op_start, op,
					   &named_op);//构建命名操作节点
		acpi_ps_free_op(op);//释放临时Op节点(因为named_op已创建)

#ifdef ACPI_ASL_COMPILER
		/* 反汇编器特殊处理：外部操作码(EXTERNAL_OP)解析失败时跳过 */
		if (acpi_gbl_disasm_flag
		    && walk_state->opcode == AML_EXTERNAL_OP
		    && status == AE_NOT_FOUND) {
			/*
			 * If parsing of AML_EXTERNAL_OP's name path fails, then skip
			 * past this opcode and keep parsing. This is a much better
			 * alternative than to abort the entire disassembler. At this
			 * point, the parser_state is at the end of the namepath of the
			 * external declaration opcode. Setting walk_state->Aml to
			 * walk_state->parser_state.Aml + 2 moves increments the
			 * walk_state->Aml past the object type and the paramcount of the
			 * external opcode.
			 */
			walk_state->aml = walk_state->parser_state.aml + 2;
			walk_state->parser_state.aml = walk_state->aml;
			return_ACPI_STATUS(AE_CTRL_PARSE_CONTINUE);
		}
#endif
		//检查命名操作构建是否成功
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}

		*new_op = named_op;//返回构建好的命名操作节点
		return_ACPI_STATUS(AE_OK);
	}

	/* Not a named opcode, just allocate Op and append to parent */

	/* 第四步：处理非命名操作码的常规情况 */
	/* 特殊处理创建类操作码(如Field、BankField等) */
	if (walk_state->op_info->flags & AML_CREATE) {
		/*
		 * Backup to beginning of create_XXXfield declaration
		 * body_length is unknown until we parse the body
		 */
		op->named.data = aml_op_start;//指向操作码起始位置
		op->named.length = 0;//初始长度设为0(后续解析填充)
	}

	/* 特殊处理BankField操作码 */
	if (walk_state->opcode == AML_BANK_FIELD_OP) {
		/*
		 * Backup to beginning of bank_field declaration
		 * body_length is unknown until we parse the body
		 */
		//同样保存原始AML位置信息
		op->named.data = aml_op_start;
		op->named.length = 0;
	}

	/* 第五步：将新节点附加到父作用域 */

	parent_scope = acpi_ps_get_parent_scope(&(walk_state->parser_state));//获取当前解析位置的父作用域节点
	acpi_ps_append_arg(parent_scope, op);//将新Op节点添加到父节点的参数列表

	/* 第六步：处理目标操作数标记 */
	if (parent_scope) {
		op_info =
		    acpi_ps_get_opcode_info(parent_scope->common.aml_opcode);//获取父操作码的详细信息
	
		/* 标记目标操作数(需要写入的操作数) */
		if (op_info->flags & AML_HAS_TARGET) {
			argument_count =
			    acpi_ps_get_argument_count(op_info->type);//获取预期的参数数量
			if (parent_scope->common.arg_list_length >
			    argument_count) {//如果实际参数数量超过预期，标记为Target
				op->common.flags |= ACPI_PARSEOP_TARGET;
			}
		}

		/*
		 * Special case for both Increment() and Decrement(), where
		 * the lone argument is both a source and a target.
		 */
		/* 特殊处理Increment/Decrement操作 */
		else if ((parent_scope->common.aml_opcode == AML_INCREMENT_OP)
			 || (parent_scope->common.aml_opcode ==
			     AML_DECREMENT_OP)) {
			op->common.flags |= ACPI_PARSEOP_TARGET;//这些操作的参数既是源也是目标
		}
	}

	/* 第七步：执行下降回调函数(如果存在) */
	if (walk_state->descending_callback != NULL) {
		/*
		 * Find the object. This will either insert the object into
		 * the namespace or simply look it up
		 */
		walk_state->op = *new_op = op;//设置当前操作节点

		status = walk_state->descending_callback(walk_state, &op);//调用回调函数进行进一步处理
		status = acpi_ps_next_parse_state(walk_state, op, status);//更新解析状态
		if (status == AE_CTRL_PENDING) {//处理挂起状态
			status = AE_CTRL_PARSE_PENDING;
		}
	}

	return_ACPI_STATUS(status);// 返回最终状态
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_complete_op
 *
 * PARAMETERS:  walk_state          - Current state
 *              op                  - Returned Op
 *              status              - Parse status before complete Op
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Complete Op
 *
 ******************************************************************************/
/*
 * acpi_ps_complete_op - 完成当前Op(解析树节点)的解析处理并进行状态转换
 * @walk_state: 当前AML解析/执行状态
 * @op:         当前处理的解析节点（输入输出参数）
 * @status:     前序操作返回的状态码
 *
 * 核心功能：
 * 1. 处理不同状态码的控制流（break/continue/terminate等）
 * 2. 维护作用域栈和参数计数
 * 3. 执行完成回调并清理解析树节点
 * 4. 处理模块级代码的错误恢复
 *
 * 状态机设计：
 * - 根据status参数进入不同的处理路径
 * - 支持正常完成(AE_OK)和控制流转移(AE_CTRL_*) 
 * - 严格管理解析树生命周期
 */
acpi_status
acpi_ps_complete_op(struct acpi_walk_state *walk_state,
		    union acpi_parse_object **op, acpi_status status)
{
	acpi_status status2;

	ACPI_FUNCTION_TRACE_PTR(ps_complete_op, walk_state);

	/*
	 * Finished one argument of the containing scope
	 */
	walk_state->parser_state.scope->parse_scope.arg_count--;//减少当前作用域的参数计数器

	/* 关闭此操作（将导致解析子树删除）*/
	status2 = acpi_ps_complete_this_op(walk_state, *op);//完成当前Op的处理（可能释放解析树节点） 
	if (ACPI_FAILURE(status2)) {
		return_ACPI_STATUS(status2);
	}

	*op = NULL;

	switch (status) {//根据主状态码进行不同处理
	case AE_OK://正常状态

		break;

	case AE_CTRL_TRANSFER://控制转移状态（如方法调用）

		/* 准备转移到被调用的方法 */

		walk_state->prev_op = NULL;//清除前一个Op
		walk_state->prev_arg_types = walk_state->arg_types;//保存参数类型
		return_ACPI_STATUS(status);//直接返回转移状态

	case AE_CTRL_END://控制结束状态

		acpi_ps_pop_scope(&(walk_state->parser_state), op,
				  &walk_state->arg_types,
				  &walk_state->arg_count);//弹出当前作用域

		if (*op) {//如果存在新的Op
			walk_state->op = *op;//更新walk_state中的当前Op
			walk_state->op_info =
			    acpi_ps_get_opcode_info((*op)->common.aml_opcode);//获取Op信息
			walk_state->opcode = (*op)->common.aml_opcode;//设置操作码

			status = walk_state->ascending_callback(walk_state);//执行回调函数
			(void)acpi_ps_next_parse_state(walk_state, *op, status);//更新解析状态

			status2 = acpi_ps_complete_this_op(walk_state, *op);//完成新Op的处理
			if (ACPI_FAILURE(status2)) {
				return_ACPI_STATUS(status2);
			}
		}

		break;

	case AE_CTRL_BREAK://break控制流
	case AE_CTRL_CONTINUE://continue控制流

		/* Pop off scopes until we find the While */

		while (!(*op) || ((*op)->common.aml_opcode != AML_WHILE_OP)) {//弹出作用域直到找到While Op
			acpi_ps_pop_scope(&(walk_state->parser_state), op,
					  &walk_state->arg_types,
					  &walk_state->arg_count);
		}

		/* Close this iteration of the While loop */
		/* 完成当前While循环迭代 */
		walk_state->op = *op;
		walk_state->op_info =
		    acpi_ps_get_opcode_info((*op)->common.aml_opcode);//获取Op信息
		walk_state->opcode = (*op)->common.aml_opcode;//设置操作码

		status = walk_state->ascending_callback(walk_state);//执行回调函数
		(void)acpi_ps_next_parse_state(walk_state, *op, status);//设置状态码

		status2 = acpi_ps_complete_this_op(walk_state, *op);//完成当前Op的处理
		if (ACPI_FAILURE(status2)) {
			return_ACPI_STATUS(status2);
		}

		break;

	case AE_CTRL_TERMINATE://终止控制流

		/* 清理控制状态栈 */
		do {
			if (*op) {
				status2 =
				    acpi_ps_complete_this_op(walk_state, *op);//完成当前Op的处理
				if (ACPI_FAILURE(status2)) {
					return_ACPI_STATUS(status2);
				}

				acpi_ut_delete_generic_state
				    (acpi_ut_pop_generic_state
				     (&walk_state->control_state));//弹出并删除控制状态
			}

			acpi_ps_pop_scope(&(walk_state->parser_state), op,
					  &walk_state->arg_types,
					  &walk_state->arg_count);//弹出当前作用域

		} while (*op);

		return_ACPI_STATUS(AE_OK);//返回成功

	default:		/* 所有其他非AE_OK状态 */

		do {
			if (*op) {//检查当前Op是否有效
                                /*
                                 * 特殊处理REGION_OP和DATA_REGION_OP：
                                 * 这些Opcode即使在错误情况下也可能已经创建了命名空间节点，
                                 * 因此需要显式清理
                                 */
				if (((*op)->common.aml_opcode == AML_REGION_OP)
				    || ((*op)->common.aml_opcode ==
					AML_DATA_REGION_OP)) {
					acpi_ns_delete_children((*op)->common.
								node);//递归删除该节点下的所有子节点
					acpi_ns_remove_node((*op)->common.node);//从命名空间移除该节点本身
					(*op)->common.node = NULL;//清空节点指针避免悬空引用
					acpi_ps_delete_parse_tree(*op);//删除整个解析树
				}

				status2 =
				    acpi_ps_complete_this_op(walk_state, *op);//完成当前Op的处理
				if (ACPI_FAILURE(status2)) {
					return_ACPI_STATUS(status2);
				}
			}

			acpi_ps_pop_scope(&(walk_state->parser_state), op,
					  &walk_state->arg_types,
					  &walk_state->arg_count);//弹出作用域

		} while (*op);

#if 0
		/*
		 * TBD: Cleanup parse ops on error
		 */
		if (*op == NULL) {
			acpi_ps_pop_scope(parser_state, op,
					  &walk_state->arg_types,
					  &walk_state->arg_count);
		}
#endif
		walk_state->prev_op = NULL;//清除前一个Op
		walk_state->prev_arg_types = walk_state->arg_types;//保存参数类型 

		if (walk_state->parse_flags & ACPI_PARSE_MODULE_LEVEL) {//模块级错误特殊处理
			/*
			 * 在执行模块级别的代码时出现了某种错误。我们需要跳过导致错误的部分，
			 * 并继续进行解析。一次运行时错误不应该导致整个表格加载失败，
			 * 因为在导致错误的部分之后，可能还存在正确的 AML 代码。
			 */
			ACPI_INFO(("Ignoring error and continuing table load"));
			return_ACPI_STATUS(AE_OK);
		}
		return_ACPI_STATUS(status);//返回原始状态
	}

	/* This scope complete? */

	if (acpi_ps_has_completed_scope(&(walk_state->parser_state))) {//检查当前作用域是否完成
		acpi_ps_pop_scope(&(walk_state->parser_state), op,
				  &walk_state->arg_types,
				  &walk_state->arg_count);//作用域完成则弹出
		ACPI_DEBUG_PRINT((ACPI_DB_PARSE, "Popped scope, Op=%p\n", *op));
	} else {
		*op = NULL;//作用域未完成则清空Op
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_complete_final_op
 *
 * PARAMETERS:  walk_state          - Current state
 *              op                  - Current Op
 *              status              - Current parse status before complete last
 *                                    Op
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Complete last Op.
 *
 ******************************************************************************/

acpi_status
acpi_ps_complete_final_op(struct acpi_walk_state *walk_state,
			  union acpi_parse_object *op, acpi_status status)
{
	acpi_status status2;

	ACPI_FUNCTION_TRACE_PTR(ps_complete_final_op, walk_state);

	/*
	 * Complete the last Op (if not completed), and clear the scope stack.
	 * It is easily possible to end an AML "package" with an unbounded number
	 * of open scopes (such as when several ASL blocks are closed with
	 * sequential closing braces). We want to terminate each one cleanly.
	 */
	ACPI_DEBUG_PRINT((ACPI_DB_PARSE, "AML package complete at Op %p\n",
			  op));
	do {
		if (op) {
			if (walk_state->ascending_callback != NULL) {
				walk_state->op = op;
				walk_state->op_info =
				    acpi_ps_get_opcode_info(op->common.
							    aml_opcode);
				walk_state->opcode = op->common.aml_opcode;

				status =
				    walk_state->ascending_callback(walk_state);
				status =
				    acpi_ps_next_parse_state(walk_state, op,
							     status);
				if (status == AE_CTRL_PENDING) {
					status =
					    acpi_ps_complete_op(walk_state, &op,
								AE_OK);
					if (ACPI_FAILURE(status)) {
						return_ACPI_STATUS(status);
					}
				}

				if (status == AE_CTRL_TERMINATE) {
					status = AE_OK;

					/* Clean up */
					do {
						if (op) {
							status2 =
							    acpi_ps_complete_this_op
							    (walk_state, op);
							if (ACPI_FAILURE
							    (status2)) {
								return_ACPI_STATUS
								    (status2);
							}
						}

						acpi_ps_pop_scope(&
								  (walk_state->
								   parser_state),
								  &op,
								  &walk_state->
								  arg_types,
								  &walk_state->
								  arg_count);

					} while (op);

					return_ACPI_STATUS(status);
				}

				else if (ACPI_FAILURE(status)) {

					/* First error is most important */

					(void)
					    acpi_ps_complete_this_op(walk_state,
								     op);
					return_ACPI_STATUS(status);
				}
			}

			status2 = acpi_ps_complete_this_op(walk_state, op);
			if (ACPI_FAILURE(status2)) {
				return_ACPI_STATUS(status2);
			}
		}

		acpi_ps_pop_scope(&(walk_state->parser_state), &op,
				  &walk_state->arg_types,
				  &walk_state->arg_count);

	} while (op);

	return_ACPI_STATUS(status);
}
