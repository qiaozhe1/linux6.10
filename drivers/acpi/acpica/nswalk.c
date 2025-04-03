// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nswalk - Functions for walking the ACPI namespace
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nswalk")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_next_node
 *
 * PARAMETERS:  parent_node         - Parent node whose children we are
 *                                    getting
 *              child_node          - Previous child that was found.
 *                                    The NEXT child will be returned
 *
 * RETURN:      struct acpi_namespace_node - Pointer to the NEXT child or NULL if
 *                                    none is found.
 *
 * DESCRIPTION: Return the next peer node within the namespace. If Handle
 *              is valid, Scope is ignored. Otherwise, the first node
 *              within Scope is returned.
 *
 ******************************************************************************/
struct acpi_namespace_node *acpi_ns_get_next_node(struct acpi_namespace_node
						  *parent_node,
						  struct acpi_namespace_node
						  *child_node)
{
	ACPI_FUNCTION_ENTRY();

	if (!child_node) {

		/* It's really the parent's _scope_ that we want */

		return (parent_node->child);
	}

	/* Otherwise just return the next peer */

	return (child_node->peer);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_next_node_typed
 *
 * PARAMETERS:  type                - Type of node to be searched for
 *              parent_node         - Parent node whose children we are
 *                                    getting
 *              child_node          - Previous child that was found.
 *                                    The NEXT child will be returned
 *
 * RETURN:      struct acpi_namespace_node - Pointer to the NEXT child or NULL if
 *                                    none is found.
 *
 * DESCRIPTION: Return the next peer node within the namespace. If Handle
 *              is valid, Scope is ignored. Otherwise, the first node
 *              within Scope is returned.
 *
 ******************************************************************************/

struct acpi_namespace_node *acpi_ns_get_next_node_typed(acpi_object_type type,
							struct
							acpi_namespace_node
							*parent_node,
							struct
							acpi_namespace_node
							*child_node)
{
	struct acpi_namespace_node *next_node = NULL;

	ACPI_FUNCTION_ENTRY();

	next_node = acpi_ns_get_next_node(parent_node, child_node);


	/* If any type is OK, we are done */

	if (type == ACPI_TYPE_ANY) {

		/* next_node is NULL if we are at the end-of-list */

		return (next_node);
	}

	/* Must search for the node -- but within this scope only */

	while (next_node) {

		/* If type matches, we are done */

		if (next_node->type == type) {
			return (next_node);
		}

		/* Otherwise, move on to the next peer node */

		next_node = next_node->peer;
	}

	/* Not found */

	return (NULL);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_walk_namespace
 *
 * PARAMETERS:  type                - acpi_object_type to search for
 *              start_node          - Handle in namespace where search begins
 *              max_depth           - Depth to which search is to reach
 *              flags               - Whether to unlock the NS before invoking
 *                                    the callback routine
 *              descending_callback - Called during tree descent
 *                                    when an object of "Type" is found
 *              ascending_callback  - Called during tree ascent
 *                                    when an object of "Type" is found
 *              context             - Passed to user function(s) above
 *              return_value        - from the user_function if terminated
 *                                    early. Otherwise, returns NULL.
 * RETURNS:     Status
 *
 * DESCRIPTION: Performs a modified depth-first walk of the namespace tree,
 *              starting (and ending) at the node specified by start_handle.
 *              The callback function is called whenever a node that matches
 *              the type parameter is found. If the callback function returns
 *              a non-zero value, the search is terminated immediately and
 *              this value is returned to the caller.
 *
 *              The point of this procedure is to provide a generic namespace
 *              walk routine that can be called from multiple places to
 *              provide multiple services; the callback function(s) can be
 *              tailored to each task, whether it is a print function,
 *              a compare function, etc.
 *
 ******************************************************************************/
/**
 * acpi_ns_walk_namespace - 遍历ACPI命名空间
 * @type: 要遍历的对象类型(ACPI_TYPE_ANY表示所有类型)
 * @start_node: 遍历起始节点(ACPI_ROOT_OBJECT表示从根开始)
 * @max_depth: 最大遍历深度(ACPI_UINT32_MAX表示无限制)
 * @flags: 控制标志(ACPI_NS_WALK_*系列标志)
 * @descending_callback: 下行回调函数(进入节点时调用)
 * @ascending_callback: 上行回调函数(离开节点时调用) 
 * @context: 传递给回调函数的上下文数据
 * @return_value: 用于存储回调函数返回值的指针
 *
 * 返回值:
 *  AE_OK - 遍历成功完成
 *  AE_BAD_PARAMETER - 参数无效
 *  AE_NO_NAMESPACE - 命名空间未初始化
 *  其他状态码 - 回调函数返回的错误
 *
 * 功能说明:
 * 1. 初始化遍历参数和状态
 * 2. 处理根节点特殊情况
 * 3. 执行深度优先的递归遍历
 * 4. 对每个节点调用相应的回调函数
 */
acpi_status
acpi_ns_walk_namespace(acpi_object_type type,
		       acpi_handle start_node,
		       u32 max_depth,
		       u32 flags,
		       acpi_walk_callback descending_callback,
		       acpi_walk_callback ascending_callback,
		       void *context, void **return_value)
{
	acpi_status status;//操作状态码
	acpi_status mutex_status;//互斥锁状态
	struct acpi_namespace_node *child_node;//当前子节点指针
	struct acpi_namespace_node *parent_node;//父节点指针
	acpi_object_type child_type;//子节点类型
	u32 level;//当前遍历深度
	u8 node_previously_visited = FALSE;//节点访问标记

	ACPI_FUNCTION_TRACE(ns_walk_namespace);

	/* Special case for the namespace Root Node */

	if (start_node == ACPI_ROOT_OBJECT) {//处理根节点特殊情况。（start_node是命名空间地址，命名空间结构第一个成员是对象成员）
		start_node = acpi_gbl_root_node;// 转换为实际的根节点指针
		if (!start_node) {
			return_ACPI_STATUS(AE_NO_NAMESPACE);//返回命名空间未初始化
		}
	}

	/* Null child means "get first node" */

	parent_node = start_node;//设置当前命名空间节点为起始父节点
	child_node = acpi_ns_get_next_node(parent_node, NULL);//获取第一个子节点
	child_type = ACPI_TYPE_ANY;//初始化子节点类型
	level = 1;// 初始化当前深度

	/*
	 * Traverse the tree of nodes until we bubble back up to where we
	 * started. When Level is zero, the loop is done because we have
	 * bubbled up to (and passed) the original parent handle (start_entry)
	 */
	while (level > 0 && child_node) {
		status = AE_OK;

		/* Found next child, get the type if we are not searching for ANY */

		if (type != ACPI_TYPE_ANY) {//当非ANY类型时
			child_type = child_node->type;//获取当前子节点类型
		}

		/*
		 * 忽略所有临时命名空间节点（在控制方法执行期间创建的），除非有特
		 * 别指示。这些临时节点可能会导致竞态条件，因为它们可能在用户函数
		 * 执行期间被删除（如果用户函数调用之前解锁了命名空间）。只有调试
		 * 器命名空间转储会检查这些临时节点。
		 */
		if ((child_node->flags & ANOBJ_TEMPORARY) &&
		    !(flags & ACPI_NS_WALK_TEMP_NODES)) {//节点是临时的且未设置访问临时节点标志
			status = AE_CTRL_DEPTH;//跳过该节点（返回控制状态）
		}

		/* Type must match requested type */

		else if (child_type == type) {//检查节点类型是否匹配请求类型
			/*
			 * 找到匹配的节点，调用用户回调函数。如果设置了标志，则解锁命名空间。
			 */
			if (flags & ACPI_NS_WALK_UNLOCK) {//需要释放命名空间锁
				mutex_status =
				    acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);//释放互斥锁
				if (ACPI_FAILURE(mutex_status)) {
					return_ACPI_STATUS(mutex_status);//解锁失败，返回错误
				}
			}

			/*
			 * 调用用户回调函数（下降或上升阶段）
			 */
			if (!node_previously_visited) {//第一次访问该节点（下降阶段）
				if (descending_callback) {//存在下降回调函数(进入节点时调用)
					status =
					    descending_callback(child_node,
								level, context,
								return_value);//调用回调
				}
			} else {//第二次访问（回溯阶段）
				if (ascending_callback) {//存在上升回调函数（离开节点时调用）
					status =
					    ascending_callback(child_node,
							       level, context,
							       return_value);//调用回调
				}
			}

			if (flags & ACPI_NS_WALK_UNLOCK) {// 回调后重新加锁（如果之前解锁过）
				mutex_status =
				    acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);//重新获取锁
				if (ACPI_FAILURE(mutex_status)) {
					return_ACPI_STATUS(mutex_status);//加锁失败，返回错误
				}
			}

			switch (status) {// 处理回调返回状态
			case AE_OK:
			case AE_CTRL_DEPTH://继续遍历

				/* Just keep going */
				break;

			case AE_CTRL_TERMINATE://用户要求终止遍历

				/* Exit now, with OK status */

				return_ACPI_STATUS(AE_OK);

			default://其他错误码直接返回

				/* All others are valid exceptions */

				return_ACPI_STATUS(status);
			}
		}

		/*
		 * 深度优先搜索：如果我们被允许，尝试进入命名空间的下一层。如果我们
		 * 达到了调用者指定的最大深度，或者用户函数指明已经达到最大深度，则
		 * 不再深入。
		 */
		if (!node_previously_visited &&//如果是首次访问该节点
		    (level < max_depth) && (status != AE_CTRL_DEPTH)) {//并且未达到最大深度且状态是未被跳过
			if (child_node->child) {//如果当前节点有子节点

				/* There is at least one child of this node, visit it */

				level++;//进入下一层级
				parent_node = child_node;//当前节点成为父节点
				child_node =
				    acpi_ns_get_next_node(parent_node, NULL);//获取第一个子节点
				continue;// 重新开始循环处理子节点
			}
		}

		/* 处理节点遍历回溯逻辑 */
		/* 情况1：没有子节点，重新访问当前节点 */
		if (!node_previously_visited) {//如果是第一次访问该节点
			node_previously_visited = TRUE;//标记为已访问
			continue;//重新进入循环处理该节点的回调（上升阶段）
		}

		/* No more children, visit peers */
		/* 情况2：遍历兄弟节点 */
		child_node = acpi_ns_get_next_node(parent_node, child_node);//获取下一个兄弟节点
		if (child_node) {// 存在兄弟节点
			node_previously_visited = FALSE;//新兄弟节点未被访问过
		}

		/* No peers, re-visit parent */

		else {//无兄弟节点，回退到父节点
			/*
			 * No more children of this node (acpi_ns_get_next_node failed), go
			 * back upwards in the namespace tree to the node's parent.
			 */
			level--;//回退层级
			child_node = parent_node;//当前节点设为父节点
			parent_node = parent_node->parent;

			node_previously_visited = TRUE;//标记父节点为已访问
		}
	}

	/* Complete walk, not terminated by user function */

	return_ACPI_STATUS(AE_OK);
}
