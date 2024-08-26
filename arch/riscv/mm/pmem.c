// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Ventana Micro Systems Inc.
 */

#include <linux/export.h>
#include <linux/libnvdimm.h>

#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>

void arch_wb_cache_pmem(void *addr, size_t size)//将持久内存（PMEM）的缓存写回到内存中，确保数据从缓存同步到内存中
{
#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.wback)) {//如果 noncoherent_cache_ops.wback 不为空，说明存在自定义的写回操作函数。
		noncoherent_cache_ops.wback(virt_to_phys(addr), size);//调用自定义的写回操作，将指定的内存区域从虚拟地址转换为物理地址后执行写回。
		return;
	}
#endif
	ALT_CMO_OP(CLEAN, addr, size, riscv_cbom_block_size);//如果未定义非标准操作或未启用自定义写回操作，则使用标准的缓存维护操作。CLEAN表示清理操作(将缓存中的数据写回到内存)。riscv_cbom_block_size 是与 RISC-V 缓存块大小相关的参数，用于确定一次操作的处理块大小。
}
EXPORT_SYMBOL_GPL(arch_wb_cache_pmem);

void arch_invalidate_pmem(void *addr, size_t size)//用于无效化持久内存（PMEM）的缓存数据，确保缓存中的数据不再有效
{
#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.inv)) {//如果 noncoherent_cache_ops.inv 不为空，说明存在自定义的无效化操作函数。
		noncoherent_cache_ops.inv(virt_to_phys(addr), size);
		return;
	}
#endif
	ALT_CMO_OP(INVAL, addr, size, riscv_cbom_block_size);//如果未定义非标准操作或未启用自定义无效化操作，则使用标准的缓存维护操作。INVAL 表示无效化操作（将缓存中的数据标记为无效）。
}
EXPORT_SYMBOL_GPL(arch_invalidate_pmem);
