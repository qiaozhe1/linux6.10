// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: utcache - local cache allocation routines
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utcache")

#ifdef ACPI_USE_LOCAL_CACHE
/*******************************************************************************
 *
 * FUNCTION:    acpi_os_create_cache
 *
 * PARAMETERS:  cache_name      - Ascii name for the cache
 *              object_size     - Size of each cached object
 *              max_depth       - Maximum depth of the cache (in objects)
 *              return_cache    - Where the new cache object is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a cache object
 *
 ******************************************************************************/
acpi_status
acpi_os_create_cache(char *cache_name,
		     u16 object_size,
		     u16 max_depth, struct acpi_memory_list **return_cache)
{
	struct acpi_memory_list *cache;

	ACPI_FUNCTION_ENTRY();

	if (!cache_name || !return_cache || !object_size) {
		return (AE_BAD_PARAMETER);
	}

	/* Create the cache object */

	cache = acpi_os_allocate(sizeof(struct acpi_memory_list));
	if (!cache) {
		return (AE_NO_MEMORY);
	}

	/* Populate the cache object and return it */

	memset(cache, 0, sizeof(struct acpi_memory_list));
	cache->list_name = cache_name;
	cache->object_size = object_size;
	cache->max_depth = max_depth;

	*return_cache = cache;
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_os_purge_cache
 *
 * PARAMETERS:  cache           - Handle to cache object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Free all objects within the requested cache.
 *
 ******************************************************************************/

acpi_status acpi_os_purge_cache(struct acpi_memory_list *cache)
{
	void *next;
	acpi_status status;

	ACPI_FUNCTION_ENTRY();

	if (!cache) {
		return (AE_BAD_PARAMETER);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_CACHES);
	if (ACPI_FAILURE(status)) {
		return (status);
	}

	/* Walk the list of objects in this cache */

	while (cache->list_head) {

		/* Delete and unlink one cached state object */

		next = ACPI_GET_DESCRIPTOR_PTR(cache->list_head);
		ACPI_FREE(cache->list_head);

		cache->list_head = next;
		cache->current_depth--;
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_CACHES);
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_os_delete_cache
 *
 * PARAMETERS:  cache           - Handle to cache object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Free all objects within the requested cache and delete the
 *              cache object.
 *
 ******************************************************************************/

acpi_status acpi_os_delete_cache(struct acpi_memory_list *cache)
{
	acpi_status status;

	ACPI_FUNCTION_ENTRY();

	/* Purge all objects in the cache */

	status = acpi_os_purge_cache(cache);
	if (ACPI_FAILURE(status)) {
		return (status);
	}

	/* Now we can delete the cache object */

	acpi_os_free(cache);
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_os_release_object
 *
 * PARAMETERS:  cache       - Handle to cache object
 *              object      - The object to be released
 *
 * RETURN:      None
 *
 * DESCRIPTION: Release an object to the specified cache. If cache is full,
 *              the object is deleted.
 *
 ******************************************************************************/

acpi_status acpi_os_release_object(struct acpi_memory_list *cache, void *object)//将对象释放回缓存或直接释放内存
{
	acpi_status status;

	ACPI_FUNCTION_ENTRY();

	if (!cache || !object) {//参数有效性检查（缓存结构或对象指针为空）
		return (AE_BAD_PARAMETER);
	}

	/* If cache is full, just free this object */
	/* 如果缓存已满，直接释放对象内存 */
	if (cache->current_depth >= cache->max_depth) {//如果当前缓存深度 >= 最大允许深度
		ACPI_FREE(object);// 释放对象内存（调用系统内存释放函数）
		ACPI_MEM_TRACKING(cache->total_freed++);//更新内存跟踪统计（记录释放次数）
	}

	/* Otherwise put this object back into the cache */

	else {//否则将对象放回缓存
		status = acpi_ut_acquire_mutex(ACPI_MTX_CACHES);// 获取缓存互斥锁（确保线程安全）
		if (ACPI_FAILURE(status)) {//如果锁获取失败
			return (status);//直接返回错误码
		}

		/* Mark the object as cached */
		/*  标记对象为已缓存状态 */
		memset(object, 0xCA, cache->object_size);//将对象内存填充0xCA（调试标记，表示已释放）
		ACPI_SET_DESCRIPTOR_TYPE(object, ACPI_DESC_TYPE_CACHED);//设置描述符类型为"已缓存"

		/* Put the object at the head of the cache list */
		/* 将对象插入缓存链表头部 */
		ACPI_SET_DESCRIPTOR_PTR(object, cache->list_head);//设置对象的指针字段指向当前链表头
		cache->list_head = object;//新对象成为链表头
		cache->current_depth++;// 缓存占用深度+1

		(void)acpi_ut_release_mutex(ACPI_MTX_CACHES);//释放互斥锁（忽略返回值）
	}

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_os_acquire_object
 *
 * PARAMETERS:  cache           - Handle to cache object
 *
 * RETURN:      the acquired object. NULL on error
 *
 * DESCRIPTION: Get an object from the specified cache. If cache is empty,
 *              the object is allocated.
 *
 ******************************************************************************/

void *acpi_os_acquire_object(struct acpi_memory_list *cache)//从指定的内存缓存中获取对象。优先从缓存中复用空闲对象，若缓存已空则分配新内存
{
	acpi_status status;
	void *object;

	ACPI_FUNCTION_TRACE(os_acquire_object);

	if (!cache) {//参数有效性检查：缓存结构为空则直接返回失败
		return_PTR(NULL);
	}

	status = acpi_ut_acquire_mutex(ACPI_MTX_CACHES);// 获取缓存互斥锁（确保线程安全）
	if (ACPI_FAILURE(status)) {
		return_PTR(NULL);
	}

	ACPI_MEM_TRACKING(cache->requests++);//更新内存请求计数器（用于统计）

	/* Check the cache first */
	/* 尝试从缓存链表获取对象 */
	if (cache->list_head) {//缓存链表非空，存在可用对象

		/* There is an object available, use it */

		object = cache->list_head;//获取链表头对象指针
		cache->list_head = ACPI_GET_DESCRIPTOR_PTR(object);//更新链表头到下一个对象（通过对象中的指针字段）

		cache->current_depth--;// 缓存占用深度递减

		ACPI_MEM_TRACKING(cache->hits++);//更新缓存命中次数统计
		ACPI_DEBUG_PRINT_RAW((ACPI_DB_EXEC,
				      "%s: Object %p from %s cache\n",
				      ACPI_GET_FUNCTION_NAME, object,
				      cache->list_name));//输出调试信息（无需额外锁）

		status = acpi_ut_release_mutex(ACPI_MTX_CACHES);//释放锁并检查状态
		if (ACPI_FAILURE(status)) {
			return_PTR(NULL);
		}

		/* Clear (zero) the previously used Object */

		memset(object, 0, cache->object_size);//清零对象内存（确保干净状态）
	} else {//缓存为空，需分配新内存
		/* The cache is empty, create a new object */

		ACPI_MEM_TRACKING(cache->total_allocated++);//更新总分配次数统计

#ifdef ACPI_DBG_TRACK_ALLOCATIONS
		if ((cache->total_allocated - cache->total_freed) >
		    cache->max_occupied) {
			cache->max_occupied =
			    cache->total_allocated - cache->total_freed;//记录最大内存占用量（调试用途）
		}
#endif

		/* Avoid deadlock with ACPI_ALLOCATE_ZEROED */

		status = acpi_ut_release_mutex(ACPI_MTX_CACHES);//释放锁以避免死锁（分配内存可能需要其他资源）
		if (ACPI_FAILURE(status)) {
			return_PTR(NULL);
		}

		object = ACPI_ALLOCATE_ZEROED(cache->object_size);//分配新内存（kmalloc清零初始化）
		if (!object) {
			return_PTR(NULL);
		}
	}

	return_PTR(object);
}
#endif				/* ACPI_USE_LOCAL_CACHE */
