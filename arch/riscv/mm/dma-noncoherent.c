// SPDX-License-Identifier: GPL-2.0-only
/*
 * RISC-V specific functions to support DMA for non-coherent devices
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 */

#include <linux/dma-direct.h>
#include <linux/dma-map-ops.h>
#include <linux/mm.h>
#include <asm/cacheflush.h>
#include <asm/dma-noncoherent.h>

static bool noncoherent_supported __ro_after_init;
int dma_cache_alignment __ro_after_init = ARCH_DMA_MINALIGN;
EXPORT_SYMBOL_GPL(dma_cache_alignment);
/*
 * 用于将缓存中的数据写回到物理内存。
 * 参数 paddr：物理地址.
 * 参数 size：数据大小。
 * */
static inline void arch_dma_cache_wback(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);//将物理地址 paddr 转换为虚拟地址 vaddr

#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.wback)) {//检查 noncoherent_cache_ops 结构中的 wback 函数指针是否存在
		noncoherent_cache_ops.wback(paddr, size);//指针存在，则调用该函数进行非标准的缓存写回操作
		return;
	}
#endif
	/*
	 * 使用替代的缓存管理操作进行缓存清理。
	 * ALT_CMO_OP：用于执行特定的缓存管理操作。
	 * CLEAN 表示执行缓存清理操作，将缓存中的数据写回到内存。
	 * vaddr 是虚拟地址
	 * size 是数据大小。
	 * riscv_cbom_block_size 是缓存块大小。
	 * */
	ALT_CMO_OP(CLEAN, vaddr, size, riscv_cbom_block_size);
}
/*
 * 用于将缓存中的数据无效化
 * */
static inline void arch_dma_cache_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);//物理地址 paddr 转换为虚拟地址 vaddr

#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.inv)) {//检查 noncoherent_cache_ops 结构中的 inv 函数指针是否存在。
		noncoherent_cache_ops.inv(paddr, size);//指针存在，则调用该函数进行非标准的缓存无效化操作
		return;
	}
#endif
	/*
	 * 用替代的缓存管理操作进行缓存无效化
	 * ALT_CMO_OP 是一个宏或函数，用于执行特定的缓存管理操作。
	 * INVAL 表示执行缓存无效化操作。
	 * vaddr 是虚拟地址。
	 * size 是数据大小。
	 * riscv_cbom_block_size 是缓存块大小。
	 * */
	ALT_CMO_OP(INVAL, vaddr, size, riscv_cbom_block_size);
}
/*
 * 用于将缓存中的数据写回并无效化
 * */
static inline void arch_dma_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	void *vaddr = phys_to_virt(paddr);//将物理地址 paddr 转换为虚拟地址 vaddr。

#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.wback_inv)) {//检查 noncoherent_cache_ops 结构中的 wback_inv 函数指针是否存在。
		noncoherent_cache_ops.wback_inv(paddr, size);//函数指针存在，调用该函数进行非标准的缓存写回并无效化操作。

		return;
	}
#endif
	/*
	 * 使用替代的缓存管理操作进行缓存写回并无效化。
	 * FLUSH 表示执行缓存写回并无效化操作。
	 * */
	ALT_CMO_OP(FLUSH, vaddr, size, riscv_cbom_block_size);
}

static inline bool arch_sync_dma_clean_before_fromdevice(void)
{
	return true;
}

static inline bool arch_sync_dma_cpu_needs_post_dma_flush(void)
{
	return true;
}
/*
 * 根据 DMA 数据传输的方向，选择适当的缓存操作，以确保在 DMA 传输前后缓存的一致性
 * 参数 paddr：物理地址。
 * 参数 size：数据大小。
 * 参数 dir：DMA 数据方向，类型为 enum dma_data_direction。
 * */
void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE://如果是 DMA_TO_DEVICE，表示数据从内存传输到设备。
		arch_dma_cache_wback(paddr, size);//将缓存中的数据写回到物理内存
		break;

	case DMA_FROM_DEVICE://表示数据从设备传输到内存。
		if (!arch_sync_dma_clean_before_fromdevice()) {//函数如果返回 false，表示不需要在 DMA 传输前清理缓存。
			arch_dma_cache_inv(paddr, size);//进行缓存无效化操作
			break;
		}
		fallthrough;//否则，继续执行下一个 case 语句

	case DMA_BIDIRECTIONAL://表示数据双向传输。
		/* Skip the invalidate here if it's done later */
		if (IS_ENABLED(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) &&//检查是否配置了 CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU
		    arch_sync_dma_cpu_needs_post_dma_flush())//并判断CPU是否需要在DMA之后进行缓存刷新。
			arch_dma_cache_wback(paddr, size);//进行缓存写回操作
		else
			arch_dma_cache_wback_inv(paddr, size);//进行缓存写回并无效化操作
		break;

	default:
		break;
	}
}
/*
 * 用于在 CPU 和 DMA 之间同步缓存操作。
 * 参数 paddr：物理地址。
 * 参数 size：数据大小。
 * 参数 dir：DMA 数据方向，类型为 enum dma_data_direction。
 * */
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE://表示数据从内存传输到设备
		break;

	case DMA_FROM_DEVICE://表示数据从设备传输到内存。
	case DMA_BIDIRECTIONAL://表示数据双向传输。
		/* FROM_DEVICE invalidate needed if speculative CPU prefetch only */
		if (arch_sync_dma_cpu_needs_post_dma_flush())//检查是否需要在 DMA 传输之后进行缓存刷新。
			arch_dma_cache_inv(paddr, size);//需要刷新，调用该函数进行缓存无效化操作。
		break;

	default:
		break;
	}
}
/*
 * 用于为 DMA 操作准备一致性内存。
 * 参数 page：指向页面结构 page 的指针。
 * 参数 size：数据大小
 * */
void arch_dma_prep_coherent(struct page *page, size_t size)
{
	void *flush_addr = page_address(page);//将页面地址转换为虚拟地址

#ifdef CONFIG_RISCV_NONSTANDARD_CACHE_OPS
	if (unlikely(noncoherent_cache_ops.wback_inv)) {//检查 noncoherent_cache_ops 结构中的 wback_inv 函数指针是否存在。
		noncoherent_cache_ops.wback_inv(page_to_phys(page), size);//函数指针存在，调用该函数进行非标准的缓存写回并无效化操作。
		return;
	}
#endif
	/*
	 * 使用替代的缓存管理操作进行缓存写回并无效化。
	 * */
	ALT_CMO_OP(FLUSH, flush_addr, size, riscv_cbom_block_size);
}
/*
 * 用于设置设备的 DMA 操作属性
 * 参数 dev：指向设备结构 device 的指针。
 * 参数 coherent：布尔值，表示设备是否支持一致性 DMA。
 * */
void arch_setup_dma_ops(struct device *dev, bool coherent)
{
	/*如果设备不支持一致性 DMA 且 riscv_cbom_block_size 大于 ARCH_DMA_MINALIGN，发出警告并标记系统状态为不规范 (TAINT_CPU_OUT_OF_SPEC)*/
	WARN_TAINT(!coherent && riscv_cbom_block_size > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than riscv,cbom-block-size (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, riscv_cbom_block_size);
	/*如果设备不支持一致性 DMA 且系统不支持非一致性操作，发出警告并标记系统状态为不规范 (TAINT_CPU_OUT_OF_SPEC)*/
	WARN_TAINT(!coherent && !noncoherent_supported, TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: device non-coherent but no non-coherent operations supported",
		   dev_driver_string(dev), dev_name(dev));

	dev->dma_coherent = coherent;//设置设备的 DMA 一致性属性
}
/*
 * 用于标记系统支持非一致性 DMA
 * */
void riscv_noncoherent_supported(void)
{
	/*如果 riscv_cbom_block_size 为 0，发出警告，表示在没有指定块大小的情况下启用了非一致性 DMA 支持。*/
	WARN(!riscv_cbom_block_size,
	     "Non-coherent DMA support enabled without a block size\n");
	noncoherent_supported = true;//表示系统支持非一致性 DMA。
}
/*
 * 用于设置 DMA 缓存对齐。
 * */
void __init riscv_set_dma_cache_alignment(void)
{
	if (!noncoherent_supported)//检查系统是否支持非一致性 DMA。
		dma_cache_alignment = 1;//如果不支持非一致性 DMA，将 DMA 缓存对齐设置为 1
}
