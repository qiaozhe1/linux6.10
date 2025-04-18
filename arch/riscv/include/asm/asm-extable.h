/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ASM_EXTABLE_H
#define __ASM_ASM_EXTABLE_H

#define EX_TYPE_NONE			0	//表示没有异常类型，通常用作默认值或占位符。
#define EX_TYPE_FIXUP			1	//表示修复异常
#define EX_TYPE_BPF			2	//表示 BPF（Berkeley Packet Filter）异常
#define EX_TYPE_UACCESS_ERR_ZERO	3	//表示用户访问错误置零异常
#define EX_TYPE_LOAD_UNALIGNED_ZEROPAD	4	//非对齐加载零填充异常

#ifdef CONFIG_MMU

#ifdef __ASSEMBLY__

#define __ASM_EXTABLE_RAW(insn, fixup, type, data)	\
	.pushsection	__ex_table, "a";		\
	.balign		4;				\
	.long		((insn) - .);			\
	.long		((fixup) - .);			\
	.short		(type);				\
	.short		(data);				\
	.popsection;

	.macro		_asm_extable, insn, fixup
	__ASM_EXTABLE_RAW(\insn, \fixup, EX_TYPE_FIXUP, 0)
	.endm

#else /* __ASSEMBLY__ */

#include <linux/bits.h>
#include <linux/stringify.h>
#include <asm/gpr-num.h>

#define __ASM_EXTABLE_RAW(insn, fixup, type, data)	\
	".pushsection	__ex_table, \"a\"\n"		\
	".balign	4\n"				\
	".long		((" insn ") - .)\n"		\
	".long		((" fixup ") - .)\n"		\
	".short		(" type ")\n"			\
	".short		(" data ")\n"			\
	".popsection\n"

#define _ASM_EXTABLE(insn, fixup)	\
	__ASM_EXTABLE_RAW(#insn, #fixup, __stringify(EX_TYPE_FIXUP), "0")

#define EX_DATA_REG_ERR_SHIFT	0
#define EX_DATA_REG_ERR		GENMASK(4, 0)
#define EX_DATA_REG_ZERO_SHIFT	5
#define EX_DATA_REG_ZERO	GENMASK(9, 5)

#define EX_DATA_REG_DATA_SHIFT	0
#define EX_DATA_REG_DATA	GENMASK(4, 0)
#define EX_DATA_REG_ADDR_SHIFT	5
#define EX_DATA_REG_ADDR	GENMASK(9, 5)

#define EX_DATA_REG(reg, gpr)						\
	"((.L__gpr_num_" #gpr ") << " __stringify(EX_DATA_REG_##reg##_SHIFT) ")"

#define _ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, zero)		\
	__DEFINE_ASM_GPR_NUMS						\
	__ASM_EXTABLE_RAW(#insn, #fixup, 				\
			  __stringify(EX_TYPE_UACCESS_ERR_ZERO),	\
			  "("						\
			    EX_DATA_REG(ERR, err) " | "			\
			    EX_DATA_REG(ZERO, zero)			\
			  ")")

#define _ASM_EXTABLE_UACCESS_ERR(insn, fixup, err)			\
	_ASM_EXTABLE_UACCESS_ERR_ZERO(insn, fixup, err, zero)

#define _ASM_EXTABLE_LOAD_UNALIGNED_ZEROPAD(insn, fixup, data, addr)		\
	__DEFINE_ASM_GPR_NUMS							\
	__ASM_EXTABLE_RAW(#insn, #fixup,					\
			  __stringify(EX_TYPE_LOAD_UNALIGNED_ZEROPAD),		\
			  "("							\
			    EX_DATA_REG(DATA, data) " | "			\
			    EX_DATA_REG(ADDR, addr)				\
			  ")")

#endif /* __ASSEMBLY__ */

#else /* CONFIG_MMU */
	#define _ASM_EXTABLE_UACCESS_ERR(insn, fixup, err)
#endif /* CONFIG_MMU */

#endif /* __ASM_ASM_EXTABLE_H */
