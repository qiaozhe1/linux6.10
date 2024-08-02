// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Sunplus Core Technology Co., Ltd.
 *  Lennox Wu <lennox.wu@sunplusct.com>
 *  Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2013 Regents of the University of California
 */


#include <linux/bitfield.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <asm/asm-extable.h>
#include <asm/ptrace.h>

static inline unsigned long
get_ex_fixup(const struct exception_table_entry *ex)
{
	return ((unsigned long)&ex->fixup + ex->fixup);
}

static bool ex_handler_fixup(const struct exception_table_entry *ex,
			     struct pt_regs *regs)
{
	regs->epc = get_ex_fixup(ex);
	return true;
}

static inline unsigned long regs_get_gpr(struct pt_regs *regs, unsigned int offset)
{
	if (unlikely(!offset || offset > MAX_REG_OFFSET))
		return 0;

	return *(unsigned long *)((unsigned long)regs + offset);
}

static inline void regs_set_gpr(struct pt_regs *regs, unsigned int offset,
				unsigned long val)
{
	if (unlikely(offset > MAX_REG_OFFSET))
		return;

	if (offset)
		*(unsigned long *)((unsigned long)regs + offset) = val;
}

static bool ex_handler_uaccess_err_zero(const struct exception_table_entry *ex,
					struct pt_regs *regs)
{
	int reg_err = FIELD_GET(EX_DATA_REG_ERR, ex->data);
	int reg_zero = FIELD_GET(EX_DATA_REG_ZERO, ex->data);

	regs_set_gpr(regs, reg_err * sizeof(unsigned long), -EFAULT);
	regs_set_gpr(regs, reg_zero * sizeof(unsigned long), 0);

	regs->epc = get_ex_fixup(ex);
	return true;
}

static bool
ex_handler_load_unaligned_zeropad(const struct exception_table_entry *ex,
				  struct pt_regs *regs)
{
	int reg_data = FIELD_GET(EX_DATA_REG_DATA, ex->data);//从异常条目ex的数据字段中提取数据寄存器索引
	int reg_addr = FIELD_GET(EX_DATA_REG_ADDR, ex->data);//从异常表条目ex的数据字段中提取地址寄存器索引
	unsigned long data, addr, offset;

	addr = regs_get_gpr(regs, reg_addr * sizeof(unsigned long));//从寄存器regs中获取地址寄存器reg_addr的值并返回

	offset = addr & 0x7UL;//计算地址中的偏移量 
	addr &= ~0x7UL;//清除地址中的偏移量，得到对齐地址

	data = *(unsigned long *)addr >> (offset * 8);//从对齐地址 addr 中提取数据，根据偏移量 offset 右移数据

	regs_set_gpr(regs, reg_data * sizeof(unsigned long), data);//将提取的数据data写入regs对应的数据寄存器 中

	regs->epc = get_ex_fixup(ex);//设置 EPC（异常程序计数器）为修正后的地址
	return true;
}

bool fixup_exception(struct pt_regs *regs)//用于内核态修复各种类型的异常
{
	const struct exception_table_entry *ex;//声明异常表条目指针

	ex = search_exception_tables(regs->epc);//在异常表中搜索异常地址
	if (!ex)
		return false;//如果未找到异常表条目,返回 false，表示未能修复异常

	switch (ex->type) {//根据异常表条目的类型选择处理方式
	case EX_TYPE_FIXUP:
		return ex_handler_fixup(ex, regs);//处理修复异常
	case EX_TYPE_BPF:
		return ex_handler_bpf(ex, regs);//处理 BPF 异常
	case EX_TYPE_UACCESS_ERR_ZERO:
		return ex_handler_uaccess_err_zero(ex, regs);// 处理用户访问错误置零异常
	case EX_TYPE_LOAD_UNALIGNED_ZEROPAD:
		return ex_handler_load_unaligned_zeropad(ex, regs);//处理非对齐加载零填充异常
	}

	BUG();
}
