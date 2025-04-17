// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nsparse - namespace interface to AML parser
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acparser.h"
#include "acdispat.h"
#include "actables.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsparse")

/*******************************************************************************
 *
 * FUNCTION:    ns_execute_table
 *
 * PARAMETERS:  table_desc      - An ACPI table descriptor for table to parse
 *              start_node      - Where to enter the table into the namespace
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load ACPI/AML table by executing the entire table as a single
 *              large control method.
 *
 * NOTE: The point of this is to execute any module-level code in-place
 * as the table is parsed. Some AML code depends on this behavior.
 *
 * It is a run-time option at this time, but will eventually become
 * the default.
 *
 * Note: This causes the table to only have a single-pass parse.
 * However, this is compatible with other ACPI implementations.
 *
 ******************************************************************************/
/*
 * acpi_ns_execute_table - 执行ACPI表中的AML字节码（模块级代码）
 * @table_index: 全局表列表中目标表的索引
 * @start_node:  命名空间执行起点（通常为根节点）
 * 
 * 核心流程：
 * 1. 获取并验证ACPI表头
 * 2. 提取AML字节码段
 * 3. 创建临时方法对象封装AML代码
 * 4. 构建执行上下文环境
 * 5. 调用解析器执行AML代码
 * 
 * 关键设计：
 * - 将整个表视为临时方法对象执行
 * - 支持模块级代码（MLC）的立即执行
 * - 维护owner_id与表资源的关联
 */
acpi_status
acpi_ns_execute_table(u32 table_index, struct acpi_namespace_node *start_node)
{
	acpi_status status;
	struct acpi_table_header *table;//ACPI表头指针
	acpi_owner_id owner_id;//资源所有者ID
	struct acpi_evaluate_info *info = NULL;//方法评估信息结构
	u32 aml_length;//AML代码段长度
	u8 *aml_start;//AML代码起始地址
	union acpi_operand_object *method_obj = NULL;//临时方法对象指针

	ACPI_FUNCTION_TRACE(ns_execute_table);

	status = acpi_get_table_by_index(table_index, &table);//从全局表列表获取表指针
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* 表格必须至少包含一个完整的表头 */
	if (table->length < sizeof(struct acpi_table_header)) {//检查表长度是否合法
		return_ACPI_STATUS(AE_BAD_HEADER);
	}

	aml_start = (u8 *)table + sizeof(struct acpi_table_header);//计算AML代码段位置,AML起始地址=表头后
	aml_length = table->length - sizeof(struct acpi_table_header);//AML长度=总长-头长

	status = acpi_tb_get_owner_id(table_index, &owner_id);//从表描述符获取owner_id
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Create, initialize, and link a new temporary method object */

	method_obj = acpi_ut_create_internal_object(ACPI_TYPE_METHOD);//分配方法对象内存
	if (!method_obj) {//如果方法对象创建失败
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Allocate the evaluation information block */

	info = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_evaluate_info));//分配方法评估信息结构
	if (!info) {
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_PARSE,
			      "%s: Create table pseudo-method for [%4.4s] @%p, method %p\n",
			      ACPI_GET_FUNCTION_NAME, table->signature, table,
			      method_obj));

	/* 初始化方法对象属性,将整个表作为一个方法对象处理 */
	method_obj->method.aml_start = aml_start;//设置AML代码起始地址
	method_obj->method.aml_length = aml_length; // 设置AML代码长度
	method_obj->method.owner_id = owner_id;//绑定所有者ID
	method_obj->method.info_flags |= ACPI_METHOD_MODULE_LEVEL;//标记为模块级方法

	/* 构建评估上下文 */
	info->pass_number = ACPI_IMODE_EXECUTE;//设置为执行模式
	info->node = start_node;//设置起始命名空间节点(根命名空间节点)
	info->obj_desc = method_obj;//绑定方法对象
	info->node_flags = info->node->flags;//继承节点标志
	info->full_pathname = acpi_ns_get_normalized_pathname(info->node, TRUE);//获取节点路径
	if (!info->full_pathname) {//检查路径字符串分配
		status = AE_NO_MEMORY;
		goto cleanup;
	}

	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  (Definition Block level)\n",
			      "Module-level evaluation"));

	status = acpi_ps_execute_table(info);//核心执行：解析并执行AML字节码

	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  (Definition Block level)\n",
			      "Module-level complete"));

cleanup://资源回收标签
	if (info) {// 清理评估信息结构
		ACPI_FREE(info->full_pathname);//释放路径字符串内存
		info->full_pathname = NULL;//防止悬空指针
	}
	ACPI_FREE(info);//释放评估信息结构体
	acpi_ut_remove_reference(method_obj);//减少方法对象引用计数（可能释放内存）
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    ns_one_complete_parse
 *
 * PARAMETERS:  pass_number             - 1 or 2
 *              table_desc              - The table to be parsed.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Perform one complete parse of an ACPI/AML table.
 *
 ******************************************************************************/

acpi_status
acpi_ns_one_complete_parse(u32 pass_number,
			   u32 table_index,
			   struct acpi_namespace_node *start_node)
{
	union acpi_parse_object *parse_root;
	acpi_status status;
	u32 aml_length;
	u8 *aml_start;
	struct acpi_walk_state *walk_state;
	struct acpi_table_header *table;
	acpi_owner_id owner_id;

	ACPI_FUNCTION_TRACE(ns_one_complete_parse);

	status = acpi_get_table_by_index(table_index, &table);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Table must consist of at least a complete header */

	if (table->length < sizeof(struct acpi_table_header)) {
		return_ACPI_STATUS(AE_BAD_HEADER);
	}

	aml_start = (u8 *)table + sizeof(struct acpi_table_header);
	aml_length = table->length - sizeof(struct acpi_table_header);

	status = acpi_tb_get_owner_id(table_index, &owner_id);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Create and init a Root Node */

	parse_root = acpi_ps_create_scope_op(aml_start);
	if (!parse_root) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Create and initialize a new walk state */

	walk_state = acpi_ds_create_walk_state(owner_id, NULL, NULL, NULL);
	if (!walk_state) {
		acpi_ps_free_op(parse_root);
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	status = acpi_ds_init_aml_walk(walk_state, parse_root, NULL,
				       aml_start, aml_length, NULL,
				       (u8)pass_number);
	if (ACPI_FAILURE(status)) {
		acpi_ds_delete_walk_state(walk_state);
		goto cleanup;
	}

	/* Found OSDT table, enable the namespace override feature */

	if (ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_OSDT) &&
	    pass_number == ACPI_IMODE_LOAD_PASS1) {
		walk_state->namespace_override = TRUE;
	}

	/* start_node is the default location to load the table */

	if (start_node && start_node != acpi_gbl_root_node) {
		status =
		    acpi_ds_scope_stack_push(start_node, ACPI_TYPE_METHOD,
					     walk_state);
		if (ACPI_FAILURE(status)) {
			acpi_ds_delete_walk_state(walk_state);
			goto cleanup;
		}
	}

	/* Parse the AML */

	ACPI_DEBUG_PRINT((ACPI_DB_PARSE,
			  "*PARSE* pass %u parse\n", pass_number));
	acpi_ex_enter_interpreter();
	status = acpi_ps_parse_aml(walk_state);
	acpi_ex_exit_interpreter();

cleanup:
	acpi_ps_delete_parse_tree(parse_root);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_parse_table
 *
 * PARAMETERS:  table_desc      - An ACPI table descriptor for table to parse
 *              start_node      - Where to enter the table into the namespace
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Parse AML within an ACPI table and return a tree of ops
 *
 ******************************************************************************/
/*
 * acpi_ns_parse_table - 解析并执行ACPI表中的AML代码
 * @table_index: 需要解析的ACPI表索引
 * @start_node:  解析的起始命名空间节点（通常是根节点）
 *
 * 核心功能：
 * 1. 将整个ACPI表作为顶级控制方法执行
 * 2. 直接执行模块级代码（Module-Level Code, MLC）
 * 3. 单次解析模式（兼容其他ACPI实现）
 *
 * 关键特性：
 * - 即时执行模块级AML代码（硬件初始化关键阶段）
 * - 支持OperationRegion等需要立即生效的AML操作
 * - 与DSDT/SSDT表的解析行为保持一致
 */
acpi_status
acpi_ns_parse_table(u32 table_index, struct acpi_namespace_node *start_node)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_parse_table);

	/*
	 * 将 AML 表作为一个大的控制方法执行。
	 * 这样做的目的是在解析表时就原地执行任何模块级别的代码。一些 AML 代
	 * 码依赖于这种行为。
	 *
	 * 注意：这会导致表只有单次解析。然而，这与其他 ACPI 实现兼容。
	 */
	ACPI_DEBUG_PRINT_RAW((ACPI_DB_PARSE,
			      "%s: **** Start table execution pass\n",
			      ACPI_GET_FUNCTION_NAME));

	status = acpi_ns_execute_table(table_index, start_node);//核心执行函数：实际解析并执行AML代码

	return_ACPI_STATUS(status);//返回时添加调试跟踪信息
}
