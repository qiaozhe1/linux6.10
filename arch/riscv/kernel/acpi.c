// SPDX-License-Identifier: GPL-2.0-only
/*
 *  RISC-V Specific Low-Level ACPI Boot Support
 *
 *  Copyright (C) 2013-2014, Linaro Ltd.
 *	Author: Al Stone <al.stone@linaro.org>
 *	Author: Graeme Gregory <graeme.gregory@linaro.org>
 *	Author: Hanjun Guo <hanjun.guo@linaro.org>
 *	Author: Tomasz Nowicki <tomasz.nowicki@linaro.org>
 *	Author: Naresh Bhat <naresh.bhat@linaro.org>
 *
 *  Copyright (C) 2021-2023, Ventana Micro Systems Inc.
 *	Author: Sunil V L <sunilvl@ventanamicro.com>
 */

#include <linux/acpi.h>
#include <linux/efi.h>
#include <linux/io.h>
#include <linux/memblock.h>
#include <linux/pci.h>

int acpi_noirq = 1;		/* skip ACPI IRQ initialization */
int acpi_disabled = 1;
EXPORT_SYMBOL(acpi_disabled);

int acpi_pci_disabled = 1;	/* skip ACPI PCI scan and IRQ initialization */
EXPORT_SYMBOL(acpi_pci_disabled);

static bool param_acpi_off __initdata;
static bool param_acpi_on __initdata;
static bool param_acpi_force __initdata;

static struct acpi_madt_rintc cpu_madt_rintc[NR_CPUS];

static int __init parse_acpi(char *arg)
{
	if (!arg)
		return -EINVAL;

	/* "acpi=off" disables both ACPI table parsing and interpreter */
	if (strcmp(arg, "off") == 0)
		param_acpi_off = true;
	else if (strcmp(arg, "on") == 0) /* prefer ACPI over DT */
		param_acpi_on = true;
	else if (strcmp(arg, "force") == 0) /* force ACPI to be enabled */
		param_acpi_force = true;
	else
		return -EINVAL;	/* Core will print when we return error */

	return 0;
}
early_param("acpi", parse_acpi);

/*
 * acpi_fadt_sanity_check() - Check FADT presence and carry out sanity
 *			      checks on it
 *
 * Return 0 on success,  <0 on failure
 * 用于检查 FADT 表的有效性和合规性。
 */
static int __init acpi_fadt_sanity_check(void)
{
	struct acpi_table_header *table;//定义一个指向 ACPI 表头的指针，用于存储 FADT 表的指针。
	struct acpi_table_fadt *fadt;//定义一个指向 FADT（固件 ACPI 描述表）的指针。
	acpi_status status;//定义变量 `status` 来存储 ACPI 函数的返回状态。
	int ret = 0;//定义并初始化返回值为 0，表示默认情况下没有错误。

	/*
	 * FADT is required on riscv; retrieve it to check its presence
	 * and carry out revision and ACPI HW reduced compliancy tests
	 * FADT 是 RISC-V 上必须存在的 ACPI 表；检索它以检查其存在性，
	 * 并进行修订版本检查和 ACPI 硬件减少兼容性测试。
	 */
	status = acpi_get_table(ACPI_SIG_FADT, 0, &table);//通过 ACPI 接口获取 FADT 表。
	if (ACPI_FAILURE(status)) {// 如果获取表失败
		const char *msg = acpi_format_exception(status);//获取错误信息字符串。

		pr_err("Failed to get FADT table, %s\n", msg);//打印错误信息。
		return -ENODEV;
	}

	fadt = (struct acpi_table_fadt *)table;//将获取的表头转换为 FADT 表的结构体指针

	/*
	 * The revision in the table header is the FADT's Major revision. The
	 * FADT also has a minor revision, which is stored in the FADT itself.
	 *
	 * TODO: Currently, we check for 6.5 as the minimum version to check
	 * for HW_REDUCED flag. However, once RISC-V updates are released in
	 * the ACPI spec, we need to update this check for exact minor revision
	 */
	if (table->revision < 6 || (table->revision == 6 && fadt->minor_revision < 5))//检查 FADT 版本是否小于 6.5
		pr_err(FW_BUG "Unsupported FADT revision %d.%d, should be 6.5+\n",
		       table->revision, fadt->minor_revision);// 如果不支持的版本，打印错误信息。

	if (!(fadt->flags & ACPI_FADT_HW_REDUCED)) {//检查 FADT 表中的标志，判断是否支持硬件减少
		pr_err("FADT not ACPI hardware reduced compliant\n");// 如果不支持，打印错误信息
		ret = -EINVAL;
	}

	/*
	 * acpi_get_table() creates FADT table mapping that
	 * should be released after parsing and before resuming boot
	 */
	acpi_put_table(table);// 释放 FADT 表的内存映射。
	return ret;//返回检查结果，0 表示成功，非 0 表示错误。
}

/*
 * acpi_boot_table_init() called from setup_arch(), always.
 *	1. find RSDP and get its address, and then find XSDT
 *	2. extract all tables and checksums them all
 *	3. check ACPI FADT HW reduced flag
 *
 * We can parse ACPI boot-time tables such as MADT after
 * this function is called.
 *
 * On return ACPI is enabled if either:
 *
 * - ACPI tables are initialized and sanity checks passed
 * - acpi=force was passed in the command line and ACPI was not disabled
 *   explicitly through acpi=off command line parameter
 *
 * ACPI is disabled on function return otherwise
 *
 * 用于在系统引导过程中决定是否启用 ACPI（高级配置与电源接口），并
 * 对 ACPI 表进行初始化和合理性检查。ACPI 是一种开放标准，用于在操
 * 作系统和硬件之间传递信息，并为电源管理和硬件配置提供接口。
 */
void __init acpi_boot_table_init(void)
{
	/*
	 * Enable ACPI instead of device tree unless
	 * - ACPI has been disabled explicitly (acpi=off), or
	 * - firmware has not populated ACPI ptr in EFI system table
	 *   and ACPI has not been [force] enabled (acpi=on|force)
	 */
	if (param_acpi_off ||//如果ACPI 被显式禁用（通过命令行参数 acpi=off）
	    (!param_acpi_on && !param_acpi_force &&//或者固件未在 EFI 系统表中填充 ACPI 指针，并且没有强制启用 ACPI
	     efi.acpi20 == EFI_INVALID_TABLE_ADDR))
		return;//如果满足上述条件之一，则直接返回，不初始化 ACPI。

	/*
	 * ACPI is disabled at this point. Enable it in order to parse
	 * the ACPI tables and carry out sanity checks
	 */
	enable_acpi();//启用ACPI

	/*
	 * If ACPI tables are initialized and FADT sanity checks passed,
	 * leave ACPI enabled and carry on booting; otherwise disable ACPI
	 * on initialization error.
	 * If acpi=force was passed on the command line it forces ACPI
	 * to be enabled even if its initialization failed.
	 */
	if (acpi_table_init() || acpi_fadt_sanity_check()) {//如果 ACPI 表初始化成功并且 FADT（固定 ACPI 描述表）合理性检查通过
		pr_err("Failed to init ACPI tables\n");//如果初始化失败，打印错误信息。
		if (!param_acpi_force)//如果未强制启用 ACPI，则禁用 ACPI
			disable_acpi();
	}
}

static int acpi_parse_madt_rintc(union acpi_subtable_headers *header, const unsigned long end)
{
	struct acpi_madt_rintc *rintc = (struct acpi_madt_rintc *)header;
	int cpuid;

	if (!(rintc->flags & ACPI_MADT_ENABLED))
		return 0;

	cpuid = riscv_hartid_to_cpuid(rintc->hart_id);
	/*
	 * When CONFIG_SMP is disabled, mapping won't be created for
	 * all cpus.
	 * CPUs more than num_possible_cpus, will be ignored.
	 */
	if (cpuid >= 0 && cpuid < num_possible_cpus())
		cpu_madt_rintc[cpuid] = *rintc;

	return 0;
}

/*
 * Instead of parsing (and freeing) the ACPI table, cache
 * the RINTC structures since they are frequently used
 * like in  cpuinfo.
 * 用于初始化 RINTC (RISC-V Interrupt Controller) 映射。
 */
void __init acpi_init_rintc_map(void)
{
	if (acpi_table_parse_madt(ACPI_MADT_TYPE_RINTC, acpi_parse_madt_rintc, 0) <= 0) {//如果从 ACPI MADT 表中解析 RINTC (RISC-V Interrupt Controller) 条目失败或没有找到有效条目，则执行以下逻辑。
		pr_err("No valid RINTC entries exist\n");//打印错误信息，表示没有找到有效的 RINTC 条目。
		BUG();
	}
}

struct acpi_madt_rintc *acpi_cpu_get_madt_rintc(int cpu)
{
	return &cpu_madt_rintc[cpu];
}

u32 get_acpi_id_for_cpu(int cpu)
{
	return acpi_cpu_get_madt_rintc(cpu)->uid;
}

/*
 * __acpi_map_table() will be called before paging_init(), so early_ioremap()
 * or early_memremap() should be called here to for ACPI table mapping.
 */
void __init __iomem *__acpi_map_table(unsigned long phys, unsigned long size)
{
	if (!size)
		return NULL;

	return early_ioremap(phys, size);
}

void __init __acpi_unmap_table(void __iomem *map, unsigned long size)
{
	if (!map || !size)
		return;

	early_iounmap(map, size);
}

void __iomem *acpi_os_ioremap(acpi_physical_address phys, acpi_size size)
{
	efi_memory_desc_t *md, *region = NULL;
	pgprot_t prot;

	if (WARN_ON_ONCE(!efi_enabled(EFI_MEMMAP)))
		return NULL;

	for_each_efi_memory_desc(md) {
		u64 end = md->phys_addr + (md->num_pages << EFI_PAGE_SHIFT);

		if (phys < md->phys_addr || phys >= end)
			continue;

		if (phys + size > end) {
			pr_warn(FW_BUG "requested region covers multiple EFI memory regions\n");
			return NULL;
		}
		region = md;
		break;
	}

	/*
	 * It is fine for AML to remap regions that are not represented in the
	 * EFI memory map at all, as it only describes normal memory, and MMIO
	 * regions that require a virtual mapping to make them accessible to
	 * the EFI runtime services.
	 */
	prot = PAGE_KERNEL_IO;
	if (region) {
		switch (region->type) {
		case EFI_LOADER_CODE:
		case EFI_LOADER_DATA:
		case EFI_BOOT_SERVICES_CODE:
		case EFI_BOOT_SERVICES_DATA:
		case EFI_CONVENTIONAL_MEMORY:
		case EFI_PERSISTENT_MEMORY:
			if (memblock_is_map_memory(phys) ||
			    !memblock_is_region_memory(phys, size)) {
				pr_warn(FW_BUG "requested region covers kernel memory\n");
				return NULL;
			}

			/*
			 * Mapping kernel memory is permitted if the region in
			 * question is covered by a single memblock with the
			 * NOMAP attribute set: this enables the use of ACPI
			 * table overrides passed via initramfs.
			 * This particular use case only requires read access.
			 */
			fallthrough;

		case EFI_RUNTIME_SERVICES_CODE:
			/*
			 * This would be unusual, but not problematic per se,
			 * as long as we take care not to create a writable
			 * mapping for executable code.
			 */
			prot = PAGE_KERNEL_RO;
			break;

		case EFI_ACPI_RECLAIM_MEMORY:
			/*
			 * ACPI reclaim memory is used to pass firmware tables
			 * and other data that is intended for consumption by
			 * the OS only, which may decide it wants to reclaim
			 * that memory and use it for something else. We never
			 * do that, but we usually add it to the linear map
			 * anyway, in which case we should use the existing
			 * mapping.
			 */
			if (memblock_is_map_memory(phys))
				return (void __iomem *)__va(phys);
			fallthrough;

		default:
			if (region->attribute & EFI_MEMORY_WB)
				prot = PAGE_KERNEL;
			else if ((region->attribute & EFI_MEMORY_WC) ||
				 (region->attribute & EFI_MEMORY_WT))
				prot = pgprot_writecombine(PAGE_KERNEL);
		}
	}

	return ioremap_prot(phys, size, pgprot_val(prot));
}

#ifdef CONFIG_PCI

/*
 * These interfaces are defined just to enable building ACPI core.
 * TODO: Update it with actual implementation when external interrupt
 * controller support is added in RISC-V ACPI.
 */
int raw_pci_read(unsigned int domain, unsigned int bus, unsigned int devfn,
		 int reg, int len, u32 *val)
{
	return PCIBIOS_DEVICE_NOT_FOUND;
}

int raw_pci_write(unsigned int domain, unsigned int bus, unsigned int devfn,
		  int reg, int len, u32 val)
{
	return PCIBIOS_DEVICE_NOT_FOUND;
}

int acpi_pci_bus_find_domain_nr(struct pci_bus *bus)
{
	return -1;
}

struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	return NULL;
}
#endif	/* CONFIG_PCI */
