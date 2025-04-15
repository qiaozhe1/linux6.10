// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: psutils - Parser miscellaneous utilities (Parser only)
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "amlcode.h"
#include "acconvert.h"

#define _COMPONENT          ACPI_PARSER
ACPI_MODULE_NAME("psutils")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_create_scope_op
 *
 * PARAMETERS:  None
 *
 * RETURN:      A new Scope object, null on failure
 *
 * DESCRIPTION: Create a Scope and associated namepath op with the root name
 *
 ******************************************************************************/
/*
 * acpi_ps_create_scope_op - 创建并初始化SCOPE操作符的解析节点
 * @aml: 指向AML字节码中ScopeOp操作码的指针（用于调试定位）
 *
 * 功能说明：
 * 1. 分配SCOPE操作符的解析节点内存
 * 2. 初始化节点基础属性（操作码类型、AML位置）
 * 3. 设置默认名称为ACPI根路径标识符（"\"）
 * 4. 作为解析树的根节点使用
 *
 * 典型调用场景：
 * - ACPI表加载时的初始作用域创建
 * - 控制方法执行的上下文初始化
 */
union acpi_parse_object *acpi_ps_create_scope_op(u8 *aml)
{
	union acpi_parse_object *scope_op;

	scope_op = acpi_ps_alloc_op(AML_SCOPE_OP, aml);//分配SCOPE操作符节点（AML_SCOPE_OP=0x10）
	if (!scope_op) {
		return (NULL);
	}

	scope_op->named.name = ACPI_ROOT_NAME;//设置节点名称为根路径标识符
	return (scope_op);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_init_op
 *
 * PARAMETERS:  op              - A newly allocated Op object
 *              opcode          - Opcode to store in the Op
 *
 * RETURN:      None
 *
 * DESCRIPTION: Initialize a parse (Op) object
 *
 ******************************************************************************/

void acpi_ps_init_op(union acpi_parse_object *op, u16 opcode)
{
	ACPI_FUNCTION_ENTRY();

	op->common.descriptor_type = ACPI_DESC_TYPE_PARSER;
	op->common.aml_opcode = opcode;

	ACPI_DISASM_ONLY_MEMBERS(acpi_ut_safe_strncpy(op->common.aml_op_name,
						      (acpi_ps_get_opcode_info
						       (opcode))->name,
						      sizeof(op->common.
							     aml_op_name)));
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_alloc_op
 *
 * PARAMETERS:  opcode          - Opcode that will be stored in the new Op
 *              aml             - Address of the opcode
 *
 * RETURN:      Pointer to the new Op, null on failure
 *
 * DESCRIPTION: Allocate an acpi_op, choose op type (and thus size) based on
 *              opcode. A cache of opcodes is available for the pure
 *              GENERIC_OP, since this is by far the most commonly used.
 *
 ******************************************************************************/
/*
 * acpi_ps_alloc_op - 分配并初始化ACPI解析树节点(ParseOp)
 * @opcode: AML操作码（如AML_SCOPE_OP=0x10）
 * @aml:    对应AML字节码的指针（用于调试定位）
 *
 * 核心功能：
 * 1. 根据操作码类型选择内存分配策略（普通/扩展节点）
 * 2. 初始化节点基础属性（操作码类型、AML位置、标志位）
 * 3. 处理特殊操作码的附加需求（如ScopeOp的全局引用）
 * 4. 支持注释捕获模式（ASL编译器调试用）
 *
 * 内存管理：
 * - 使用两个slab缓存提高内存利用率：
 *   acpi_gbl_ps_node_cache      : 8字节基础节点（占70%场景）
 *   acpi_gbl_ps_node_ext_cache  : 16字节扩展节点（带命名/延迟字段）
 */
union acpi_parse_object *acpi_ps_alloc_op(u16 opcode, u8 *aml)
{
	union acpi_parse_object *op;//解析节点指针
	const struct acpi_opcode_info *op_info;//操作码描述信息
	u8 flags = ACPI_PARSEOP_GENERIC;//默认节点类型标记

	ACPI_FUNCTION_ENTRY();

	op_info = acpi_ps_get_opcode_info(opcode);//获取操作码元信息（从全局acpi_gbl_aml_op_info数组获取）

	/* 根据操作码特性确定节点类型 */
	if (op_info->flags & AML_DEFER) {// 需要延迟解析的操作码（如MethodCall）
		flags = ACPI_PARSEOP_DEFERRED;//标记为延迟处理类型
	} else if (op_info->flags & AML_NAMED) {//命名对象（Device/Method等）
		flags = ACPI_PARSEOP_NAMED_OBJECT;//需要扩展存储名称字段
	} else if (opcode == AML_INT_BYTELIST_OP) {//字节列表操作（如原始数据块）
		flags = ACPI_PARSEOP_BYTELIST;//特殊处理类型
	}

	/* 根据类型选择内存分配策略 */
	if (flags == ACPI_PARSEOP_GENERIC) {//普通操作码（占大多数场景）

		/* The generic op (default) is by far the most common (16 to 1) */

		op = acpi_os_acquire_object(acpi_gbl_ps_node_cache);//从基础缓存分配8字节
	} else {//需要扩展存储的节点
		/* Extended parseop */

		op = acpi_os_acquire_object(acpi_gbl_ps_node_ext_cache);//从扩展缓存分配16字节
	}

	/* Initialize the Op */
	/* 初始化节点内容 */
	if (op) {
		acpi_ps_init_op(op, opcode);//基础初始化（清空+设置opcode）
		op->common.aml = aml;//记录AML原始位置（调试用）
		op->common.flags = flags;//设置节点类型标记
		ASL_CV_CLEAR_OP_COMMENTS(op);//清除注释字段（编译调试用）

		if (opcode == AML_SCOPE_OP) {//如果Scope操作需要更新全局当前作用域
			acpi_gbl_current_scope = op;//设置全局作用域指针
		}

		if (acpi_gbl_capture_comments) {//检查是否启用注释捕获
			ASL_CV_TRANSFER_COMMENTS(op);//转移注释信息到节点
		}
	}

	return (op);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ps_free_op
 *
 * PARAMETERS:  op              - Op to be freed
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Free an Op object. Either put it on the GENERIC_OP cache list
 *              or actually free it.
 *
 ******************************************************************************/

void acpi_ps_free_op(union acpi_parse_object *op)
{
	ACPI_FUNCTION_NAME(ps_free_op);

	ASL_CV_CLEAR_OP_COMMENTS(op);
	if (op->common.aml_opcode == AML_INT_RETURN_VALUE_OP) {
		ACPI_DEBUG_PRINT((ACPI_DB_ALLOCATIONS,
				  "Free retval op: %p\n", op));
	}

	if (op->common.flags & ACPI_PARSEOP_GENERIC) {
		(void)acpi_os_release_object(acpi_gbl_ps_node_cache, op);
	} else {
		(void)acpi_os_release_object(acpi_gbl_ps_node_ext_cache, op);
	}
}

/*******************************************************************************
 *
 * FUNCTION:    Utility functions
 *
 * DESCRIPTION: Low level character and object functions
 *
 ******************************************************************************/

/*
 * Is "c" a namestring lead character?
 */
u8 acpi_ps_is_leading_char(u32 c)
{
	return ((u8) (c == '_' || (c >= 'A' && c <= 'Z')));
}

/*
 * Get op's name (4-byte name segment) or 0 if unnamed
 */
u32 acpi_ps_get_name(union acpi_parse_object * op)
{

	/* The "generic" object has no name associated with it */

	if (op->common.flags & ACPI_PARSEOP_GENERIC) {
		return (0);
	}

	/* Only the "Extended" parse objects have a name */

	return (op->named.name);
}

/*
 * Set op's name
 */
void acpi_ps_set_name(union acpi_parse_object *op, u32 name)
{

	/* The "generic" object has no name associated with it */

	if (op->common.flags & ACPI_PARSEOP_GENERIC) {
		return;
	}

	op->named.name = name;
}
