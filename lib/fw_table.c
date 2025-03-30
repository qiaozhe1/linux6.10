// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  fw_tables.c - Parsing support for ACPI and ACPI-like tables provided by
 *                platform or device firmware
 *
 *  Copyright (C) 2001 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2023 Intel Corp.
 */
#include <linux/errno.h>
#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/fw_table.h>

enum acpi_subtable_type {
	ACPI_SUBTABLE_COMMON,
	ACPI_SUBTABLE_HMAT,
	ACPI_SUBTABLE_PRMT,
	ACPI_SUBTABLE_CEDT,
	CDAT_SUBTABLE,
};

struct acpi_subtable_entry {
	union acpi_subtable_headers *hdr;
	enum acpi_subtable_type type;
};

static unsigned long __init_or_fwtbl_lib
acpi_get_entry_type(struct acpi_subtable_entry *entry)
{
	switch (entry->type) {
	case ACPI_SUBTABLE_COMMON:
		return entry->hdr->common.type;
	case ACPI_SUBTABLE_HMAT:
		return entry->hdr->hmat.type;
	case ACPI_SUBTABLE_PRMT:
		return 0;
	case ACPI_SUBTABLE_CEDT:
		return entry->hdr->cedt.type;
	case CDAT_SUBTABLE:
		return entry->hdr->cdat.type;
	}
	return 0;
}

static unsigned long __init_or_fwtbl_lib
acpi_get_entry_length(struct acpi_subtable_entry *entry)
{
	switch (entry->type) {
	case ACPI_SUBTABLE_COMMON:
		return entry->hdr->common.length;
	case ACPI_SUBTABLE_HMAT:
		return entry->hdr->hmat.length;
	case ACPI_SUBTABLE_PRMT:
		return entry->hdr->prmt.length;
	case ACPI_SUBTABLE_CEDT:
		return entry->hdr->cedt.length;
	case CDAT_SUBTABLE: {
		__le16 length = (__force __le16)entry->hdr->cdat.length;

		return le16_to_cpu(length);
	}
	}
	return 0;
}

static unsigned long __init_or_fwtbl_lib
acpi_get_subtable_header_length(struct acpi_subtable_entry *entry)
{
	switch (entry->type) {
	case ACPI_SUBTABLE_COMMON:
		return sizeof(entry->hdr->common);
	case ACPI_SUBTABLE_HMAT:
		return sizeof(entry->hdr->hmat);
	case ACPI_SUBTABLE_PRMT:
		return sizeof(entry->hdr->prmt);
	case ACPI_SUBTABLE_CEDT:
		return sizeof(entry->hdr->cedt);
	case CDAT_SUBTABLE:
		return sizeof(entry->hdr->cdat);
	}
	return 0;
}

static enum acpi_subtable_type __init_or_fwtbl_lib
acpi_get_subtable_type(char *id)
{
	if (strncmp(id, ACPI_SIG_HMAT, 4) == 0)
		return ACPI_SUBTABLE_HMAT;
	if (strncmp(id, ACPI_SIG_PRMT, 4) == 0)
		return ACPI_SUBTABLE_PRMT;
	if (strncmp(id, ACPI_SIG_CEDT, 4) == 0)
		return ACPI_SUBTABLE_CEDT;
	if (strncmp(id, ACPI_SIG_CDAT, 4) == 0)
		return CDAT_SUBTABLE;
	return ACPI_SUBTABLE_COMMON;
}

static unsigned long __init_or_fwtbl_lib
acpi_table_get_length(enum acpi_subtable_type type,
		      union fw_table_header *header)
{
	if (type == CDAT_SUBTABLE) {
		__le32 length = (__force __le32)header->cdat.length;

		return le32_to_cpu(length);
	}

	return header->acpi.length;
}

static __init_or_fwtbl_lib int call_handler(struct acpi_subtable_proc *proc,
					    union acpi_subtable_headers *hdr,
					    unsigned long end)
{
	if (proc->handler)
		return proc->handler(hdr, end);
	if (proc->handler_arg)
		return proc->handler_arg(hdr, proc->arg, end);
	return -EINVAL;
}

/**
 * acpi_parse_entries_array - for each proc_num find a suitable subtable
 *
 * @id: table id (for debugging purposes)
 * @table_size: size of the root table
 * @max_length: maximum size of the table (ignore if 0)
 * @table_header: where does the table start?
 * @proc: array of acpi_subtable_proc struct containing entry id
 *        and associated handler with it
 * @proc_num: how big proc is?
 * @max_entries: how many entries can we process?
 *
 * For each proc_num find a subtable with proc->id and run proc->handler
 * on it. Assumption is that there's only single handler for particular
 * entry id.
 *
 * The table_size is not the size of the complete ACPI table (the length
 * field in the header struct), but only the size of the root table; i.e.,
 * the offset from the very first byte of the complete ACPI table, to the
 * first byte of the very first subtable.
 *
 * On success returns sum of all matching entries for all proc handlers.
 * Otherwise, -ENODEV or -EINVAL is returned.
 */
/**
 * acpi_parse_entries_array - 解析ACPI表中的多个子表条目
 * @id: ACPI表签名(4字符)
 * @table_size: 基本表头大小
 * @table_header: ACPI表头指针
 * @max_length: 最大解析长度(0表示不限制)
 * @proc: 子表处理信息数组
 * @proc_num: 处理信息数组元素个数
 * @max_entries: 最大处理条目数(0表示不限制)
 *
 * 返回值:
 *   成功处理的条目数, 或错误码(<0)
 *
 * 功能说明:
 * 1. 计算表长度和边界
 * 2. 遍历所有子表条目
 * 3. 匹配并调用对应的处理函数
 * 4. 处理长度检查和边界条件
 */
int __init_or_fwtbl_lib
acpi_parse_entries_array(char *id, unsigned long table_size,
			 union fw_table_header *table_header,
			 unsigned long max_length,
			 struct acpi_subtable_proc *proc,
			 int proc_num, unsigned int max_entries)
{
	unsigned long table_len, table_end, subtable_len, entry_len;
	struct acpi_subtable_entry entry;//当前处理的子表条目
	enum acpi_subtable_type type;//子表类型
	int count = 0;//成功处理的条目计数器
	int i;//循环计数器

	type = acpi_get_subtable_type(id);//根据表ID获取子表类型
	table_len = acpi_table_get_length(type, table_header);//获取ACPI表的总长度
	if (max_length && max_length < table_len)//如果指定了最大长度且小于表长度，则使用最大长度
		table_len = max_length;
	table_end = (unsigned long)table_header + table_len;//计算表的结束地址 = 起始地址 + 表长度

	/* 解析所有条目以寻找匹配项。 */

	entry.type = type;//设置子表类型
	entry.hdr = (union acpi_subtable_headers *)
	    ((unsigned long)table_header + table_size);//计算第一个子表的位置 = 表头地址 + 表头大小
	subtable_len = acpi_get_subtable_header_length(&entry);//获取子表头的长度

	while (((unsigned long)entry.hdr) + subtable_len < table_end) {//循环处理所有子表，直到超出表边界
		for (i = 0; i < proc_num; i++) {//遍历所有处理程序
			if (acpi_get_entry_type(&entry) != proc[i].id)//检查当前条目类型是否匹配处理程序ID
				continue;//不匹配则跳过

			if (!max_entries || count < max_entries)// 检查条目数量限制
				if (call_handler(&proc[i], entry.hdr, table_end))//调用处理函数，失败则返回错误
					return -EINVAL;

			proc[i].count++;//增加该处理程序的成功计数
			count++;//增加总成功计数
			break;//匹配后跳出循环
		}

		/*
		 * If entry->length is 0, break from this loop to avoid
		 * infinite loop.
		 */
		entry_len = acpi_get_entry_length(&entry);//获取当前条目的长度
		if (entry_len == 0) {//检查长度是否为0（防止无限循环）
			pr_err("[%4.4s:0x%02x] Invalid zero length\n", id, proc->id);//打印错误信息：表ID和处理程序ID
			return -EINVAL;
		}

		entry.hdr = (union acpi_subtable_headers *)
		    ((unsigned long)entry.hdr + entry_len);//移动到下一个子表：当前地址 + 当前长度
	}

	if (max_entries && count > max_entries) {//检查是否超过最大条目限制
		pr_warn("[%4.4s:0x%02x] ignored %i entries of %i found\n",
			id, proc->id, count - max_entries, count);
	}

	return count;//返回成功处理的条目总数
}

int __init_or_fwtbl_lib
cdat_table_parse(enum acpi_cdat_type type,
		 acpi_tbl_entry_handler_arg handler_arg,
		 void *arg,
		 struct acpi_table_cdat *table_header,
		 unsigned long length)
{
	struct acpi_subtable_proc proc = {
		.id		= type,
		.handler_arg	= handler_arg,
		.arg		= arg,
	};

	if (!table_header)
		return -EINVAL;

	return acpi_parse_entries_array(ACPI_SIG_CDAT,
					sizeof(struct acpi_table_cdat),
					(union fw_table_header *)table_header,
					length, &proc, 1, 0);
}
EXPORT_SYMBOL_FWTBL_LIB(cdat_table_parse);
