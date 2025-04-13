// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: utownerid - Support for Table/Method Owner IDs
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utownerid")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_allocate_owner_id
 *
 * PARAMETERS:  owner_id        - Where the new owner ID is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Allocate a table or method owner ID. The owner ID is used to
 *              track objects created by the table or method, to be deleted
 *              when the method exits or the table is unloaded.
 *
 ******************************************************************************/
/* 
 * acpi_ut_allocate_owner_id - 分配唯一的所有者ID用于资源跟踪
 * @owner_id: 输出参数，用于存储新分配的所有者ID
 *
 * 核心机制：
 * 1. 使用32位掩码数组管理4095个可用ID（0x000-0xFFE）
 * 2. 采用轮询算法避免ID分配热点
 * 3. 通过全局掩码数组和索引实现无锁位操作
 * 4. 错误检测机制防止重复分配
 *
 * 关键限制：
 * - 最大支持4095个并发ID（ACPI规范限制）
 * - ID 0x000为非法值，0xFFF为保留值
 */
acpi_status acpi_ut_allocate_owner_id(acpi_owner_id *owner_id)
{
	u32 i;//外层循环计数器（掩码数组索引）
	u32 j;//当前处理的掩码数组下标
	u32 k;//位偏移计数器（0-31位）
	acpi_status status;//操作状态码

	ACPI_FUNCTION_TRACE(ut_allocate_owner_id);

	/*防止对同一位置重复分配ID */
	if (*owner_id) {//检查目标地址是否已包含有效ID
		ACPI_ERROR((AE_INFO,
			    "Owner ID [0x%3.3X] already exists", *owner_id));
		return_ACPI_STATUS(AE_ALREADY_EXISTS);
	}

	/* Mutex for the global ID mask */

	status = acpi_ut_acquire_mutex(ACPI_MTX_CACHES);//获取全局缓存互斥锁
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/*
    	 * 搜索策略：
    	 * 1. 从上次成功位置开始搜索（j = acpi_gbl_last_owner_id_index）
    	 * 2. 循环次数为掩码数组长度+1（处理环状搜索）
    	 * 3. 每个掩码从上次偏移位置开始检查
    	 */
	for (i = 0, j = acpi_gbl_last_owner_id_index;
	     i < (ACPI_NUM_OWNERID_MASKS + 1); i++, j++) {//最多遍历数组长度+1次
		if (j >= ACPI_NUM_OWNERID_MASKS) {//处理数组越界（环状回绕)
			j = 0;	/* Wraparound to start of mask array */
		}

		/* 遍历当前掩码的32个位 */
		for (k = acpi_gbl_next_owner_id_offset; k < 32; k++) {//从上次偏移开始
			if (acpi_gbl_owner_id_mask[j] == ACPI_UINT32_MAX) {//如果当前掩码已满

				/* There are no free IDs in this mask */

				break;// 跳过该掩码，进入下一个数组元素
			}

			/*
			 * 注意：u32 强制转换确保 1 作为无符号整数存储。省略强制转换可能会导致
			 * 1作为 int 类型存储。一些编译器或运行时错误检测可能会将此视为错误。
			 */
			if (!(acpi_gbl_owner_id_mask[j] & ((u32)1 << k))) {//检查第k位是否可用
				/*
				 * 找到一个空闲的 ID。实际的 ID 是位索引加一，零是无效的所有者ID。
				 * 将其保存为最后分配的 ID，并更新全局 ID 掩码。
				 */
				acpi_gbl_owner_id_mask[j] |= ((u32)1 << k);//设置占用位

				acpi_gbl_last_owner_id_index = (u8)j;//更新最后使用下标
				acpi_gbl_next_owner_id_offset = (u8)(k + 1);//更新下次起始偏移

				/*
				 * 从索引和位位置构造编码的 ID
				 *
				 * 注意：最后的 [j].k（位 4095）永远不会使用，并且被标记为永久分配（防止 +1 溢出）
				 */
				*owner_id =
				    (acpi_owner_id)((k + 1) + ACPI_MUL_32(j));//构造32位所有者ID：j*32 + (k+1)

				ACPI_DEBUG_PRINT((ACPI_DB_VALUES,
						  "Allocated OwnerId: 0x%3.3X\n",
						  (unsigned int)*owner_id));
				goto exit;
			}
		}

		acpi_gbl_next_owner_id_offset = 0;// 重置位偏移计数器
	}

	/*
	 * 所有的 owner_ids 都已分配完毕。通常情况下，这不应该发生，因为 ID 会在释
	 * 放后被重用。ID 在表加载时（每个表一个）和方法执行时分配，
	 * 当表卸载或方法执行完成时，它们会被释放。
	 *
	 * 如果发生此错误，可能是因为调用的控制方法有非常深的嵌套，或者可能存在一个
	 * 错误，导致 ID 没有被正确释放。
	 */
	status = AE_OWNER_ID_LIMIT;//定义错误码（0x000F）
	ACPI_ERROR((AE_INFO,
		    "Could not allocate new OwnerId (4095 max), AE_OWNER_ID_LIMIT"));

exit:
	(void)acpi_ut_release_mutex(ACPI_MTX_CACHES);//释放互斥锁
	return_ACPI_STATUS(status);//带调试跟踪的返回
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_release_owner_id
 *
 * PARAMETERS:  owner_id_ptr        - Pointer to a previously allocated owner_ID
 *
 * RETURN:      None. No error is returned because we are either exiting a
 *              control method or unloading a table. Either way, we would
 *              ignore any error anyway.
 *
 * DESCRIPTION: Release a table or method owner ID. Valid IDs are 1 - 255
 *
 ******************************************************************************/

void acpi_ut_release_owner_id(acpi_owner_id *owner_id_ptr)
{
	acpi_owner_id owner_id = *owner_id_ptr;
	acpi_status status;
	u32 index;
	u32 bit;

	ACPI_FUNCTION_TRACE_U32(ut_release_owner_id, owner_id);

	/* Always clear the input owner_id (zero is an invalid ID) */

	*owner_id_ptr = 0;

	/* Zero is not a valid owner_ID */

	if (owner_id == 0) {
		ACPI_ERROR((AE_INFO, "Invalid OwnerId: 0x%3.3X", owner_id));
		return_VOID;
	}

	/* Mutex for the global ID mask */

	status = acpi_ut_acquire_mutex(ACPI_MTX_CACHES);
	if (ACPI_FAILURE(status)) {
		return_VOID;
	}

	/* Normalize the ID to zero */

	owner_id--;

	/* Decode ID to index/offset pair */

	index = ACPI_DIV_32(owner_id);
	bit = (u32)1 << ACPI_MOD_32(owner_id);

	/* Free the owner ID only if it is valid */

	if (acpi_gbl_owner_id_mask[index] & bit) {
		acpi_gbl_owner_id_mask[index] ^= bit;
	} else {
		ACPI_ERROR((AE_INFO,
			    "Attempted release of non-allocated OwnerId: 0x%3.3X",
			    owner_id + 1));
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_CACHES);
	return_VOID;
}
