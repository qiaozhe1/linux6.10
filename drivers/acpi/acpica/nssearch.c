// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: nssearch - Namespace search
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"

#ifdef ACPI_ASL_COMPILER
#include "amlcode.h"
#endif

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nssearch")

/* Local prototypes */
static acpi_status
acpi_ns_search_parent_tree(u32 target_name,
			   struct acpi_namespace_node *node,
			   acpi_object_type type,
			   struct acpi_namespace_node **return_node);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_search_one_scope
 *
 * PARAMETERS:  target_name     - Ascii ACPI name to search for
 *              parent_node     - Starting node where search will begin
 *              type            - Object type to match
 *              return_node     - Where the matched Named obj is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Search a single level of the namespace. Performs a
 *              simple search of the specified level, and does not add
 *              entries or search parents.
 *
 *
 *      Named object lists are built (and subsequently dumped) in the
 *      order in which the names are encountered during the namespace load;
 *
 *      All namespace searching is linear in this implementation, but
 *      could be easily modified to support any improved search
 *      algorithm. However, the linear search was chosen for simplicity
 *      and because the trees are small and the other interpreter
 *      execution overhead is relatively high.
 *
 *      Note: CPU execution analysis has shown that the AML interpreter spends
 *      a very small percentage of its time searching the namespace. Therefore,
 *      the linear search seems to be sufficient, as there would seem to be
 *      little value in improving the search.
 *
 ******************************************************************************/
/* 在指定的命名空间层级（即父节点的子节点链表中）搜索特定名称的节点 */
acpi_status
acpi_ns_search_one_scope(u32 target_name,//要查找的目标名称（4字符压缩为32位整数）
			 struct acpi_namespace_node *parent_node,//父命名空间节点（搜索起点）
			 acpi_object_type type,//期望的对象类型（过滤条件）
			 struct acpi_namespace_node **return_node)//返回找到的节点指针
{
	struct acpi_namespace_node *node;//当前遍历的节点指针

	ACPI_FUNCTION_TRACE(ns_search_one_scope);

#ifdef ACPI_DEBUG_OUTPUT
	if (ACPI_LV_NAMES & acpi_dbg_level) {//如果启用了命名空间调试级别
		char *scope_name;

		/* 获取父节点的规范化路径名（用于调试输出） */
		scope_name = acpi_ns_get_normalized_pathname(parent_node, TRUE);
		if (scope_name) {//如果成功获取路径名
			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Searching %s (%p) For [%4.4s] (%s)\n",
					  scope_name, parent_node,
					  ACPI_CAST_PTR(char, &target_name),
					  acpi_ut_get_type_name(type)));

			ACPI_FREE(scope_name);
		}
	}
#endif

	/*
	 * Search for name at this namespace level, which is to say that we
	 * must search for the name among the children of this object
	 */
	/* 在当前命名空间层级搜索名称，即需要在该对象的子节点中查找 */
	node = parent_node->child;//从父节点的第一个子节点开始遍历
	while (node) {//遍历同级节点链表

		/* Check for match against the name */
		/* 检查名称是否匹配 */
		if (node->name.integer == target_name) {//比较压缩后的名称整数

			/* Resolve a control method alias if any */

			if (acpi_ns_get_type(node) ==
			    ACPI_TYPE_LOCAL_METHOD_ALIAS) {//如果是控制方法别名，则解析实际对象
				node =
				    ACPI_CAST_PTR(struct acpi_namespace_node,
						  node->object);//获取别名指向的实际对象
			}

			/* Found matching entry */

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Name [%4.4s] (%s) %p found in scope [%4.4s] %p\n",
					  ACPI_CAST_PTR(char, &target_name),//显示目标名称
					  acpi_ut_get_type_name(node->type),//显示节点实际类型
					  node,
					  acpi_ut_get_node_name(parent_node),//父节点名称
					  parent_node));//父节点地址

			*return_node = node;//通过输出参数返回找到的节点
			return_ACPI_STATUS(AE_OK);
		}

		/* Didn't match name, move on to the next peer object */

		node = node->peer;//peer指针指向下一个同级节点
	}

	/* Searched entire namespace level, not found */

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
			  "Name [%4.4s] (%s) not found in search in scope [%4.4s] "
			  "%p first child %p\n",
			  ACPI_CAST_PTR(char, &target_name),
			  acpi_ut_get_type_name(type),
			  acpi_ut_get_node_name(parent_node), parent_node,
			  parent_node->child));

	return_ACPI_STATUS(AE_NOT_FOUND);//返回未找到错误
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_search_parent_tree
 *
 * PARAMETERS:  target_name     - Ascii ACPI name to search for
 *              node            - Starting node where search will begin
 *              type            - Object type to match
 *              return_node     - Where the matched Node is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Called when a name has not been found in the current namespace
 *              level. Before adding it or giving up, ACPI scope rules require
 *              searching enclosing scopes in cases identified by acpi_ns_local().
 *
 *              "A name is located by finding the matching name in the current
 *              name space, and then in the parent name space. If the parent
 *              name space does not contain the name, the search continues
 *              recursively until either the name is found or the name space
 *              does not have a parent (the root of the name space). This
 *              indicates that the name is not found" (From ACPI Specification,
 *              section 5.3)
 *
 ******************************************************************************/

static acpi_status
acpi_ns_search_parent_tree(u32 target_name,
			   struct acpi_namespace_node *node,
			   acpi_object_type type,
			   struct acpi_namespace_node **return_node)
{
	acpi_status status;
	struct acpi_namespace_node *parent_node;

	ACPI_FUNCTION_TRACE(ns_search_parent_tree);

	parent_node = node->parent;

	/*
	 * If there is no parent (i.e., we are at the root) or type is "local",
	 * we won't be searching the parent tree.
	 */
	if (!parent_node) {
		ACPI_DEBUG_PRINT((ACPI_DB_NAMES, "[%4.4s] has no parent\n",
				  ACPI_CAST_PTR(char, &target_name)));
		return_ACPI_STATUS(AE_NOT_FOUND);
	}

	if (acpi_ns_local(type)) {
		ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
				  "[%4.4s] type [%s] must be local to this scope (no parent search)\n",
				  ACPI_CAST_PTR(char, &target_name),
				  acpi_ut_get_type_name(type)));
		return_ACPI_STATUS(AE_NOT_FOUND);
	}

	/* Search the parent tree */

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
			  "Searching parent [%4.4s] for [%4.4s]\n",
			  acpi_ut_get_node_name(parent_node),
			  ACPI_CAST_PTR(char, &target_name)));

	/* Search parents until target is found or we have backed up to the root */

	while (parent_node) {
		/*
		 * Search parent scope. Use TYPE_ANY because we don't care about the
		 * object type at this point, we only care about the existence of
		 * the actual name we are searching for. Typechecking comes later.
		 */
		status =
		    acpi_ns_search_one_scope(target_name, parent_node,
					     ACPI_TYPE_ANY, return_node);
		if (ACPI_SUCCESS(status)) {
			return_ACPI_STATUS(status);
		}

		/* Not found here, go up another level (until we reach the root) */

		parent_node = parent_node->parent;
	}

	/* Not found in parent tree */

	return_ACPI_STATUS(AE_NOT_FOUND);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_search_and_enter
 *
 * PARAMETERS:  target_name         - Ascii ACPI name to search for (4 chars)
 *              walk_state          - Current state of the walk
 *              node                - Starting node where search will begin
 *              interpreter_mode    - Add names only in ACPI_MODE_LOAD_PASS_x.
 *                                    Otherwise,search only.
 *              type                - Object type to match
 *              flags               - Flags describing the search restrictions
 *              return_node         - Where the Node is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Search for a name segment in a single namespace level,
 *              optionally adding it if it is not found. If the passed
 *              Type is not Any and the type previously stored in the
 *              entry was Any (i.e. unknown), update the stored type.
 *
 *              In ACPI_IMODE_EXECUTE, search only.
 *              In other modes, search and add if not found.
 *
 ******************************************************************************/
/**
 * acpi_ns_search_and_enter - 在指定命名空间层级搜索并可能创建新节点
 * @target_name: 要查找/创建的4字节ACPI名称（未打包格式）
 * @walk_state: 当前walk状态对象（包含解析上下文）
 * @node: 搜索起点的命名空间节点
 * @interpreter_mode: 解释器模式（AM_NO_MODE或AM_TABLE_LOAD）
 * @type: 要创建的对象类型（如果未找到）
 * @flags: 控制标志位（见下文）
 * @return_node: 返回找到/创建的节点指针
 *
 * 标志位选项：
 *   ACPI_NS_ERROR_IF_FOUND    - 如果节点已存在则返回错误
 *   ACPI_NS_OVERRIDE_IF_FOUND - 允许覆盖现有节点
 *   ACPI_NS_SEARCH_PARENT    - 在父级中继续搜索
 *   ACPI_NS_DONT_OPEN_SCOPE  - 不打开新作用域
 *   ACPI_NS_NO_PEER_SEARCH   - 不搜索同级节点
 *   ACPI_NS_PREFIX_IS_SCOPE  - 输入前缀包含作用域
 *
 * 返回值：
 *  AE_OK            - 成功找到/创建节点
 *  AE_BAD_PARAMETER - 无效参数
 *  AE_ALREADY_EXISTS- 节点已存在且设置了ERROR_IF_FOUND
 *  AE_NO_MEMORY     - 内存分配失败
 *  AE_NOT_FOUND     - 未找到节点且未创建新节点
 *
 * 上下文：
 *  - 必须持有ACPI_MTX_NAMESPACE互斥锁
 *  - 可能在表加载或运行时调用
 */
acpi_status
acpi_ns_search_and_enter(u32 target_name,
			 struct acpi_walk_state *walk_state,
			 struct acpi_namespace_node *node,
			 acpi_interpreter_mode interpreter_mode,
			 acpi_object_type type,
			 u32 flags, struct acpi_namespace_node **return_node)
{
	acpi_status status;
	struct acpi_namespace_node *new_node;//新创建的节点指针

	ACPI_FUNCTION_TRACE(ns_search_and_enter);

	/* Parameter validation */

	if (!node || !target_name || !return_node) {//参数验证
		ACPI_ERROR((AE_INFO,
			    "Null parameter: Node %p Name 0x%X ReturnNode %p",
			    node, target_name, return_node));
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/*
	 * Name must consist of valid ACPI characters. We will repair the name if
	 * necessary because we don't want to abort because of this, but we want
	 * all namespace names to be printable. A warning message is appropriate.
	 *
	 * This issue came up because there are in fact machines that exhibit
	 * this problem, and we want to be able to enable ACPI support for them,
	 * even though there are a few bad names.
	 */
	/*
	 * 名称必须由有效的ACPI字符组成。如果需要我们会修复名称，因为我们不希望因此
	 * 中止，但希望所有命名空间名称都可打印。这种情况下输出警告信息是合适的。
	 *
	 * 出现此问题是因为实际存在这种情况的机器，我们希望为它们启用ACPI支持，尽管
	 * 存在一些错误名称
	 * */
	acpi_ut_repair_name(ACPI_CAST_PTR(char, &target_name));//修复非法ACPI名称

	/* 首先尝试在调用者指定的命名空间层级查找名称 */
	*return_node = ACPI_ENTRY_NOT_FOUND;//初始化返回节点为"未找到"
	status = acpi_ns_search_one_scope(target_name, node, type, return_node);//在当前作用域中搜索目标名称
	if (status != AE_NOT_FOUND) {//检查搜索结果是否为"未找到"之外的状态
		/*
		 * If we found it AND the request specifies that a find is an error,
		 * return the error
		 * 如果找到节点 且 调用者指定"找到即报错"，则返回相应错误
		 */
		if (status == AE_OK) {//如果成功在命名空间找到该节点

			/* The node was found in the namespace */

			/*
			 * If the namespace override feature is enabled for this node,
			 * delete any existing attached sub-object and make the node
			 * look like a new node that is owned by the override table.
			 * 如果为此节点启用了命名空间覆盖功能（ACPI_NS_OVERRIDE_IF_FOUND），
			 * 请删除任何现有的附加子对象，并使该节点看起来像一个被覆盖表拥有的新节点。
			 */
			if (flags & ACPI_NS_OVERRIDE_IF_FOUND) {
				ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
						  "Namespace override: %4.4s pass %u type %X Owner %X\n",
						  ACPI_CAST_PTR(char,
								&target_name),
						  interpreter_mode,
						  (*return_node)->type,
						  walk_state->owner_id));

				acpi_ns_delete_children(*return_node);//删除该节点下的所有子节点
				if (acpi_gbl_runtime_namespace_override) {//检查是否为运行时命名空间覆盖
					acpi_ut_remove_reference((*return_node)->object);//移除原对象的引用计数
					(*return_node)->object = NULL;//清空对象指针
					(*return_node)->owner_id =
					    walk_state->owner_id;//更新节点所有者ID
				} else {//非运行时情况：直接移除整个节点
					acpi_ns_remove_node(*return_node);
					*return_node = ACPI_ENTRY_NOT_FOUND;//将返回节点标记为未找到
				}
			}

			/* Return an error if we don't expect to find the object */

			else if (flags & ACPI_NS_ERROR_IF_FOUND) {//如果设置了ERROR_IF_FOUND标志，返回"已存在"错误
				status = AE_ALREADY_EXISTS;
			}
		}
#ifdef ACPI_ASL_COMPILER
		/* 仅ACPI编译器环境下：处理外部标记的特殊情况 */
		if (*return_node && (*return_node)->type == ACPI_TYPE_ANY) {
			(*return_node)->flags |= ANOBJ_IS_EXTERNAL;//为ACPI_TYPE_ANY类型节点添加外部标记
		}
#endif

		/* Either found it or there was an error: finished either way */

		return_ACPI_STATUS(status);//无论找到节点还是出现错误，此时都可以直接返回状态
	}

	/*
	 * The name was not found. If we are NOT performing the first pass
	 * (name entry) of loading the namespace, search the parent tree (all the
	 * way to the root if necessary.) We don't want to perform the parent
	 * search when the namespace is actually being loaded. We want to perform
	 * the search when namespace references are being resolved (load pass 2)
	 * and during the execution phase.
	 * 未找到名称时的处理逻辑：
	 * 如果我们不在加载命名空间的第一阶段（名称录入阶段），则搜索父节点树（必要时一直搜索到根节点）。
	 * 我们不想在真正加载命名空间时执行父级搜索， 而是希望在解析命名空间引用时（加载第二阶段）
	 * 和执行阶段执行这种搜索。
	 */
	if ((interpreter_mode != ACPI_IMODE_LOAD_PASS1) &&//不是加载第一阶段
	    (flags & ACPI_NS_SEARCH_PARENT)) {//且允许搜索父节点
		/*
		 * Not found at this level - search parent tree according to the
		 * ACPI specification
		 * 当前层级未找到 - 根据ACPI规范搜索父节点树
		 */
		status =
		    acpi_ns_search_parent_tree(target_name, node, type,
					       return_node);//搜索父节点树
		if (ACPI_SUCCESS(status)) {
			return_ACPI_STATUS(status);//如果在父级中找到，直接返回
		}
	}

	/* In execute mode, just search, never add names. Exit now */
	/* 执行模式下的特殊处理：仅执行搜索操作，绝不添加新名称，立即退出 */
	if (interpreter_mode == ACPI_IMODE_EXECUTE) {
		ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
				  "%4.4s Not found in %p [Not adding]\n",
				  ACPI_CAST_PTR(char, &target_name), node));

		return_ACPI_STATUS(AE_NOT_FOUND);
	}

	/* Create the new named object */

	new_node = acpi_ns_create_node(target_name);//创建新的命名对象，分配新节点内存
	if (!new_node) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}
#ifdef ACPI_ASL_COMPILER

	/* Node is an object defined by an External() statement */
	/* ACPI编译器专用处理：标记由External()语句定义的外部对象 */
	if (flags & ACPI_NS_EXTERNAL ||//如果是显式外部标记
	    (walk_state && walk_state->opcode == AML_SCOPE_OP)) {//或SCOPE操作
		new_node->flags |= ANOBJ_IS_EXTERNAL;//设置外部标志位
	}
#endif
	/* 处理临时节点标记 */
	if (flags & ACPI_NS_TEMPORARY) {
		new_node->flags |= ANOBJ_TEMPORARY;//设置临时节点标志
	}

	/* Install the new object into the parent's list of children */
	/* 将新节点安装到父节点的子节点列表中 */
	acpi_ns_install_node(walk_state, node, new_node, type);
	*return_node = new_node;//通过参数返回新节点
	return_ACPI_STATUS(AE_OK);//返回成功状态
}
