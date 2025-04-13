// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: exregion - ACPI default op_region (address space) handlers
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_EXECUTER
ACPI_MODULE_NAME("exregion")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_system_memory_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the System Memory address space (Op Region)
 *
 ******************************************************************************/
/* 处理ACPI系统内存空间的读写操作 */
acpi_status
acpi_ex_system_memory_space_handler(u32 function,
				    acpi_physical_address address,
				    u32 bit_width,
				    u64 *value,
				    void *handler_context, void *region_context)
{
	acpi_status status = AE_OK;
	void *logical_addr_ptr = NULL;
	struct acpi_mem_space_context *mem_info = region_context;//获取区域的元数据（如物理地址基址、长度）
	struct acpi_mem_mapping *mm = mem_info->cur_mm;//获取当前活跃的内存映射节点（cur_mm指向最近使用的映射）
	u32 length;
	acpi_size map_length;
#ifdef ACPI_MISALIGNMENT_NOT_SUPPORTED
	u32 remainder;
#endif

	ACPI_FUNCTION_TRACE(ex_system_memory_space_handler);

	/* Validate and translate the bit width */
	/* 验证并将位宽转换为字节长度 */
	switch (bit_width) {
	case 8:

		length = 1;
		break;

	case 16:

		length = 2;
		break;

	case 32:

		length = 4;
		break;

	case 64:

		length = 8;
		break;

	default:

		ACPI_ERROR((AE_INFO, "Invalid SystemMemory width %u",
			    bit_width));
		return_ACPI_STATUS(AE_AML_OPERAND_VALUE);//无效位宽返回AE_AML_OPERAND_VALUE
	}

#ifdef ACPI_MISALIGNMENT_NOT_SUPPORTED
	/*
	 * Hardware does not support non-aligned data transfers, we must verify
	 * the request.
	 */
	(void)acpi_ut_short_divide((u64) address, length, NULL, &remainder);//检查地址是否对齐到length的倍数，适用场景于硬件不支持非对齐访问时触发。
	if (remainder != 0) {
		return_ACPI_STATUS(AE_AML_ALIGNMENT);
	}
#endif

	/*
	 * Does the request fit into the cached memory mapping?
	 * Is 1) Address below the current mapping? OR
	 *    2) Address beyond the current mapping?
	 */
	if (!mm || (address < mm->physical_address) ||
	    ((u64) address + length > (u64) mm->physical_address + mm->length)) {//如果当前映射无效或地址超出范围
		/*
		 * The request cannot be resolved by the current memory mapping.
		 *
		 * Look for an existing saved mapping covering the address range
		 * at hand.  If found, save it as the current one and carry out
		 * the access.
		 */
		for (mm = mem_info->first_mm; mm; mm = mm->next_mm) {//遍历现有映射链表寻找匹配项
			/* 跳过当前映射并检查覆盖范围 */
			if (mm == mem_info->cur_mm)
				continue;

			if (address < mm->physical_address)
				continue;

			if ((u64) address + length >
					(u64) mm->physical_address + mm->length)
				continue;

			mem_info->cur_mm = mm;//将符合范围的映射节点记录到当前活跃节点（mem_info->cur_mm）
			goto access;
		}

		/* Create a new mappings list entry */
		/* 没有找到，创建新映射 */
		mm = ACPI_ALLOCATE_ZEROED(sizeof(*mm));//为内存映射节点结构申请内存
		if (!mm) {
			ACPI_ERROR((AE_INFO,
				    "Unable to save memory mapping at 0x%8.8X%8.8X, size %u",
				    ACPI_FORMAT_UINT64(address), length));
			return_ACPI_STATUS(AE_NO_MEMORY);
		}

		/*
		 * 尝试将请求的地址映射到区域末尾。但是，我们绝不会映
		 * 射超过一页，也不会跨越页边界。
		 */
		map_length = (acpi_size)
		    ((mem_info->address + mem_info->length) - address);//计算映射长度

		if (map_length > ACPI_DEFAULT_PAGE_SIZE)//映射长度不能超过一页
			map_length = ACPI_DEFAULT_PAGE_SIZE;

		/* Create a new mapping starting at the address given */

		logical_addr_ptr = acpi_os_map_memory(address, map_length);//为新地址创建映射，返回虚拟地址
		if (!logical_addr_ptr) {
			ACPI_ERROR((AE_INFO,
				    "Could not map memory at 0x%8.8X%8.8X, size %u",
				    ACPI_FORMAT_UINT64(address),
				    (u32)map_length));
			ACPI_FREE(mm);//映射失败，释放节点内存
			return_ACPI_STATUS(AE_NO_MEMORY);
		}

		/* Save the physical address and mapping size */
		/* 保存物理地址和映射大小到映射节点 */
		mm->logical_address = logical_addr_ptr;//映射的虚拟地址
		mm->physical_address = address;//物理地址
		mm->length = map_length;

		/*
		 * Add the new entry to the mappigs list and save it as the
		 * current mapping.
		 * 将新映射节点添加到映射列表头部，并将其保存为当前映射。
		 */
		mm->next_mm = mem_info->first_mm;
		mem_info->first_mm = mm;

		mem_info->cur_mm = mm;
	}

access:
	/*
	 * Generate a logical pointer corresponding to the address we want to
	 * access
	 */
	logical_addr_ptr = mm->logical_address +
		((u64) address - (u64) mm->physical_address);//根据物理地址偏移计算虚拟地址指针(映射的虚拟基址-物理地址偏移量)

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "System-Memory (width %u) R/W %u Address=%8.8X%8.8X\n",
			  bit_width, function, ACPI_FORMAT_UINT64(address)));

	/*
	 * 执行内存读取或写入操作
	 *
	 * 注意：对于不支持非对齐传输的机器，已在上文检查了目标地址的对齐情况。我们
	 * 不会尝试将传输拆分为更小的（字节大小）块，因为AML特别要求了硬件可能需要的
	 * 传输宽度。
	 */
	switch (function) {//读操作
	case ACPI_READ:

		*value = 0;
		switch (bit_width) {//根据位宽选择对应的位宽读函数
		case 8:

			*value = (u64)ACPI_GET8(logical_addr_ptr);//从虚拟地址读取8位宽的数据。
			break;

		case 16:

			*value = (u64)ACPI_GET16(logical_addr_ptr);
			break;

		case 32:

			*value = (u64)ACPI_GET32(logical_addr_ptr);
			break;

		case 64:

			*value = (u64)ACPI_GET64(logical_addr_ptr);
			break;

		default:

			/* bit_width was already validated */

			break;
		}
		break;

	case ACPI_WRITE://写操作

		switch (bit_width) {
		case 8:

			ACPI_SET8(logical_addr_ptr, *value);//将value写入虚拟地址的8位宽位置
			break;

		case 16:

			ACPI_SET16(logical_addr_ptr, *value);
			break;

		case 32:

			ACPI_SET32(logical_addr_ptr, *value);
			break;

		case 64:

			ACPI_SET64(logical_addr_ptr, *value);
			break;

		default:

			/* bit_width was already validated */

			break;
		}
		break;

	default:

		status = AE_BAD_PARAMETER;
		break;
	}

	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_system_io_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the System IO address space (Op Region)
 *
 ******************************************************************************/
/* 
 * acpi_ex_system_io_space_handler - ACPI系统I/O空间访问处理程序
 * @function:    操作类型（读/写）
 * @address:     I/O端口物理地址
 * @bit_width:   访问位宽（8/16/32位）
 * @value:       读操作时存储结果值，写操作时提供输入值
 * @handler_context: 处理程序上下文（通常为NULL）
 * @region_context:  区域上下文（通常为NULL）
 * 
 * 返回值:
 *  AE_OK         - 操作成功
 *  AE_BAD_PARAMETER - 无效参数
 *  AE_IO_ERROR   - I/O访问失败（具体错误码见底层函数）
 * 
 * 功能描述:
 *  该函数是ACPI系统I/O地址空间（SystemIO OpRegion）的标准访问处理程序，
 *  负责执行实际的端口读写操作，支持不同位宽的访问模式。
 */
acpi_status
acpi_ex_system_io_space_handler(u32 function,
				acpi_physical_address address,
				u32 bit_width,
				u64 *value,
				void *handler_context, void *region_context)
{
	acpi_status status = AE_OK;
	u32 value32;//中间值缓存（用于32位操作）

	ACPI_FUNCTION_TRACE(ex_system_io_space_handler);

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "System-IO (width %u) R/W %u Address=%8.8X%8.8X\n",
			  bit_width, function, ACPI_FORMAT_UINT64(address)));

	/* Decode the function parameter */

	switch (function) {//操作类型分发 
	case ACPI_READ://读操作处理

		status = acpi_hw_read_port((acpi_io_address)address,
					   &value32, bit_width);//调用底层端口读取函数,执行实际的inb/inw/inl指令
		*value = value32;//转换为64位存储（高位清零）
		break;

	case ACPI_WRITE://写操作处理

		status = acpi_hw_write_port((acpi_io_address)address,
					    (u32)*value, bit_width);//调用底层端口写入函数,执行outb/outw/outl指令
		break;

	default://非法操作类型处理

		status = AE_BAD_PARAMETER;//返回参数错误
		break;
	}

	return_ACPI_STATUS(status);
}

#ifdef ACPI_PCI_CONFIGURED
/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_pci_config_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the PCI Config address space (Op Region)
 *
 ******************************************************************************/
/* 
 * acpi_ex_pci_config_space_handler - PCI配置空间操作处理程序
 * @function:     操作类型（ACPI_READ/ACPI_WRITE）
 * @address:      配置寄存器地址（0-255字节偏移）
 * @bit_width:    访问位宽（8/16/32位）
 * @value:        读操作输出缓冲区指针，写操作输入值指针
 * @handler_context: 未使用（保留字段）
 * @region_context:  PCI设备标识信息（struct acpi_pci_id指针）
 *
 * 返回值:
 *  AE_OK         - 操作成功
 *  AE_BAD_PARAMETER - 无效参数
 *  AE_ERROR      - PCI访问失败（具体错误见内核日志）
 *
 * 功能描述:
 *  此函数是ACPI PCI配置空间操作区域的标准处理程序，
 *  通过操作系统底层接口实现真实的PCI配置空间访问。
 *  支持8/16/32位访问模式，符合PCI Local Bus Spec 3.0第6章。
 */
acpi_status
acpi_ex_pci_config_space_handler(u32 function,
				 acpi_physical_address address,
				 u32 bit_width,
				 u64 *value,
				 void *handler_context, void *region_context)
{
	acpi_status status = AE_OK;
	struct acpi_pci_id *pci_id;//PCI设备定位标识符
	u16 pci_register;

	ACPI_FUNCTION_TRACE(ex_pci_config_space_handler);

	/*
	 *  acpi_os(Read|Write)pci_configuration 的参数如下：
	 *  
	 *  pci_segment 是 PCI 总线段范围 0-31，
	 *  pci_bus 是 PCI 总线号范围 0-255，
	 *  pci_device 是 PCI 设备号范围 0-31，
	 *  pci_function 是 PCI 设备功能号，
	 *  pci_register 是配置空间寄存器范围 0-255 字节。
	 *  value - 写操作时的输入值，读操作时的输出地址
	 */
	pci_id = (struct acpi_pci_id *)region_context;//获取PCI设备信息
	pci_register = (u16) (u32) address;//获取地址，并确保地址在合法范围

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "Pci-Config %u (%u) Seg(%04x) Bus(%04x) "
			  "Dev(%04x) Func(%04x) Reg(%04x)\n",
			  function, bit_width, pci_id->segment, pci_id->bus,
			  pci_id->device, pci_id->function, pci_register));

	switch (function) {
	case ACPI_READ:// PCI配置空间读操作

		*value = 0;
		status =
		    acpi_os_read_pci_configuration(pci_id, pci_register, value,
						   bit_width);
		break;

	case ACPI_WRITE://PCI配置空间写操作

		status =
		    acpi_os_write_pci_configuration(pci_id, pci_register,
						    *value, bit_width);
		break;

	default:

		status = AE_BAD_PARAMETER;
		break;
	}

	return_ACPI_STATUS(status);
}
#endif

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_cmos_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the CMOS address space (Op Region)
 *
 ******************************************************************************/

acpi_status
acpi_ex_cmos_space_handler(u32 function,
			   acpi_physical_address address,
			   u32 bit_width,
			   u64 *value,
			   void *handler_context, void *region_context)
{
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE(ex_cmos_space_handler);

	return_ACPI_STATUS(status);
}

#ifdef ACPI_PCI_CONFIGURED
/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_pci_bar_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the PCI bar_target address space (Op Region)
 *
 ******************************************************************************/

acpi_status
acpi_ex_pci_bar_space_handler(u32 function,
			      acpi_physical_address address,
			      u32 bit_width,
			      u64 *value,
			      void *handler_context, void *region_context)
{
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE(ex_pci_bar_space_handler);

	return_ACPI_STATUS(status);
}
#endif

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_data_table_space_handler
 *
 * PARAMETERS:  function            - Read or Write operation
 *              address             - Where in the space to read or write
 *              bit_width           - Field width in bits (8, 16, or 32)
 *              value               - Pointer to in or out value
 *              handler_context     - Pointer to Handler's context
 *              region_context      - Pointer to context specific to the
 *                                    accessed region
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Handler for the Data Table address space (Op Region)
 *
 ******************************************************************************/
/* 
 * acpi_ex_data_table_space_handler - ACPI数据表区域（Data Table Region）访问处理程序
 * @function:      操作类型（ACPI_READ/ACPI_WRITE）
 * @address:       数据表内的偏移地址（相对地址）
 * @bit_width:     访问位宽（8/16/32/64位）
 * @value:         读操作时输出缓冲区指针，写操作时输入数据指针
 * @handler_context: 未使用（保留字段）
 * @region_context:  数据表映射上下文（struct acpi_data_table_mapping指针）
 * 
 * 返回值:
 *  AE_OK           - 操作成功
 *  AE_BAD_PARAMETER - 无效操作类型
 *
 * 功能描述:
 *  该函数实现ACPI数据表区域（如DSDT、SSDT等）的读写访问，
 *  通过内存拷贝操作直接操作数据表内容。关键流程：
 *   1. 从上下文中获取数据表基地址
 *   2. 计算目标地址（基地址 + 偏移量）
 *   3. 按指定位宽执行内存拷贝
 *
 * 注意:
 *   - 调用前必须验证位宽合法性（8/16/32/64位）
 *   - 地址偏移必须在数据表范围内（依赖上层校验）
 */
acpi_status
acpi_ex_data_table_space_handler(u32 function,
				 acpi_physical_address address,
				 u32 bit_width,
				 u64 *value,
				 void *handler_context, void *region_context)
{
	struct acpi_data_table_mapping *mapping;//数据表上下文指针
	char *pointer;//目标内存地址指针

	ACPI_FUNCTION_TRACE(ex_data_table_space_handler);

	mapping = (struct acpi_data_table_mapping *) region_context;//获取数据表上下文（包含数据表基地址）
		
	/* 
    	 * 计算目标内存地址：
    	 * 1. 将上下文中的基地址转换为char*类型（字节粒度）
    	 * 2. address参数为数据表内偏移量（需转换为物理地址差）
    	 * 3. ACPI_PTR_TO_PHYSADDR 宏将基地址转换为物理地址（平台相关实现）
    	 */
	pointer = ACPI_CAST_PTR(char, mapping->pointer) +
	    (address - ACPI_PTR_TO_PHYSADDR(mapping->pointer));

	/*
	 * 执行内存读取或写入。位宽度已经被验证过。
	 */
	switch (function) {
	case ACPI_READ://读操作：数据表 -> 输出缓冲区

		memcpy(ACPI_CAST_PTR(char, value), pointer,
		       ACPI_DIV_8(bit_width));
		break;

	case ACPI_WRITE://写操作：输入数据 -> 数据表

		memcpy(pointer, ACPI_CAST_PTR(char, value),
		       ACPI_DIV_8(bit_width));
		break;

	default:

		return_ACPI_STATUS(AE_BAD_PARAMETER);//非法操作类型处理
	}

	return_ACPI_STATUS(AE_OK);//返回成功状态
}
