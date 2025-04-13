// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: tbxfload - Table load/unload external interfaces
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#define EXPORT_ACPI_INTERFACES

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "actables.h"
#include "acevents.h"

#define _COMPONENT          ACPI_TABLES
ACPI_MODULE_NAME("tbxfload")

/*******************************************************************************
 *
 * FUNCTION:    acpi_load_tables
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load the ACPI tables from the RSDT/XSDT
 *
 ******************************************************************************/
acpi_status ACPI_INIT_FUNCTION acpi_load_tables(void)//加载ACPI表并初始化命名空间。
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_load_tables);

	/*
	 * 安装默认的操作区域处理程序。这些是由ACPI规范定义为“始终可用”的
	 * 处理程序——即，system_memory（系统内存）、system_IO（系统I/O）和
	 * PCI_Config（PCI配置）。这也意味着这些地址空间无需运行任何_REG方
	 * 法。我们需要在这些AML代码执行之前安装这些处理程序，尤其是任何模
	 * 块级别的代码（2015年11月）。
	 * 请注意，我们允许OSPM在acpi_initialize_subsystem()和acpi_load_tables()
	 * 之间安装他们自己的区域处理器，以使用他们定制的默认区域处理器。
	 */
	status = acpi_ev_install_region_handlers();//注册ACPI规范定义的默认区域处理程序:系统内存,系统I/O,PCI配置空间(访问PCI设备寄存器)
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"During Region initialization"));
		return_ACPI_STATUS(status);
	}

	/* 从表格中加载命名空间 */
	status = acpi_tb_load_namespace();//解析所有已加载的ACPI表（如DSDT、SSDT等），将命名空间对象（如方法、操作区域）填充到内核的命名空间树中。

	/* Don't let single failures abort the load */

	if (status == AE_CTRL_TERMINATE) {//AE_CTRL_TERMINATE：表示加载过程中遇到可恢复错误（如无效表），但继续加载其他表。
		status = AE_OK;
	}

	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"While loading namespace from ACPI tables"));
	}

	/*
	 * Initialize the objects in the namespace that remain uninitialized.
	 * This runs the executable AML that may be part of the declaration of
	 * these name objects:
	 *     operation_regions, buffer_fields, Buffers, and Packages.
	 *
	 */
	status = acpi_ns_initialize_objects();//执行命名空间中未初始化对象的AML代码:操作区域(初始化硬件寄存器映射。),缓冲区字段(解析缓冲区结构),静态数据(初始化预定义数据结构)
	if (ACPI_SUCCESS(status)) {
		acpi_gbl_namespace_initialized = TRUE;//acpi_gbl_namespace_initialized表示命名空间已完全初始化，允许后续ACPI方法执行。
	}

	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL_INIT(acpi_load_tables)

/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_load_namespace
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load the namespace from the DSDT and all SSDTs/PSDTs found in
 *              the RSDT/XSDT.
 *
 ******************************************************************************/
/* 
 * acpi_tb_load_namespace - 加载并解析ACPI表到命名空间
 * 
 * 返回值:
 *  AE_OK            - 所有表成功加载
 *  AE_CTRL_TERMINATE - 部分表加载失败（但部分成功）
 *  AE_NO_ACPI_TABLES - 无有效DSDT表
 * 
 * 功能描述:
 *  1. 验证并加载DSDT表（必须存在）
 *  2. 可选复制DSDT以防止BIOS篡改
 *  3. 加载所有SSDT/PSDT等辅助表
 *  4. 统计加载结果并输出日志
 */
acpi_status acpi_tb_load_namespace(void)
{
	acpi_status status;
	u32 i;
	struct acpi_table_header *new_dsdt;
	struct acpi_table_desc *table;
	u32 tables_loaded = 0;//成功加载表计数
	u32 tables_failed = 0;//加载失败表计数

	ACPI_FUNCTION_TRACE(tb_load_namespace);

	(void)acpi_ut_acquire_mutex(ACPI_MTX_TABLES);//获取全局表锁

	/*
	 * 加载命名空间。DSDT是必需的，但SSDT和PSDT表是可选的。验证DSDT。
	 */
	table = &acpi_gbl_root_table_list.tables[acpi_gbl_dsdt_index];//获取DSDT表描述符

	if (!acpi_gbl_root_table_list.current_table_count ||//检查表数量是否为0
	    !ACPI_COMPARE_NAMESEG(table->signature.ascii, ACPI_SIG_DSDT) ||// 检查签名是否为DSDT
	    ACPI_FAILURE(acpi_tb_validate_table(table))) {//验证表完整性
		status = AE_NO_ACPI_TABLES;//返回无有效ACPI表错误
		goto unlock_and_exit;//跳转到清理流程
	}

	/*
	 * 保存 DSDT 指针以便简单访问。这是映射的内存地址。我们需要在此处小心，
	 * 因为随着表格在运行时加载，.Tables 数组的地址可能会动态变化。
	 * 注意：.Pointer 字段在调用 acpi_tb_validate_table 之后才会进行验证。
	 */
	acpi_gbl_DSDT = table->pointer;//映射后的DSDT内存指针

	/*
	 * 可选地将整个DSDT复制到本地内存（而不是简单地映射它）。有些BIOS会破坏或
	 * 替换原始的DSDT，从而需要这个选项。默认值为FALSE，不复制DSDT。
	 */
	if (acpi_gbl_copy_dsdt_locally) {// 根据配置标志决定(可选操作)
		new_dsdt = acpi_tb_copy_dsdt(acpi_gbl_dsdt_index);//深拷贝DSDT到本地内存
		if (new_dsdt) {//如果拷贝成功
			acpi_gbl_DSDT = new_dsdt;//使用拷贝后的DSDT
		}
	}

	/*
	 * 保存原始的DSDT头部，以便检测表格损坏以及/或者从操作系统外部替换DSDT。
	 */
	memcpy(&acpi_gbl_original_dsdt_header, acpi_gbl_DSDT,
	       sizeof(struct acpi_table_header));//复制头结构前48字节

	/* 加载并解析DSDT到命名空间 */
	(void)acpi_ut_release_mutex(ACPI_MTX_TABLES);//临时释放锁
	status = acpi_ns_load_table(acpi_gbl_dsdt_index, acpi_gbl_root_node);//解析DSDT AML
	(void)acpi_ut_acquire_mutex(ACPI_MTX_TABLES);//重新获取锁
	if (ACPI_FAILURE(status)) {//如果解析失败
		ACPI_EXCEPTION((AE_INFO, status, "[DSDT] table load failed"));
		tables_failed++;//失败计数
	} else {
		tables_loaded++;//成功计数
	}

	/* 加载其他辅助表（SSDT/PSDT/OSDT） 注意：循环会使表格保持锁定状态。 */
	for (i = 0; i < acpi_gbl_root_table_list.current_table_count; ++i) {
		table = &acpi_gbl_root_table_list.tables[i];//获取当前表描述符

		/* 跳过无效表：无地址或非SSDT/PSDT/OSDT类型 */
		if (!table->address ||
		    (!ACPI_COMPARE_NAMESEG
		     (table->signature.ascii, ACPI_SIG_SSDT)
		     && !ACPI_COMPARE_NAMESEG(table->signature.ascii,
					      ACPI_SIG_PSDT)
		     && !ACPI_COMPARE_NAMESEG(table->signature.ascii,
					      ACPI_SIG_OSDT))
		    || ACPI_FAILURE(acpi_tb_validate_table(table))) {
			continue;// 跳过当前表
		}

		/* 在加载数据表时忽略错误，尽可能多地获取数据 */
		(void)acpi_ut_release_mutex(ACPI_MTX_TABLES);// 释放表锁
		status = acpi_ns_load_table(i, acpi_gbl_root_node);//解析表到命名空间
		(void)acpi_ut_acquire_mutex(ACPI_MTX_TABLES);
		if (ACPI_FAILURE(status)) {//如果解析失败
			ACPI_EXCEPTION((AE_INFO, status,
					"(%4.4s:%8.8s) while loading table",
					table->signature.ascii,
					table->pointer->oem_table_id));

			tables_failed++;//失败计数

			ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
					      "Table [%4.4s:%8.8s] (id FF) - Table namespace load failed\n\n",
					      table->signature.ascii,
					      table->pointer->oem_table_id));
		} else {
			tables_loaded++;//成功计数
		}
	}

	/* 统计并输出加载结果 */
	if (!tables_failed) {
		ACPI_INFO(("%u ACPI AML tables successfully acquired and loaded", tables_loaded));
	} else {
		ACPI_ERROR((AE_INFO,
			    "%u table load failures, %u successful",
			    tables_failed, tables_loaded));

		/* Indicate at least one failure */

		status = AE_CTRL_TERMINATE;
	}

#ifdef ACPI_APPLICATION
	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT, "\n"));
#endif

unlock_and_exit:
	(void)acpi_ut_release_mutex(ACPI_MTX_TABLES);// 释放表锁
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_install_table
 *
 * PARAMETERS:  table               - Pointer to the ACPI table to be installed.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Dynamically install an ACPI table.
 *              Note: This function should only be invoked after
 *                    acpi_initialize_tables() and before acpi_load_tables().
 *
 ******************************************************************************/

acpi_status ACPI_INIT_FUNCTION
acpi_install_table(struct acpi_table_header *table)
{
	acpi_status status;
	u32 table_index;

	ACPI_FUNCTION_TRACE(acpi_install_table);

	status = acpi_tb_install_standard_table(ACPI_PTR_TO_PHYSADDR(table),
						ACPI_TABLE_ORIGIN_EXTERNAL_VIRTUAL,
						table, FALSE, FALSE,
						&table_index);

	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL_INIT(acpi_install_table)

/*******************************************************************************
 *
 * FUNCTION:    acpi_install_physical_table
 *
 * PARAMETERS:  address             - Address of the ACPI table to be installed.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Dynamically install an ACPI table.
 *              Note: This function should only be invoked after
 *                    acpi_initialize_tables() and before acpi_load_tables().
 *
 ******************************************************************************/
acpi_status ACPI_INIT_FUNCTION
acpi_install_physical_table(acpi_physical_address address)
{
	acpi_status status;
	u32 table_index;

	ACPI_FUNCTION_TRACE(acpi_install_physical_table);

	status = acpi_tb_install_standard_table(address,
						ACPI_TABLE_ORIGIN_INTERNAL_PHYSICAL,
						NULL, FALSE, FALSE,
						&table_index);

	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL_INIT(acpi_install_physical_table)

/*******************************************************************************
 *
 * FUNCTION:    acpi_load_table
 *
 * PARAMETERS:  table               - Pointer to a buffer containing the ACPI
 *                                    table to be loaded.
 *              table_idx           - Pointer to a u32 for storing the table
 *                                    index, might be NULL
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Dynamically load an ACPI table from the caller's buffer. Must
 *              be a valid ACPI table with a valid ACPI table header.
 *              Note1: Mainly intended to support hotplug addition of SSDTs.
 *              Note2: Does not copy the incoming table. User is responsible
 *              to ensure that the table is not deleted or unmapped.
 *
 ******************************************************************************/
acpi_status acpi_load_table(struct acpi_table_header *table, u32 *table_idx)
{
	acpi_status status;
	u32 table_index;

	ACPI_FUNCTION_TRACE(acpi_load_table);

	/* Parameter validation */

	if (!table) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* Install the table and load it into the namespace */

	ACPI_INFO(("Host-directed Dynamic ACPI Table Load:"));
	status = acpi_tb_install_and_load_table(ACPI_PTR_TO_PHYSADDR(table),
						ACPI_TABLE_ORIGIN_EXTERNAL_VIRTUAL,
						table, FALSE, &table_index);
	if (table_idx) {
		*table_idx = table_index;
	}

	if (ACPI_SUCCESS(status)) {

		/* Complete the initialization/resolution of new objects */

		acpi_ns_initialize_objects();
	}

	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_load_table)

/*******************************************************************************
 *
 * FUNCTION:    acpi_unload_parent_table
 *
 * PARAMETERS:  object              - Handle to any namespace object owned by
 *                                    the table to be unloaded
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Via any namespace object within an SSDT or OEMx table, unloads
 *              the table and deletes all namespace objects associated with
 *              that table. Unloading of the DSDT is not allowed.
 *              Note: Mainly intended to support hotplug removal of SSDTs.
 *
 ******************************************************************************/
acpi_status acpi_unload_parent_table(acpi_handle object)
{
	struct acpi_namespace_node *node =
	    ACPI_CAST_PTR(struct acpi_namespace_node, object);
	acpi_status status = AE_NOT_EXIST;
	acpi_owner_id owner_id;
	u32 i;

	ACPI_FUNCTION_TRACE(acpi_unload_parent_table);

	/* Parameter validation */

	if (!object) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/*
	 * The node owner_id is currently the same as the parent table ID.
	 * However, this could change in the future.
	 */
	owner_id = node->owner_id;
	if (!owner_id) {

		/* owner_id==0 means DSDT is the owner. DSDT cannot be unloaded */

		return_ACPI_STATUS(AE_TYPE);
	}

	/* Must acquire the table lock during this operation */

	status = acpi_ut_acquire_mutex(ACPI_MTX_TABLES);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Find the table in the global table list */

	for (i = 0; i < acpi_gbl_root_table_list.current_table_count; i++) {
		if (owner_id != acpi_gbl_root_table_list.tables[i].owner_id) {
			continue;
		}

		/*
		 * Allow unload of SSDT and OEMx tables only. Do not allow unload
		 * of the DSDT. No other types of tables should get here, since
		 * only these types can contain AML and thus are the only types
		 * that can create namespace objects.
		 */
		if (ACPI_COMPARE_NAMESEG
		    (acpi_gbl_root_table_list.tables[i].signature.ascii,
		     ACPI_SIG_DSDT)) {
			status = AE_TYPE;
			break;
		}

		(void)acpi_ut_release_mutex(ACPI_MTX_TABLES);
		status = acpi_tb_unload_table(i);
		(void)acpi_ut_acquire_mutex(ACPI_MTX_TABLES);
		break;
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_TABLES);
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_unload_parent_table)
/*******************************************************************************
 *
 * FUNCTION:    acpi_unload_table
 *
 * PARAMETERS:  table_index         - Index as returned by acpi_load_table
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Via the table_index representing an SSDT or OEMx table, unloads
 *              the table and deletes all namespace objects associated with
 *              that table. Unloading of the DSDT is not allowed.
 *              Note: Mainly intended to support hotplug removal of SSDTs.
 *
 ******************************************************************************/
acpi_status acpi_unload_table(u32 table_index)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_unload_table);

	if (table_index == 1) {

		/* table_index==1 means DSDT is the owner. DSDT cannot be unloaded */

		return_ACPI_STATUS(AE_TYPE);
	}

	status = acpi_tb_unload_table(table_index);
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL(acpi_unload_table)
