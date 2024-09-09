// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copied from arch/arm64/kernel/cpufeature.c
 *
 * Copyright (C) 2015 ARM Ltd.
 * Copyright (C) 2017 SiFive
 */

#include <linux/acpi.h>
#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/ctype.h>
#include <linux/log2.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/of.h>
#include <asm/acpi.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/hwcap.h>
#include <asm/patch.h>
#include <asm/processor.h>
#include <asm/sbi.h>
#include <asm/vector.h>

#define NUM_ALPHA_EXTS ('z' - 'a' + 1)

unsigned long elf_hwcap __read_mostly;

/* Host ISA bitmap */
static DECLARE_BITMAP(riscv_isa, RISCV_ISA_EXT_MAX) __read_mostly;

/* Per-cpu ISA extensions. */
struct riscv_isainfo hart_isa[NR_CPUS];

/**
 * riscv_isa_extension_base() - Get base extension word
 *
 * @isa_bitmap: ISA bitmap to use
 * Return: base extension word as unsigned long value
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
unsigned long riscv_isa_extension_base(const unsigned long *isa_bitmap)
{
	if (!isa_bitmap)
		return riscv_isa[0];
	return isa_bitmap[0];
}
EXPORT_SYMBOL_GPL(riscv_isa_extension_base);

/**
 * __riscv_isa_extension_available() - Check whether given extension
 * is available or not
 *
 * @isa_bitmap: ISA bitmap to use
 * @bit: bit position of the desired extension
 * Return: true or false
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, unsigned int bit)
{
	const unsigned long *bmap = (isa_bitmap) ? isa_bitmap : riscv_isa;//如果提供了 isa_bitmap，则使用它；否则使用默认的riscv_isa位图

	if (bit >= RISCV_ISA_EXT_MAX)//如果传入的 bit 值大于或等于支持的最大扩展标识符，则返回 false
		return false;

	return test_bit(bit, bmap) ? true : false;//检查 bmap 中是否设置了 bit 位，如果是则返回 true，否则返回 false
}
EXPORT_SYMBOL_GPL(__riscv_isa_extension_available);

static bool riscv_isa_extension_check(int id)
{
	switch (id) {
	case RISCV_ISA_EXT_ZICBOM:
		if (!riscv_cbom_block_size) {
			pr_err("Zicbom detected in ISA string, disabling as no cbom-block-size found\n");
			return false;
		} else if (!is_power_of_2(riscv_cbom_block_size)) {
			pr_err("Zicbom disabled as cbom-block-size present, but is not a power-of-2\n");
			return false;
		}
		return true;
	case RISCV_ISA_EXT_ZICBOZ:
		if (!riscv_cboz_block_size) {
			pr_err("Zicboz detected in ISA string, disabling as no cboz-block-size found\n");
			return false;
		} else if (!is_power_of_2(riscv_cboz_block_size)) {
			pr_err("Zicboz disabled as cboz-block-size present, but is not a power-of-2\n");
			return false;
		}
		return true;
	case RISCV_ISA_EXT_INVALID:
		return false;
	}

	return true;
}

#define _RISCV_ISA_EXT_DATA(_name, _id, _subset_exts, _subset_exts_size) {	\
	.name = #_name,								\
	.property = #_name,							\
	.id = _id,								\
	.subset_ext_ids = _subset_exts,						\
	.subset_ext_size = _subset_exts_size					\
}

#define __RISCV_ISA_EXT_DATA(_name, _id) _RISCV_ISA_EXT_DATA(_name, _id, NULL, 0)

/* Used to declare pure "lasso" extension (Zk for instance) */
#define __RISCV_ISA_EXT_BUNDLE(_name, _bundled_exts) \
	_RISCV_ISA_EXT_DATA(_name, RISCV_ISA_EXT_INVALID, _bundled_exts, ARRAY_SIZE(_bundled_exts))

/* Used to declare extensions that are a superset of other extensions (Zvbb for instance) */
#define __RISCV_ISA_EXT_SUPERSET(_name, _id, _sub_exts) \
	_RISCV_ISA_EXT_DATA(_name, _id, _sub_exts, ARRAY_SIZE(_sub_exts))

static const unsigned int riscv_zk_bundled_exts[] = {
	RISCV_ISA_EXT_ZBKB,
	RISCV_ISA_EXT_ZBKC,
	RISCV_ISA_EXT_ZBKX,
	RISCV_ISA_EXT_ZKND,
	RISCV_ISA_EXT_ZKNE,
	RISCV_ISA_EXT_ZKR,
	RISCV_ISA_EXT_ZKT,
};

static const unsigned int riscv_zkn_bundled_exts[] = {
	RISCV_ISA_EXT_ZBKB,
	RISCV_ISA_EXT_ZBKC,
	RISCV_ISA_EXT_ZBKX,
	RISCV_ISA_EXT_ZKND,
	RISCV_ISA_EXT_ZKNE,
	RISCV_ISA_EXT_ZKNH,
};

static const unsigned int riscv_zks_bundled_exts[] = {
	RISCV_ISA_EXT_ZBKB,
	RISCV_ISA_EXT_ZBKC,
	RISCV_ISA_EXT_ZKSED,
	RISCV_ISA_EXT_ZKSH
};

#define RISCV_ISA_EXT_ZVKN	\
	RISCV_ISA_EXT_ZVKNED,	\
	RISCV_ISA_EXT_ZVKNHB,	\
	RISCV_ISA_EXT_ZVKB,	\
	RISCV_ISA_EXT_ZVKT

static const unsigned int riscv_zvkn_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKN
};

static const unsigned int riscv_zvknc_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKN,
	RISCV_ISA_EXT_ZVBC
};

static const unsigned int riscv_zvkng_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKN,
	RISCV_ISA_EXT_ZVKG
};

#define RISCV_ISA_EXT_ZVKS	\
	RISCV_ISA_EXT_ZVKSED,	\
	RISCV_ISA_EXT_ZVKSH,	\
	RISCV_ISA_EXT_ZVKB,	\
	RISCV_ISA_EXT_ZVKT

static const unsigned int riscv_zvks_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKS
};

static const unsigned int riscv_zvksc_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKS,
	RISCV_ISA_EXT_ZVBC
};

static const unsigned int riscv_zvksg_bundled_exts[] = {
	RISCV_ISA_EXT_ZVKS,
	RISCV_ISA_EXT_ZVKG
};

static const unsigned int riscv_zvbb_exts[] = {
	RISCV_ISA_EXT_ZVKB
};

/*
 * While the [ms]envcfg CSRs were not defined until version 1.12 of the RISC-V
 * privileged ISA, the existence of the CSRs is implied by any extension which
 * specifies [ms]envcfg bit(s). Hence, we define a custom ISA extension for the
 * existence of the CSR, and treat it as a subset of those other extensions.
 */
static const unsigned int riscv_xlinuxenvcfg_exts[] = {
	RISCV_ISA_EXT_XLINUXENVCFG
};

/*
 * The canonical order of ISA extension names in the ISA string is defined in
 * chapter 27 of the unprivileged specification.
 *
 * Ordinarily, for in-kernel data structures, this order is unimportant but
 * isa_ext_arr defines the order of the ISA string in /proc/cpuinfo.
 *
 * The specification uses vague wording, such as should, when it comes to
 * ordering, so for our purposes the following rules apply:
 *
 * 1. All multi-letter extensions must be separated from other extensions by an
 *    underscore.
 *
 * 2. Additional standard extensions (starting with 'Z') must be sorted after
 *    single-letter extensions and before any higher-privileged extensions.
 *
 * 3. The first letter following the 'Z' conventionally indicates the most
 *    closely related alphabetical extension category, IMAFDQLCBKJTPVH.
 *    If multiple 'Z' extensions are named, they must be ordered first by
 *    category, then alphabetically within a category.
 *
 * 3. Standard supervisor-level extensions (starting with 'S') must be listed
 *    after standard unprivileged extensions.  If multiple supervisor-level
 *    extensions are listed, they must be ordered alphabetically.
 *
 * 4. Standard machine-level extensions (starting with 'Zxm') must be listed
 *    after any lower-privileged, standard extensions.  If multiple
 *    machine-level extensions are listed, they must be ordered
 *    alphabetically.
 *
 * 5. Non-standard extensions (starting with 'X') must be listed after all
 *    standard extensions. If multiple non-standard extensions are listed, they
 *    must be ordered alphabetically.
 *
 * An example string following the order is:
 *    rv64imadc_zifoo_zigoo_zafoo_sbar_scar_zxmbaz_xqux_xrux
 *
 * New entries to this struct should follow the ordering rules described above.
 */
const struct riscv_isa_ext_data riscv_isa_ext[] = {
	__RISCV_ISA_EXT_DATA(i, RISCV_ISA_EXT_i),
	__RISCV_ISA_EXT_DATA(m, RISCV_ISA_EXT_m),
	__RISCV_ISA_EXT_DATA(a, RISCV_ISA_EXT_a),
	__RISCV_ISA_EXT_DATA(f, RISCV_ISA_EXT_f),
	__RISCV_ISA_EXT_DATA(d, RISCV_ISA_EXT_d),
	__RISCV_ISA_EXT_DATA(q, RISCV_ISA_EXT_q),
	__RISCV_ISA_EXT_DATA(c, RISCV_ISA_EXT_c),
	__RISCV_ISA_EXT_DATA(v, RISCV_ISA_EXT_v),
	__RISCV_ISA_EXT_DATA(h, RISCV_ISA_EXT_h),
	__RISCV_ISA_EXT_SUPERSET(zicbom, RISCV_ISA_EXT_ZICBOM, riscv_xlinuxenvcfg_exts),
	__RISCV_ISA_EXT_SUPERSET(zicboz, RISCV_ISA_EXT_ZICBOZ, riscv_xlinuxenvcfg_exts),
	__RISCV_ISA_EXT_DATA(zicntr, RISCV_ISA_EXT_ZICNTR),
	__RISCV_ISA_EXT_DATA(zicond, RISCV_ISA_EXT_ZICOND),
	__RISCV_ISA_EXT_DATA(zicsr, RISCV_ISA_EXT_ZICSR),
	__RISCV_ISA_EXT_DATA(zifencei, RISCV_ISA_EXT_ZIFENCEI),
	__RISCV_ISA_EXT_DATA(zihintntl, RISCV_ISA_EXT_ZIHINTNTL),
	__RISCV_ISA_EXT_DATA(zihintpause, RISCV_ISA_EXT_ZIHINTPAUSE),
	__RISCV_ISA_EXT_DATA(zihpm, RISCV_ISA_EXT_ZIHPM),
	__RISCV_ISA_EXT_DATA(zacas, RISCV_ISA_EXT_ZACAS),
	__RISCV_ISA_EXT_DATA(zfa, RISCV_ISA_EXT_ZFA),
	__RISCV_ISA_EXT_DATA(zfh, RISCV_ISA_EXT_ZFH),
	__RISCV_ISA_EXT_DATA(zfhmin, RISCV_ISA_EXT_ZFHMIN),
	__RISCV_ISA_EXT_DATA(zba, RISCV_ISA_EXT_ZBA),
	__RISCV_ISA_EXT_DATA(zbb, RISCV_ISA_EXT_ZBB),
	__RISCV_ISA_EXT_DATA(zbc, RISCV_ISA_EXT_ZBC),
	__RISCV_ISA_EXT_DATA(zbkb, RISCV_ISA_EXT_ZBKB),
	__RISCV_ISA_EXT_DATA(zbkc, RISCV_ISA_EXT_ZBKC),
	__RISCV_ISA_EXT_DATA(zbkx, RISCV_ISA_EXT_ZBKX),
	__RISCV_ISA_EXT_DATA(zbs, RISCV_ISA_EXT_ZBS),
	__RISCV_ISA_EXT_BUNDLE(zk, riscv_zk_bundled_exts),
	__RISCV_ISA_EXT_BUNDLE(zkn, riscv_zkn_bundled_exts),
	__RISCV_ISA_EXT_DATA(zknd, RISCV_ISA_EXT_ZKND),
	__RISCV_ISA_EXT_DATA(zkne, RISCV_ISA_EXT_ZKNE),
	__RISCV_ISA_EXT_DATA(zknh, RISCV_ISA_EXT_ZKNH),
	__RISCV_ISA_EXT_DATA(zkr, RISCV_ISA_EXT_ZKR),
	__RISCV_ISA_EXT_BUNDLE(zks, riscv_zks_bundled_exts),
	__RISCV_ISA_EXT_DATA(zkt, RISCV_ISA_EXT_ZKT),
	__RISCV_ISA_EXT_DATA(zksed, RISCV_ISA_EXT_ZKSED),
	__RISCV_ISA_EXT_DATA(zksh, RISCV_ISA_EXT_ZKSH),
	__RISCV_ISA_EXT_DATA(ztso, RISCV_ISA_EXT_ZTSO),
	__RISCV_ISA_EXT_SUPERSET(zvbb, RISCV_ISA_EXT_ZVBB, riscv_zvbb_exts),
	__RISCV_ISA_EXT_DATA(zvbc, RISCV_ISA_EXT_ZVBC),
	__RISCV_ISA_EXT_DATA(zvfh, RISCV_ISA_EXT_ZVFH),
	__RISCV_ISA_EXT_DATA(zvfhmin, RISCV_ISA_EXT_ZVFHMIN),
	__RISCV_ISA_EXT_DATA(zvkb, RISCV_ISA_EXT_ZVKB),
	__RISCV_ISA_EXT_DATA(zvkg, RISCV_ISA_EXT_ZVKG),
	__RISCV_ISA_EXT_BUNDLE(zvkn, riscv_zvkn_bundled_exts),
	__RISCV_ISA_EXT_BUNDLE(zvknc, riscv_zvknc_bundled_exts),
	__RISCV_ISA_EXT_DATA(zvkned, RISCV_ISA_EXT_ZVKNED),
	__RISCV_ISA_EXT_BUNDLE(zvkng, riscv_zvkng_bundled_exts),
	__RISCV_ISA_EXT_DATA(zvknha, RISCV_ISA_EXT_ZVKNHA),
	__RISCV_ISA_EXT_DATA(zvknhb, RISCV_ISA_EXT_ZVKNHB),
	__RISCV_ISA_EXT_BUNDLE(zvks, riscv_zvks_bundled_exts),
	__RISCV_ISA_EXT_BUNDLE(zvksc, riscv_zvksc_bundled_exts),
	__RISCV_ISA_EXT_DATA(zvksed, RISCV_ISA_EXT_ZVKSED),
	__RISCV_ISA_EXT_DATA(zvksh, RISCV_ISA_EXT_ZVKSH),
	__RISCV_ISA_EXT_BUNDLE(zvksg, riscv_zvksg_bundled_exts),
	__RISCV_ISA_EXT_DATA(zvkt, RISCV_ISA_EXT_ZVKT),
	__RISCV_ISA_EXT_DATA(smaia, RISCV_ISA_EXT_SMAIA),
	__RISCV_ISA_EXT_DATA(smstateen, RISCV_ISA_EXT_SMSTATEEN),
	__RISCV_ISA_EXT_DATA(ssaia, RISCV_ISA_EXT_SSAIA),
	__RISCV_ISA_EXT_DATA(sscofpmf, RISCV_ISA_EXT_SSCOFPMF),
	__RISCV_ISA_EXT_DATA(sstc, RISCV_ISA_EXT_SSTC),
	__RISCV_ISA_EXT_DATA(svinval, RISCV_ISA_EXT_SVINVAL),
	__RISCV_ISA_EXT_DATA(svnapot, RISCV_ISA_EXT_SVNAPOT),
	__RISCV_ISA_EXT_DATA(svpbmt, RISCV_ISA_EXT_SVPBMT),
	__RISCV_ISA_EXT_DATA(xandespmu, RISCV_ISA_EXT_XANDESPMU),
};

const size_t riscv_isa_ext_count = ARRAY_SIZE(riscv_isa_ext);

static void __init match_isa_ext(const struct riscv_isa_ext_data *ext, const char *name,
				 const char *name_end, struct riscv_isainfo *isainfo)
{
	if ((name_end - name == strlen(ext->name)) &&
	     !strncasecmp(name, ext->name, name_end - name)) {
		/*
		 * If this is a bundle, enable all the ISA extensions that
		 * comprise the bundle.
		 */
		if (ext->subset_ext_size) {
			for (int i = 0; i < ext->subset_ext_size; i++) {
				if (riscv_isa_extension_check(ext->subset_ext_ids[i]))
					set_bit(ext->subset_ext_ids[i], isainfo->isa);
			}
		}

		/*
		 * This is valid even for bundle extensions which uses the RISCV_ISA_EXT_INVALID id
		 * (rejected by riscv_isa_extension_check()).
		 */
		if (riscv_isa_extension_check(ext->id))
			set_bit(ext->id, isainfo->isa);
	}
}

static void __init riscv_parse_isa_string(unsigned long *this_hwcap, struct riscv_isainfo *isainfo,
					  unsigned long *isa2hwcap, const char *isa)
{
	/*
	 * For all possible cpus, we have already validated in
	 * the boot process that they at least contain "rv" and
	 * whichever of "32"/"64" this kernel supports, and so this
	 * section can be skipped.
	 */
	isa += 4;

	while (*isa) {
		const char *ext = isa++;
		const char *ext_end = isa;
		bool ext_long = false, ext_err = false;

		switch (*ext) {
		case 's':
			/*
			 * Workaround for invalid single-letter 's' & 'u' (QEMU).
			 * No need to set the bit in riscv_isa as 's' & 'u' are
			 * not valid ISA extensions. It works unless the first
			 * multi-letter extension in the ISA string begins with
			 * "Su" and is not prefixed with an underscore.
			 */
			if (ext[-1] != '_' && ext[1] == 'u') {
				++isa;
				ext_err = true;
				break;
			}
			fallthrough;
		case 'S':
		case 'x':
		case 'X':
		case 'z':
		case 'Z':
			/*
			 * Before attempting to parse the extension itself, we find its end.
			 * As multi-letter extensions must be split from other multi-letter
			 * extensions with an "_", the end of a multi-letter extension will
			 * either be the null character or the "_" at the start of the next
			 * multi-letter extension.
			 *
			 * Next, as the extensions version is currently ignored, we
			 * eliminate that portion. This is done by parsing backwards from
			 * the end of the extension, removing any numbers. This may be a
			 * major or minor number however, so the process is repeated if a
			 * minor number was found.
			 *
			 * ext_end is intended to represent the first character *after* the
			 * name portion of an extension, but will be decremented to the last
			 * character itself while eliminating the extensions version number.
			 * A simple re-increment solves this problem.
			 */
			ext_long = true;
			for (; *isa && *isa != '_'; ++isa)
				if (unlikely(!isalnum(*isa)))
					ext_err = true;

			ext_end = isa;
			if (unlikely(ext_err))
				break;

			if (!isdigit(ext_end[-1]))
				break;

			while (isdigit(*--ext_end))
				;

			if (tolower(ext_end[0]) != 'p' || !isdigit(ext_end[-1])) {
				++ext_end;
				break;
			}

			while (isdigit(*--ext_end))
				;

			++ext_end;
			break;
		default:
			/*
			 * Things are a little easier for single-letter extensions, as they
			 * are parsed forwards.
			 *
			 * After checking that our starting position is valid, we need to
			 * ensure that, when isa was incremented at the start of the loop,
			 * that it arrived at the start of the next extension.
			 *
			 * If we are already on a non-digit, there is nothing to do. Either
			 * we have a multi-letter extension's _, or the start of an
			 * extension.
			 *
			 * Otherwise we have found the current extension's major version
			 * number. Parse past it, and a subsequent p/minor version number
			 * if present. The `p` extension must not appear immediately after
			 * a number, so there is no fear of missing it.
			 *
			 */
			if (unlikely(!isalpha(*ext))) {
				ext_err = true;
				break;
			}

			if (!isdigit(*isa))
				break;

			while (isdigit(*++isa))
				;

			if (tolower(*isa) != 'p')
				break;

			if (!isdigit(*++isa)) {
				--isa;
				break;
			}

			while (isdigit(*++isa))
				;

			break;
		}

		/*
		 * The parser expects that at the start of an iteration isa points to the
		 * first character of the next extension. As we stop parsing an extension
		 * on meeting a non-alphanumeric character, an extra increment is needed
		 * where the succeeding extension is a multi-letter prefixed with an "_".
		 */
		if (*isa == '_')
			++isa;

		if (unlikely(ext_err))
			continue;
		if (!ext_long) {
			int nr = tolower(*ext) - 'a';

			if (riscv_isa_extension_check(nr)) {
				*this_hwcap |= isa2hwcap[nr];
				set_bit(nr, isainfo->isa);
			}
		} else {
			for (int i = 0; i < riscv_isa_ext_count; i++)
				match_isa_ext(&riscv_isa_ext[i], ext, ext_end, isainfo);
		}
	}
}
/*
 * 从系统的 ISA 字符串（通过 ACPI 或设备树）获取 RISC-V 硬件扩展的信息，
 * 并更新系统的硬件能力位图和每个 CPU 的 ISA 信息。 
 */
static void __init riscv_fill_hwcap_from_isa_string(unsigned long *isa2hwcap)
{
	struct device_node *node;//声明设备节点指针
	const char *isa;// 用于存储 ISA 字符串的指针
	int rc;
	struct acpi_table_header *rhct;//ACPI 表头指针
	acpi_status status;//ACPI 状态
	unsigned int cpu;// CPU 索引
	u64 boot_vendorid;//引导时获取的供应商 ID
	u64 boot_archid;//引导时获取的架构 ID

	if (!acpi_disabled) {// 检查 ACPI 是否被禁用
		status = acpi_get_table(ACPI_SIG_RHCT, 0, &rhct);//获取 ACPI RHCT 表
		if (ACPI_FAILURE(status))//如果获取失败，退出
			return;
	}

	boot_vendorid = riscv_get_mvendorid();// 获取当前 CPU 的供应商 ID
	boot_archid = riscv_get_marchid();// 获取当前 CPU 的架构 ID

	for_each_possible_cpu(cpu) {//遍历所有可能的 CPU
		struct riscv_isainfo *isainfo = &hart_isa[cpu];// 获取当前 CPU 的 ISA 信息结构
		unsigned long this_hwcap = 0;//用于存储当前 CPU 的硬件能力

		if (acpi_disabled) {//如果 ACPI 被禁用，使用设备树（Device Tree）
			node = of_cpu_device_node_get(cpu);// 获取当前 CPU 的设备树节点
			if (!node) {//如果节点不存在，打印警告并继续
				pr_warn("Unable to find cpu node\n");
				continue;
			}

			rc = of_property_read_string(node, "riscv,isa", &isa);//读取设备树中的 ISA 字符串
			of_node_put(node);//释放设备树节点
			if (rc) {//如果读取失败，打印警告并继续
				pr_warn("Unable to find \"riscv,isa\" devicetree entry\n");
				continue;
			}
		} else {//如果 ACPI 没有被禁用
			rc = acpi_get_riscv_isa(rhct, cpu, &isa);//从 ACPI 获取 ISA 字符串
			if (rc < 0) {//如果获取失败，打印警告并继续
				pr_warn("Unable to get ISA for the hart - %d\n", cpu);
				continue;
			}
		}

		riscv_parse_isa_string(&this_hwcap, isainfo, isa2hwcap, isa);//解析 ISA 字符串，并填充硬件能力和 ISA 信息

		/*
		 * These ones were as they were part of the base ISA when the
		 * port & dt-bindings were upstreamed, and so can be set
		 * unconditionally where `i` is in riscv,isa on DT systems.
		 */
		if (acpi_disabled) {//如果使用设备树，设置一些默认的 ISA 扩展位
			set_bit(RISCV_ISA_EXT_ZICSR, isainfo->isa);//设置 ZICSR 扩展位
			set_bit(RISCV_ISA_EXT_ZIFENCEI, isainfo->isa);//设置 ZIFENCEI 扩展位
			set_bit(RISCV_ISA_EXT_ZICNTR, isainfo->isa);//设置 ZICNTR 扩展位
			set_bit(RISCV_ISA_EXT_ZIHPM, isainfo->isa);//设置 ZIHPM 扩展位
		}

		/*
		 * "V" in ISA strings is ambiguous in practice: it should mean
		 * just the standard V-1.0 but vendors aren't well behaved.
		 * Many vendors with T-Head CPU cores which implement the 0.7.1
		 * version of the vector specification put "v" into their DTs.
		 * CPU cores with the ratified spec will contain non-zero
		 * marchid.
		 * 处理 V 扩展的特殊情况，针对一些供应商的行为(主要是芯来的CPU)
		 */
		if (acpi_disabled && boot_vendorid == THEAD_VENDOR_ID && boot_archid == 0x0) {
			this_hwcap &= ~isa2hwcap[RISCV_ISA_EXT_v];//清除 V 扩展的硬件能力
			clear_bit(RISCV_ISA_EXT_v, isainfo->isa);//清除 V 扩展的 ISA 位
		}

		/*
		 * All "okay" hart should have same isa. Set HWCAP based on
		 * common capabilities of every "okay" hart, in case they don't
		 * have.
		 */
		if (elf_hwcap)//将硬件能力设为所有 CPU 的交集，确保每个 CPU 都有相同的 ISA 能力
			elf_hwcap &= this_hwcap;//取交集
		else
			elf_hwcap = this_hwcap;//初始设定硬件能力

		if (bitmap_empty(riscv_isa, RISCV_ISA_EXT_MAX))// 更新全局 riscv_isa 位图，确保所有 harts（硬件线程）的一致性
			bitmap_copy(riscv_isa, isainfo->isa, RISCV_ISA_EXT_MAX);//复制当前hart的ISA位图
		else
			bitmap_and(riscv_isa, riscv_isa, isainfo->isa, RISCV_ISA_EXT_MAX);//取交集以保持一致性
	}

	if (!acpi_disabled && rhct)//如果 ACPI 没有禁用且获取到 RHCT 表
		acpi_put_table((struct acpi_table_header *)rhct);//释放 ACPI 表
}

static int __init riscv_fill_hwcap_from_ext_list(unsigned long *isa2hwcap)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {//遍历系统上存在的每一个CPU
		unsigned long this_hwcap = 0;//表示本次迭代CPU的硬件能力
		struct device_node *cpu_node;//定义一个指向设备节点的指针，用于获取CPU的设备节点
		struct riscv_isainfo *isainfo = &hart_isa[cpu];//获取当前 CPU 的 RISC-V ISA 信息

		cpu_node = of_cpu_device_node_get(cpu);//获取与当前 CPU 相关联的设备树节点
		if (!cpu_node) {//如果无法获取设备树节点
			pr_warn("Unable to find cpu node\n");//打印警告信息
			continue;//跳过当前 CPU，继续处理下一个 CPU
		}

		if (!of_property_present(cpu_node, "riscv,isa-extensions")) {//检查设备树节点中是否存在 "riscv,isa-extensions" 属性
			of_node_put(cpu_node);// 如果没有，则释放设备节点
			continue;//跳过当前 CPU
		}

		for (int i = 0; i < riscv_isa_ext_count; i++) {// 遍历已知的 RISC-V ISA 扩展列表
			const struct riscv_isa_ext_data *ext = &riscv_isa_ext[i];//获取当前扩展的指针

			if (of_property_match_string(cpu_node, "riscv,isa-extensions",
						     ext->property) < 0)//检查当前 CPU 的设备节点是否包含该扩展的字符串属性
				continue;//如果没有匹配的字符串属性，跳过该扩展

			if (ext->subset_ext_size) {// 如果该扩展有子集扩展，进一步处理这些子集扩展
				for (int j = 0; j < ext->subset_ext_size; j++) {//遍历子集扩展
					if (riscv_isa_extension_check(ext->subset_ext_ids[j]))// 检查子集扩展是否支持，并将其标记在 ISA 信息中
						set_bit(ext->subset_ext_ids[j], isainfo->isa);
				}
			}

			if (riscv_isa_extension_check(ext->id)) {//检查该扩展是否被支持，并将其标记在 ISA 信息中
				set_bit(ext->id, isainfo->isa);//设置扩展位

				/* Only single letter extensions get set in hwcap */
				if (strnlen(riscv_isa_ext[i].name, 2) == 1)// 如果扩展名称为单字母，则将其添加到硬件能力变量中
					this_hwcap |= isa2hwcap[riscv_isa_ext[i].id];
			}
		}

		of_node_put(cpu_node);// 处理完当前 CPU 的设备节点后，释放节点

		/*
		 * All "okay" harts should have same isa. Set HWCAP based on
		 * common capabilities of every "okay" hart, in case they don't.
		 * 所有状态为 "okay" 的处理器核应该具有相同的 ISA。设定 HWCAP 为所有 "okay" 核心的公共能力集。
		 */
		if (elf_hwcap)
			elf_hwcap &= this_hwcap;//与前面的 HWCAP 取交集，确保所有处理器核支持相同的功能
		else
			elf_hwcap = this_hwcap;// 如果 elf_hwcap 尚未设置，直接赋值

		if (bitmap_empty(riscv_isa, RISCV_ISA_EXT_MAX))//如果全局 ISA 位图为空，则复制当前处理器的 ISA 位图
			bitmap_copy(riscv_isa, isainfo->isa, RISCV_ISA_EXT_MAX);
		else
			bitmap_and(riscv_isa, riscv_isa, isainfo->isa, RISCV_ISA_EXT_MAX);//否则取交集，以确保全局 ISA 位图只包含所有处理器都支持的扩展
	}

	if (bitmap_empty(riscv_isa, RISCV_ISA_EXT_MAX))//如果全局 ISA 位图仍为空，表示没有找到支持的扩展，返回错误
		return -ENOENT;

	return 0;//成功返回0
}

#ifdef CONFIG_RISCV_ISA_FALLBACK
bool __initdata riscv_isa_fallback = true;
#else
bool __initdata riscv_isa_fallback;
static int __init riscv_isa_fallback_setup(char *__unused)
{
	riscv_isa_fallback = true;
	return 1;
}
early_param("riscv_isa_fallback", riscv_isa_fallback_setup);
#endif

void __init riscv_fill_hwcap(void)
{
	char print_str[NUM_ALPHA_EXTS + 1];//定义一个字符数组，用于存储要打印的 ISA 扩展字符串
	unsigned long isa2hwcap[26] = {0};//定义一个数组，用于将ISA扩展字母映射到硬件能力标志
	int i, j;
	/* 将 ISA 字母映射到对应的硬件能力标志 */
	isa2hwcap['i' - 'a'] = COMPAT_HWCAP_ISA_I;//映射 'i' 扩展
	isa2hwcap['m' - 'a'] = COMPAT_HWCAP_ISA_M;//映射 'm' 扩展
	isa2hwcap['a' - 'a'] = COMPAT_HWCAP_ISA_A;//映射 'a' 扩展
	isa2hwcap['f' - 'a'] = COMPAT_HWCAP_ISA_F;//映射 'f' 扩展
	isa2hwcap['d' - 'a'] = COMPAT_HWCAP_ISA_D;//映射 'd' 扩展
	isa2hwcap['c' - 'a'] = COMPAT_HWCAP_ISA_C;//映射 'c' 扩展
	isa2hwcap['v' - 'a'] = COMPAT_HWCAP_ISA_V;//映射 'v' 扩展

	if (!acpi_disabled) {//检查是否禁用了 ACPI
		riscv_fill_hwcap_from_isa_string(isa2hwcap);// 如果没有禁用 ACPI，则从 ISA 字符串中填充硬件能力
	} else {
		int ret = riscv_fill_hwcap_from_ext_list(isa2hwcap);//如果禁用了 ACPI，则从扩展列表中填充硬件能力

		if (ret && riscv_isa_fallback) {//如果填充失败并且启用了ISA回退机制
			pr_info("Falling back to deprecated \"riscv,isa\"\n");//打印信息，提示回退到过时的 "riscv,isa" 方法。
			riscv_fill_hwcap_from_isa_string(isa2hwcap);//回退到从 ISA 字符串中填充硬件能力
		}
	}

	/*
	 *  我们不支持仅有 F 而没有 D 扩展的系统，因此在此处屏蔽掉 F 扩展
	 */
	if ((elf_hwcap & COMPAT_HWCAP_ISA_F) && !(elf_hwcap & COMPAT_HWCAP_ISA_D)) {//如果检测到 F 扩展但没有 D 扩展，打印不支持的警告信息并移除 F 扩展标志
		pr_info("This kernel does not support systems with F but not D\n");
		elf_hwcap &= ~COMPAT_HWCAP_ISA_F;
	}

	if (elf_hwcap & COMPAT_HWCAP_ISA_V) {//检查是否检测到 V 扩展
		riscv_v_setup_vsize();//如果存在 V 扩展，则设置 V 扩展的相关参数
		/*
		 *  设备树中的 ISA 字符串可能包含 'v' 标志,但是如果内核未启用 
		 *  CONFIG_RISCV_ISA_V，则清除 ELF 中的 V 扩展标志。
		 */
		if (!IS_ENABLED(CONFIG_RISCV_ISA_V))
			elf_hwcap &= ~COMPAT_HWCAP_ISA_V;
	}

	memset(print_str, 0, sizeof(print_str));//初始化 print_str 字符数组为空字符串
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)//遍历所有可能的字母扩展，将存在的扩展添加到打印字符串中
		if (riscv_isa[0] & BIT_MASK(i))// 检查 riscv_isa[0] 中对应位是否被设置，即是否支持该扩展
			print_str[j++] = (char)('a' + i);// 将存在的扩展字母加入字符串
	pr_info("riscv: base ISA extensions %s\n", print_str);//打印出基础 ISA 扩展列表

	memset(print_str, 0, sizeof(print_str));// 再次初始化 print_str 字符数组为空字符串
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)//遍历硬件能力标志，添加存在的扩展到打印字符串中
		if (elf_hwcap & BIT_MASK(i))// 检查 elf_hwcap 中对应位是否被设置，即是否支持该硬件能力
			print_str[j++] = (char)('a' + i);//将存在的扩展字母加入字符串
	pr_info("riscv: ELF capabilities %s\n", print_str);//打印出 ELF 中的硬件能力列表
}

unsigned long riscv_get_elf_hwcap(void)
{
	unsigned long hwcap;

	hwcap = (elf_hwcap & ((1UL << RISCV_ISA_EXT_BASE) - 1));

	if (!riscv_v_vstate_ctrl_user_allowed())
		hwcap &= ~COMPAT_HWCAP_ISA_V;

	return hwcap;
}

void riscv_user_isa_enable(void)
{
	if (riscv_cpu_has_extension_unlikely(smp_processor_id(), RISCV_ISA_EXT_ZICBOZ))//检查当前CPU是否支持ZICBOZ 扩展
		csr_set(CSR_ENVCFG, ENVCFG_CBZE);//如果支持，则设置CSR_ENVCFG寄存器以启用相关功能
}

#ifdef CONFIG_RISCV_ALTERNATIVE
/*
 * Alternative patch sites consider 48 bits when determining when to patch
 * the old instruction sequence with the new. These bits are broken into a
 * 16-bit vendor ID and a 32-bit patch ID. A non-zero vendor ID means the
 * patch site is for an erratum, identified by the 32-bit patch ID. When
 * the vendor ID is zero, the patch site is for a cpufeature. cpufeatures
 * further break down patch ID into two 16-bit numbers. The lower 16 bits
 * are the cpufeature ID and the upper 16 bits are used for a value specific
 * to the cpufeature and patch site. If the upper 16 bits are zero, then it
 * implies no specific value is specified. cpufeatures that want to control
 * patching on a per-site basis will provide non-zero values and implement
 * checks here. The checks return true when patching should be done, and
 * false otherwise.
 */
static bool riscv_cpufeature_patch_check(u16 id, u16 value)
{
	if (!value)
		return true;

	switch (id) {
	case RISCV_ISA_EXT_ZICBOZ:
		/*
		 * Zicboz alternative applications provide the maximum
		 * supported block size order, or zero when it doesn't
		 * matter. If the current block size exceeds the maximum,
		 * then the alternative cannot be applied.
		 */
		return riscv_cboz_block_size <= (1U << value);
	}

	return false;
}

void __init_or_module riscv_cpufeature_patch_func(struct alt_entry *begin,
						  struct alt_entry *end,
						  unsigned int stage)
{
	struct alt_entry *alt;
	void *oldptr, *altptr;
	u16 id, value;

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		return;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != 0)
			continue;

		id = PATCH_ID_CPUFEATURE_ID(alt->patch_id);

		if (id >= RISCV_ISA_EXT_MAX) {
			WARN(1, "This extension id:%d is not in ISA extension list", id);
			continue;
		}

		if (!__riscv_isa_extension_available(NULL, id))
			continue;

		value = PATCH_ID_CPUFEATURE_VALUE(alt->patch_id);
		if (!riscv_cpufeature_patch_check(id, value))
			continue;

		oldptr = ALT_OLD_PTR(alt);
		altptr = ALT_ALT_PTR(alt);

		mutex_lock(&text_mutex);
		patch_text_nosync(oldptr, altptr, alt->alt_len);
		riscv_alternative_fix_offsets(oldptr, alt->alt_len, oldptr - altptr);
		mutex_unlock(&text_mutex);
	}
}
#endif
