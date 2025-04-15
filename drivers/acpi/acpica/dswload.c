// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: dswload - Dispatcher first pass namespace load callbacks
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "amlcode.h"
#include "acdispat.h"
#include "acinterp.h"
#include "acnamesp.h"
#ifdef ACPI_ASL_COMPILER
#include "acdisasm.h"
#endif

#define _COMPONENT          ACPI_DISPATCHER
ACPI_MODULE_NAME("dswload")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_init_callbacks
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *              pass_number     - 1, 2, or 3
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Init walk state callbacks
 *
 ******************************************************************************/
/*
 * acpi_ds_init_callbacks - 初始化AML解析/执行的回调函数
 * @walk_state: 遍历状态结构指针
 * @pass_number: 处理阶段标识（0-3）
 *
 * 功能说明：
 * 1. 根据不同的处理阶段配置解析标志位(parse_flags)
 * 2. 设置向下遍历(descending)和向上遍历(ascending)的回调函数
 * 3. 支持四种处理模式：纯解析/加载阶段1/加载阶段2/执行阶段
 *
 * 阶段详解：
 * - pass0: 仅解析AML（不创建命名空间对象）
 * - pass1: 加载阶段1（创建命名空间节点）
 * - pass2: 加载阶段2（初始化对象和字段）
 * - pass3: 执行阶段（运行控制方法）
 */
acpi_status
acpi_ds_init_callbacks(struct acpi_walk_state *walk_state, u32 pass_number)
{

	switch (pass_number) {
	case 0://纯解析模式（用于反汇编等场景）

		/* Parse only - caller will setup callbacks */

		walk_state->parse_flags = ACPI_PARSE_LOAD_PASS1 |//保持基础解析标志
		    ACPI_PARSE_DELETE_TREE | ACPI_PARSE_DISASSEMBLE;//完成后删除解析树|反汇编模式
		walk_state->descending_callback = NULL;//不设置向下回调
		walk_state->ascending_callback = NULL;//不设置向上回调
		break;

	case 1://加载阶段1（创建命名空间节点）

		/* Load pass 1 */

		walk_state->parse_flags = ACPI_PARSE_LOAD_PASS1 |//加载阶段1标志
		    ACPI_PARSE_DELETE_TREE;//完成后删除解析树
		walk_state->descending_callback = acpi_ds_load1_begin_op;//解析树节点创建回调
		walk_state->ascending_callback = acpi_ds_load1_end_op;//解析树节点完成回调
		break;

	case 2://加载阶段2（初始化对象）

		/* Load pass 2 */

		walk_state->parse_flags = ACPI_PARSE_LOAD_PASS1 |//保持基础标志
		    ACPI_PARSE_DELETE_TREE;//完成后删除解析树
		walk_state->descending_callback = acpi_ds_load2_begin_op;//对象初始化回调
		walk_state->ascending_callback = acpi_ds_load2_end_op;//对象完成回调
		break;

	case 3:// 执行阶段（运行控制方法）

		/* Execution pass */

		walk_state->parse_flags |= ACPI_PARSE_EXECUTE |//添加执行标志
		    ACPI_PARSE_DELETE_TREE;//完成后删除解析树
		walk_state->descending_callback = acpi_ds_exec_begin_op;//方法执行前回调
		walk_state->ascending_callback = acpi_ds_exec_end_op;//方法执行后回调
		break;

	default://无效阶段号

		return (AE_BAD_PARAMETER);//返回参数错误(0x000A)
	}

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_load1_begin_op
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *              out_op          - Where to return op if a new one is created
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Descending callback used during the loading of ACPI tables.
 *
 ******************************************************************************/
/*
 * acpi_ds_load1_begin_op - ACPI命名空间加载阶段1的起始处理函数
 * @walk_state: 当前遍历状态结构体指针
 * @out_op:     输出参数，返回处理后的解析节点
 *
 * 功能：
 * 1. 处理AML命名对象的首次加载
 * 2. 创建/验证命名空间节点
 * 3. 处理特殊操作码（如Scope）
 * 4. 维护解析树和命名空间的关联
 */
acpi_status
acpi_ds_load1_begin_op(struct acpi_walk_state *walk_state,
		       union acpi_parse_object **out_op)
{
	union acpi_parse_object *op;//当前解析节点
	struct acpi_namespace_node *node;//命名空间节点
	acpi_status status;
	acpi_object_type object_type;//ACPI对象类型
	char *path;//名称路径字符串
	u32 flags;

	ACPI_FUNCTION_TRACE_PTR(ds_load1_begin_op, walk_state->op);

	op = walk_state->op;
	ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH, "Op=%p State=%p\n", op,
			  walk_state));

	/* We are only interested in opcodes that have an associated name */
	/* 我们只对具有关联名称的操作码感兴趣 */
	if (op) {
		if (!(walk_state->op_info->flags & AML_NAMED)) {//跳过非命名对象（无需命名空间操作） 
			*out_op = op;
			return_ACPI_STATUS(AE_OK);//直接返回成功
		}

		/* 检查此对象是否已安装在命名空间中 */
		if (op->common.node) {
			*out_op = op;
			return_ACPI_STATUS(AE_OK);//直接返回成功
		}
	}

	path = acpi_ps_get_next_namestring(&walk_state->parser_state);//获取下一个名称字符串（从AML流中提取）

	/* Map the raw opcode into an internal object type */

	object_type = walk_state->op_info->object_type;//确定对象类型（从操作码信息获取）

	ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
			  "State=%p Op=%p [%s]\n", walk_state, op,
			  acpi_ut_get_type_name(object_type)));

	switch (walk_state->opcode) {//根据当前操作码进行分派处理
	case AML_SCOPE_OP://处理Scope操作符（0x10）
		/*
		 * Scope操作符的目标节点必须已存在，以便打开作用域并添加新对象
		 * 对于单段名称（namesegs），允许搜索到根节点
		 */
		status =
		    acpi_ns_lookup(walk_state->scope_info, path, object_type,
				   ACPI_IMODE_EXECUTE, ACPI_NS_SEARCH_PARENT,
				   walk_state, &(node));//查找目标命名空间节点
#ifdef ACPI_ASL_COMPILER// ASL编译器特有逻辑
		if (status == AE_NOT_FOUND) {//当目标节点不存在时
			/*
                         * 反汇编场景特殊处理：
                         * 1. 将Scope目标作为External声明添加到外部列表
                         * 2. 以加载模式重新执行命名空间查找（此时会创建节点）
                         */
			acpi_dm_add_op_to_external_list(op, path,
							ACPI_TYPE_DEVICE, 0, 0);
			status =
			    acpi_ns_lookup(walk_state->scope_info, path,
					   object_type, ACPI_IMODE_LOAD_PASS1,//ACPI_IMODE_LOAD_PASS1:加载模式,允许创建节点
					   ACPI_NS_SEARCH_PARENT, walk_state,
					   &node);
		}
#endif
		if (ACPI_FAILURE(status)) {//命名空间查找失败
			ACPI_ERROR_NAMESPACE(walk_state->scope_info, path,
					     status);
			return_ACPI_STATUS(status);//返回错误
		}

                /*
                 * 检查目标节点是否为真正打开作用域的操作码之一，
                 * 确保该目标是具有作用域语义的操作符。
                 */
		/* 验证目标节点是否为合法作用域类型 */
		switch (node->type) {//根据节点类型处理
		case ACPI_TYPE_ANY://任意类型（特殊场景）
		case ACPI_TYPE_LOCAL_SCOPE;//本地作用域（如预定义作用域）
		case ACPI_TYPE_DEVICE://设备对象
		case ACPI_TYPE_POWER://电源资源对象
		case ACPI_TYPE_PROCESSOR://处理器对象
		case ACPI_TYPE_THERMAL://热区域对象

			/* These are acceptable types */
			break;//允许的作用域类型，直接通过

		case ACPI_TYPE_INTEGER://整型（非法但兼容处理）
		case ACPI_TYPE_STRING://字符串（非法但兼容处理）
		case ACPI_TYPE_BUFFER:// 缓冲区（非法但兼容处理）
			/*
                         * 类型修正逻辑：
                         * 允许将原为Integer/String/Buffer的节点类型强制改为ACPI_TYPE_ANY
                         * 兼容类似"Name (DEB, 0) Scope (DEB)"的历史代码写法
                         * 注：第二遍加载时会生成警告
                         */
			ACPI_DEBUG_PRINT((ACPI_DB_INFO,
					  "Type override - [%4.4s] had invalid type (%s) "
					  "for Scope operator, changed to type ANY\n",
					  acpi_ut_get_node_name(node),
					  acpi_ut_get_type_name(node->type)));

			node->type = ACPI_TYPE_ANY;//修改节点类型为ANY
			walk_state->scope_info->common.value = ACPI_TYPE_ANY;//同步更新作用域信息
			break;

		case ACPI_TYPE_METHOD://方法对象（特殊处理）
			/*
                         * 模块级代码执行期间的特殊豁免：
                         * 允许根节点（此时类型为METHOD）作为Scope目标
                         * 因为根节点在模块级代码解析期间被临时标记为METHOD类型
                         */
			if ((node == acpi_gbl_root_node) &&//是根节点
			    (walk_state->
			     parse_flags & ACPI_PARSE_MODULE_LEVEL)) {//并且是模块级代码
				break;//允许通过
			}

			ACPI_FALLTHROUGH;//宏：指示编译器此处需要穿透到下个case

		default://所有其他未处理类型

			/* All other types are an error */
			/* 生成错误信息并返回类型错误 */
			ACPI_ERROR((AE_INFO,
				    "Invalid type (%s) for target of "
				    "Scope operator [%4.4s] (Cannot override)",
				    acpi_ut_get_type_name(node->type),
				    acpi_ut_get_node_name(node)));

			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);
		}
		break;

	default:
                /*
                ¦* 对于所有其他命名操作码，我们将把名称输入到命名空间中
                ¦* 
                ¦* 设置搜索标志：
                ¦* 由于我们要将名称输入命名空间，因此不希望启用到根节点的向上搜索
                ¦*
                ¦* 只有两种情况允许名称已存在：
                ¦*    1) Scope()操作符可以重新打开之前定义的作用域对象(Scope, Method, Device等)
                ¦*    2) 当我们解析延迟操作码(op_region, Buffer, buffer_field或Package)时，
                ¦*       对象名称已存在于命名空间中
                ¦*/
		if (walk_state->deferred_node) {//检查是否是延迟节点

			/* This name is already in the namespace, get the node */
			/* 该名称已存在于命名空间中，直接获取节点 */
			node = walk_state->deferred_node;//使用预定义的延迟节点
			status = AE_OK;//设置状态为成功
			break;
		}

		/*
		 * 如果正在执行方法，在加载阶段不创建任何命名空间对象，只在执行阶段创建
		 */
		if (walk_state->method_node) {//检查是否在方法执行中
			node = NULL;//不创建节点
			status = AE_OK;//设置状态为成功
			break;
		}

		flags = ACPI_NS_NO_UPSEARCH;//设置不向上搜索的标志
		if ((walk_state->opcode != AML_SCOPE_OP) &&//不是Scope操作
		    (!(walk_state->parse_flags & ACPI_PARSE_DEFERRED_OP))) {//不是延迟操作
			if (walk_state->namespace_override) {//检查是否允许覆盖
				flags |= ACPI_NS_OVERRIDE_IF_FOUND;//设置覆盖标志
				ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
						  "[%s] Override allowed\n",
						  acpi_ut_get_type_name
						  (object_type)));//调试输出
			} else {
				flags |= ACPI_NS_ERROR_IF_FOUND;//设置错误标志(如果已存在)
				ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
						  "[%s] Cannot already exist\n",
						  acpi_ut_get_type_name
						  (object_type)));//调试输出
			}
		} else {
			ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,//调试输出
					  "[%s] Both Find or Create allowed\n",
					  acpi_ut_get_type_name(object_type)));
		}

		/*
		 * 将命名类型输入到内部命名空间中。我们在解析树向下时输入名称。
		 * 任何涉及操作码参数的必要子对象必须在稍后解析树向上时创建。
		 */
		status =
		    acpi_ns_lookup(walk_state->scope_info, path, object_type,
				   ACPI_IMODE_LOAD_PASS1, flags, walk_state,
				   &node);//在命名空间中查找或创建节点
		if (ACPI_FAILURE(status)) {//检查操作是否失败
			if (status == AE_ALREADY_EXISTS) {//检查是否是已存在错误

				/* The name already exists in this scope */
				/* 名称已存在于当前作用域中 */
				if (node->flags & ANOBJ_IS_EXTERNAL) {//检查是否是外部声明
					/*
					 * 允许在之前声明为External的对象或段上创建一次
					 */
					node->flags &= ~ANOBJ_IS_EXTERNAL;//清除外部标志
					node->type = (u8) object_type;//设置节点类型

					/* Just retyped a node, probably will need to open a scope */
					/* 重新类型化节点后，可能需要打开一个作用域 */
					if (acpi_ns_opens_scope(object_type)) {//检查是否需要打开作用域
						status =
						    acpi_ds_scope_stack_push
						    (node, object_type,
						     walk_state);//将节点压入作用域栈
						if (ACPI_FAILURE(status)) {
							return_ACPI_STATUS
							    (status);
						}
					}

					status = AE_OK;//设置状态为成功
				}
			}

			if (ACPI_FAILURE(status)) {//检查最终状态是否仍为失败
				ACPI_ERROR_NAMESPACE(walk_state->scope_info,
						     path, status);
				return_ACPI_STATUS(status);//返回错误状态
			}
		}
		break;
	}

	/* Common exit */

	if (!op) {//检查操作节点是否为空

		/* Create a new op */

		op = acpi_ps_alloc_op(walk_state->opcode, walk_state->aml);//分配操作节点内存
		if (!op) {
			return_ACPI_STATUS(AE_NO_MEMORY);
		}
	}

	/* Initialize the op */

#ifdef ACPI_CONSTANT_EVAL_ONLY
	op->named.path = path;//设置操作节点的路径
#endif

	if (node) {//检查命名空间节点是否存在
		/*
		 * 将节点放入解析器使用的"op"对象中，这样当这个作用域关闭时可以快速再次获取它
		 */
		op->common.node = node;
		op->named.name = node->name.integer;
	}

	acpi_ps_append_arg(acpi_ps_get_parent_scope(&walk_state->parser_state),
			   op);//将操作节点附加到父操作节点的参数列表
	*out_op = op;//通过输出参数返回操作节点
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ds_load1_end_op
 *
 * PARAMETERS:  walk_state      - Current state of the parse tree walk
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Ascending callback used during the loading of the namespace,
 *              both control methods and everything else.
 *
 ******************************************************************************/
/*
 * acpi_ds_load1_end_op - ACPI加载阶段1的结束处理函数
 * @walk_state: 当前遍历状态结构指针
 *
 * 功能说明：
 * 1. 处理加载阶段1的各类对象的收尾工作
 * 2. 创建区域(Region)和方法(Method)对象
 * 3. 管理作用域栈的弹出操作
 * 4. 支持反汇编器的特殊处理
 */
acpi_status acpi_ds_load1_end_op(struct acpi_walk_state *walk_state)
{
	union acpi_parse_object *op;//当前操作的解析树节点指针
	acpi_object_type object_type;//存储对象类型（如ACPI_TYPE_METHOD）
	acpi_status status = AE_OK;//初始化状态为成功
#ifdef ACPI_ASL_COMPILER
	u8 param_count;//参数计数器(仅编译器使用)
#endif

	ACPI_FUNCTION_TRACE(ds_load1_end_op);

	op = walk_state->op;//获取当前解析树节点指针
	ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH, "Op=%p State=%p\n", op,
			  walk_state));
       
        /*
         * 反汇编器特殊处理：在此处处理创建字段操作
         *
         * create_buffer_field通常是加载阶段2处理的延迟操作，但在反汇编控制方法内容时，
         * 解析树使用ACPI_PARSE_LOAD_PASS1标志遍历，AML_CREATE操作会在后续遍历中处理。
         * 当控制方法与AML_CREATE对象同名时会产生问题，因为任何名称段的使用都会被检测为
         * 方法调用而不是缓冲区字段引用。
         *
         * 在反汇编时提前创建可以解决这个问题，通过在命名空间中插入命名对象，
         * 使得对该名称的引用会被视为名称字符串而非方法调用。
         */
	if ((walk_state->parse_flags & ACPI_PARSE_DISASSEMBLE) &&
	    (walk_state->op_info->flags & AML_CREATE)) {//反汇编模式且操作为创建操作
		status = acpi_ds_create_buffer_field(op, walk_state);//创建缓冲区字段
		return_ACPI_STATUS(status);//返回状态
	}

	/* 仅处理有关联名称的操作码（如Name、External、Method） */
	if (!(walk_state->op_info->flags & (AML_NAMED | AML_FIELD))) {//如果是非命名或非字段操作
		return_ACPI_STATUS(AE_OK);// 直接返回成功
	}

	/* 获取对象类型以确定是否需要弹出作用域 */
	object_type = walk_state->op_info->object_type;//从操作信息获取对象类型

	/* 处理字段操作 */
	if (walk_state->op_info->flags & AML_FIELD) {//如果是字段操作标志
		/*
                 * 如果正在执行方法，在加载阶段不创建任何命名空间对象，
                 * 只在执行阶段创建。
                 */
		if (!walk_state->method_node) {//如果不在方法执行中 → 允许创建命名空间对象
			/* 判断字段类型 */
			if (walk_state->opcode == AML_FIELD_OP ||//普通字段操作
			    walk_state->opcode == AML_BANK_FIELD_OP ||//bank字段操作
			    walk_state->opcode == AML_INDEX_FIELD_OP) {//索引字段操作
				status =
				    acpi_ds_init_field_objects(op, walk_state);//初始化字段对象:创建命名空间中的字段节点（如Field单元）
			}
		}
		return_ACPI_STATUS(status);//返回初始化结果
	}

        /*
         * 如果正在执行方法，在加载阶段不创建任何命名空间对象，
         * 只在执行阶段创建。
         */
	/* 处理区域（Region）和数据区域（DataRegion） */
	if (!walk_state->method_node) {//如果不在方法执行中 → 允许创建命名空间对象
		if (op->common.aml_opcode == AML_REGION_OP) {//如果是区域操作
			status =
			    acpi_ex_create_region(op->named.data,//区域名称
						  op->named.length,//名称长度
						  (acpi_adr_space_type)
						  ((op->common.value.arg)->
						   common.value.integer),//从参数获取类型
						  walk_state);//创建区域对象
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}
		} else if (op->common.aml_opcode == AML_DATA_REGION_OP) {//如果是数据区域操作
			status =
			    acpi_ex_create_region(op->named.data,
						  op->named.length,
						  ACPI_ADR_SPACE_DATA_TABLE,
						  walk_state);//创建数据区域对象
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}
		}
	}

	if (op->common.aml_opcode == AML_NAME_OP) {//如果是Name操作符

		/* 对于NAME操作，从参数获取对象类型 */
		if (op->common.value.arg) {//如果存在参数
			object_type = (acpi_ps_get_opcode_info((op->common.//acpi_ps_get_opcode_info函数获取参数操作码信息
								value.arg)->
							       common.
							       aml_opcode))->
			    object_type;//根据参数操作符获取类型

			/* 如果有命名空间节点，设置节点类型 */
			if (op->common.node) {
				op->common.node->type = (u8) object_type;//设置节点类型
			}
		}
	}
#ifdef ACPI_ASL_COMPILER//ASL编译器特有代码
	/*
         * 对于外部操作码，从其第一个参数中获取对象类型，
         * 并从该参数的下一个参数中获取参数数量。
         */
	if (acpi_gbl_disasm_flag &&
	    op->common.node && op->common.aml_opcode == AML_EXTERNAL_OP) {//如果是反汇编模式且节点有效且是外部操作码
		/*
		 * External对象的参数计数：
		 * - 对于非方法外部对象 → 参数计数为0
		 * - 方法外部对象 → 参数计数从后续操作符中读取
		 */

		param_count =
		    (u8)op->common.value.arg->common.next->common.value.integer;//读取参数计数
		object_type = (u8)op->common.value.arg->common.value.integer;//读取对象类型
		op->common.node->flags |= ANOBJ_IS_EXTERNAL;//标记为外部对象
		op->common.node->type = (u8)object_type;//设置对象类型

		acpi_dm_create_subobject_for_external((u8)object_type,
						      &op->common.node,
						      param_count);//为外部声明创建子对象

		/*
		 * Add the external to the external list because we may be
		 * emitting code based off of the items within the external list.
		 */
		acpi_dm_add_op_to_external_list(op, op->named.path,
						(u8)object_type, param_count,
						ACPI_EXT_ORIGIN_FROM_OPCODE |
						ACPI_EXT_RESOLVED_REFERENCE);//将外部声明添加到外部列表
	}
#endif

        /*
         * 如果正在执行方法，在加载阶段不创建任何命名空间对象，
         * 只在执行阶段创建。
         */
	if (!walk_state->method_node) {//如果不在方法执行中 → 允许创建命名空间对象
		if (op->common.aml_opcode == AML_METHOD_OP) {//如果是方法操作
                        
			/*
                         * method_op pkg_length name_string method_flags term_list
                         *
                         * 注意：我们必须在看到方法声明时就创建方法节点/对象对。
                         * 这允许在后续pass1解析方法调用时知道参数数量。
                         */
			ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
					  "LOADING-Method: State=%p Op=%p NamedObj=%p\n",
					  walk_state, op, op->named.node));

			if (!acpi_ns_get_attached_object(op->named.node)) {//检查节点空间是否已关联对象
				walk_state->operands[0] =
				    ACPI_CAST_PTR(void, op->named.node);//设置第一个参数为命名空间节点
				walk_state->num_operands = 1;//设置参数计数

				status =
				    acpi_ds_create_operands(walk_state,
							    op->common.value.
							    arg);//创建方法参数对象
				if (ACPI_SUCCESS(status)) {//如果成功创建
					status =
					    acpi_ex_create_method(op->named.
								  data,
								  op->named.
								  length,
								  walk_state);//创建方法对象
				}

				walk_state->operands[0] = NULL;//清除参数
				walk_state->num_operands = 0;//重置操作数计数

				if (ACPI_FAILURE(status)) {
					return_ACPI_STATUS(status);
				}
			}
		}
	}

	/* 弹出作用域栈（仅在加载表时） */
	if (!walk_state->method_node &&//如果不在方法执行中
	    op->common.aml_opcode != AML_EXTERNAL_OP &&//且不是外部操作码
	    acpi_ns_opens_scope(object_type)) {//且该类型需要作用域
		ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
				  "(%s): Popping scope for Op %p\n",
				  acpi_ut_get_type_name(object_type), op));//调试日志

		status = acpi_ds_scope_stack_pop(walk_state);//弹出作用域栈
	}

	return_ACPI_STATUS(status);//返回最终状态码
}
