// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 */

#include <linux/gfp.h>
#include <linux/cache.h>
#include <linux/dma-map-ops.h>
#include <xen/xen.h>

#include <asm/cacheflush.h>
#include <asm/xen/xen-ops.h>

/*
 * 用于设备和DMA之间的同步操作
 * */
void arch_sync_dma_for_device(phys_addr_t paddr, size_t size,
			      enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);//将物理地址 paddr 转换为虚拟地址

	dcache_clean_poc(start, start + size);//清理从 start 到 start + size 范围内的数据缓存。
}
/*
 * 用于在 CPU 和 DMA 之间同步缓存操作
 * */
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size,
			   enum dma_data_direction dir)
{
	unsigned long start = (unsigned long)phys_to_virt(paddr);//将物理地址 paddr 转换为虚拟地址

	if (dir == DMA_TO_DEVICE)//如果是数据从内存传输到设备
		return;//不需要进行缓存无效化操作，直接返回。

	dcache_inval_poc(start, start + size);//清理从 start 到 start + size 范围内的数据缓存。
}
/*
 * 用于准备一致性 DMA 操作。
 * */
void arch_dma_prep_coherent(struct page *page, size_t size)
{
	unsigned long start = (unsigned long)page_address(page);//将页面地址 page 转换为虚拟地址

	dcache_clean_poc(start, start + size);//理从 start 到 start + size 范围内的数据缓存。
}

void arch_setup_dma_ops(struct device *dev, bool coherent)
{
	int cls = cache_line_size_of_cpu();//获取当前 CPU 的缓存行大小
	/*如果设备不支持一致性 DMA 且 CPU 的缓存行大小大于 ARCH_DMA_MINALIGN，发出警告并标记系统状态为不规范 (TAINT_CPU_OUT_OF_SPEC)*/
	WARN_TAINT(!coherent && cls > ARCH_DMA_MINALIGN,
		   TAINT_CPU_OUT_OF_SPEC,
		   "%s %s: ARCH_DMA_MINALIGN smaller than CTR_EL0.CWG (%d < %d)",
		   dev_driver_string(dev), dev_name(dev),
		   ARCH_DMA_MINALIGN, cls);

	dev->dma_coherent = coherent;//设置设备的 DMA 一致性属性

	xen_setup_dma_ops(dev);//调用 Xen 特定的 DMA 设置操作.该函数用于在 Xen 虚拟化环境中设置设备的 DMA 操作属性。
}
