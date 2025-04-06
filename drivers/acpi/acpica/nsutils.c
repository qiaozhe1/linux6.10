// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nsutils - Utilities for accessing ACPI namespace, accessing
 *                        parents and siblings and Scope manipulation
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "amlcode.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsutils")

/* Local prototypes */
#ifdef ACPI_OBSOLETE_FUNCTIONS
acpi_name acpi_ns_find_parent_name(struct acpi_namespace_node *node_to_search);
#endif

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_print_node_pathname
 *
 * PARAMETERS:  node            - Object
 *              message         - Prefix message
 *
 * DESCRIPTION: Print an object's full namespace pathname
 *              Manages allocation/freeing of a pathname buffer
 *
 ******************************************************************************/

void
acpi_ns_print_node_pathname(struct acpi_namespace_node *node,
			    const char *message)
{
	struct acpi_buffer buffer;
	acpi_status status;

	if (!node) {
		acpi_os_printf("[NULL NAME]");
		return;
	}

	/* Convert handle to full pathname and print it (with supplied message) */

	buffer.length = ACPI_ALLOCATE_LOCAL_BUFFER;

	status = acpi_ns_handle_to_pathname(node, &buffer, TRUE);
	if (ACPI_SUCCESS(status)) {
		if (message) {
			acpi_os_printf("%s ", message);
		}

		acpi_os_printf("%s", (char *)buffer.pointer);
		ACPI_FREE(buffer.pointer);
	}
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_type
 *
 * PARAMETERS:  node        - Parent Node to be examined
 *
 * RETURN:      Type field from Node whose handle is passed
 *
 * DESCRIPTION: Return the type of a Namespace node
 *
 ******************************************************************************/

acpi_object_type acpi_ns_get_type(struct acpi_namespace_node * node)
{
	ACPI_FUNCTION_TRACE(ns_get_type);

	if (!node) {
		ACPI_WARNING((AE_INFO, "Null Node parameter"));
		return_UINT8(ACPI_TYPE_ANY);
	}

	return_UINT8(node->type);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_local
 *
 * PARAMETERS:  type        - A namespace object type
 *
 * RETURN:      LOCAL if names must be found locally in objects of the
 *              passed type, 0 if enclosing scopes should be searched
 *
 * DESCRIPTION: Returns scope rule for the given object type.
 *
 ******************************************************************************/

u32 acpi_ns_local(acpi_object_type type)
{
	ACPI_FUNCTION_TRACE(ns_local);

	if (!acpi_ut_valid_object_type(type)) {

		/* Type code out of range  */

		ACPI_WARNING((AE_INFO, "Invalid Object Type 0x%X", type));
		return_UINT32(ACPI_NS_NORMAL);
	}

	return_UINT32(acpi_gbl_ns_properties[type] & ACPI_NS_LOCAL);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_internal_name_length
 *
 * PARAMETERS:  info            - Info struct initialized with the
 *                                external name pointer.
 *
 * RETURN:      None
 *
 * DESCRIPTION: Calculate the length of the internal (AML) namestring
 *              corresponding to the external (ASL) namestring.
 *
 ******************************************************************************/

void acpi_ns_get_internal_name_length(struct acpi_namestring_info *info)
{
	const char *next_external_char;
	u32 i;

	ACPI_FUNCTION_ENTRY();

	next_external_char = info->external_name;
	info->num_carats = 0;
	info->num_segments = 0;
	info->fully_qualified = FALSE;

	/*
	 * For the internal name, the required length is 4 bytes per segment,
	 * plus 1 each for root_prefix, multi_name_prefix_op, segment count,
	 * trailing null (which is not really needed, but no there's harm in
	 * putting it there)
	 *
	 * strlen() + 1 covers the first name_seg, which has no path separator
	 */
	if (ACPI_IS_ROOT_PREFIX(*next_external_char)) {
		info->fully_qualified = TRUE;
		next_external_char++;

		/* Skip redundant root_prefix, like \\_SB.PCI0.SBRG.EC0 */

		while (ACPI_IS_ROOT_PREFIX(*next_external_char)) {
			next_external_char++;
		}
	} else {
		/* Handle Carat prefixes */

		while (ACPI_IS_PARENT_PREFIX(*next_external_char)) {
			info->num_carats++;
			next_external_char++;
		}
	}

	/*
	 * Determine the number of ACPI name "segments" by counting the number of
	 * path separators within the string. Start with one segment since the
	 * segment count is [(# separators) + 1], and zero separators is ok.
	 */
	if (*next_external_char) {
		info->num_segments = 1;
		for (i = 0; next_external_char[i]; i++) {
			if (ACPI_IS_PATH_SEPARATOR(next_external_char[i])) {
				info->num_segments++;
			}
		}
	}

	info->length = (ACPI_NAMESEG_SIZE * info->num_segments) +
	    4 + info->num_carats;

	info->next_external_char = next_external_char;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_build_internal_name
 *
 * PARAMETERS:  info            - Info struct fully initialized
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Construct the internal (AML) namestring
 *              corresponding to the external (ASL) namestring.
 *
 ******************************************************************************/

acpi_status acpi_ns_build_internal_name(struct acpi_namestring_info *info)
{
	u32 num_segments = info->num_segments;
	char *internal_name = info->internal_name;
	const char *external_name = info->next_external_char;
	char *result = NULL;
	u32 i;

	ACPI_FUNCTION_TRACE(ns_build_internal_name);

	/* Setup the correct prefixes, counts, and pointers */

	if (info->fully_qualified) {
		internal_name[0] = AML_ROOT_PREFIX;

		if (num_segments <= 1) {
			result = &internal_name[1];
		} else if (num_segments == 2) {
			internal_name[1] = AML_DUAL_NAME_PREFIX;
			result = &internal_name[2];
		} else {
			internal_name[1] = AML_MULTI_NAME_PREFIX;
			internal_name[2] = (char)num_segments;
			result = &internal_name[3];
		}
	} else {
		/*
		 * Not fully qualified.
		 * Handle Carats first, then append the name segments
		 */
		i = 0;
		if (info->num_carats) {
			for (i = 0; i < info->num_carats; i++) {
				internal_name[i] = AML_PARENT_PREFIX;
			}
		}

		if (num_segments <= 1) {
			result = &internal_name[i];
		} else if (num_segments == 2) {
			internal_name[i] = AML_DUAL_NAME_PREFIX;
			result = &internal_name[(acpi_size)i + 1];
		} else {
			internal_name[i] = AML_MULTI_NAME_PREFIX;
			internal_name[(acpi_size)i + 1] = (char)num_segments;
			result = &internal_name[(acpi_size)i + 2];
		}
	}

	/* Build the name (minus path separators) */

	for (; num_segments; num_segments--) {
		for (i = 0; i < ACPI_NAMESEG_SIZE; i++) {
			if (ACPI_IS_PATH_SEPARATOR(*external_name) ||
			    (*external_name == 0)) {

				/* Pad the segment with underscore(s) if segment is short */

				result[i] = '_';
			} else {
				/* Convert the character to uppercase and save it */

				result[i] = (char)toupper((int)*external_name);
				external_name++;
			}
		}

		/* Now we must have a path separator, or the pathname is bad */

		if (!ACPI_IS_PATH_SEPARATOR(*external_name) &&
		    (*external_name != 0)) {
			return_ACPI_STATUS(AE_BAD_PATHNAME);
		}

		/* Move on the next segment */

		external_name++;
		result += ACPI_NAMESEG_SIZE;
	}

	/* Terminate the string */

	*result = 0;

	if (info->fully_qualified) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "Returning [%p] (abs) \"\\%s\"\n",
				  internal_name, internal_name));
	} else {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Returning [%p] (rel) \"%s\"\n",
				  internal_name, internal_name));
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_internalize_name
 *
 * PARAMETERS:  *external_name          - External representation of name
 *              **Converted name        - Where to return the resulting
 *                                        internal represention of the name
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Convert an external representation (e.g. "\_PR_.CPU0")
 *              to internal form (e.g. 5c 2f 02 5f 50 52 5f 43 50 55 30)
 *
 *******************************************************************************/

acpi_status
acpi_ns_internalize_name(const char *external_name, char **converted_name)
{
	char *internal_name;
	struct acpi_namestring_info info;
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_internalize_name);

	if ((!external_name) || (*external_name == 0) || (!converted_name)) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* Get the length of the new internal name */

	info.external_name = external_name;
	acpi_ns_get_internal_name_length(&info);

	/* We need a segment to store the internal  name */

	internal_name = ACPI_ALLOCATE_ZEROED(info.length);
	if (!internal_name) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Build the name */

	info.internal_name = internal_name;
	status = acpi_ns_build_internal_name(&info);
	if (ACPI_FAILURE(status)) {
		ACPI_FREE(internal_name);
		return_ACPI_STATUS(status);
	}

	*converted_name = internal_name;
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_externalize_name
 *
 * PARAMETERS:  internal_name_length - Length of the internal name below
 *              internal_name       - Internal representation of name
 *              converted_name_length - Where the length is returned
 *              converted_name      - Where the resulting external name
 *                                    is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Convert internal name (e.g. 5c 2f 02 5f 50 52 5f 43 50 55 30)
 *              to its external (printable) form (e.g. "\_PR_.CPU0")
 *
 ******************************************************************************/

acpi_status
acpi_ns_externalize_name(u32 internal_name_length,
			 const char *internal_name,
			 u32 * converted_name_length, char **converted_name)
{
	u32 names_index = 0;
	u32 num_segments = 0;
	u32 required_length;
	u32 prefix_length = 0;
	u32 i = 0;
	u32 j = 0;

	ACPI_FUNCTION_TRACE(ns_externalize_name);

	if (!internal_name_length || !internal_name || !converted_name) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* Check for a prefix (one '\' | one or more '^') */

	switch (internal_name[0]) {
	case AML_ROOT_PREFIX:

		prefix_length = 1;
		break;

	case AML_PARENT_PREFIX:

		for (i = 0; i < internal_name_length; i++) {
			if (ACPI_IS_PARENT_PREFIX(internal_name[i])) {
				prefix_length = i + 1;
			} else {
				break;
			}
		}

		if (i == internal_name_length) {
			prefix_length = i;
		}

		break;

	default:

		break;
	}

	/*
	 * Check for object names. Note that there could be 0-255 of these
	 * 4-byte elements.
	 */
	if (prefix_length < internal_name_length) {
		switch (internal_name[prefix_length]) {
		case AML_MULTI_NAME_PREFIX:

			/* <count> 4-byte names */

			names_index = prefix_length + 2;
			num_segments = (u8)
			    internal_name[(acpi_size)prefix_length + 1];
			break;

		case AML_DUAL_NAME_PREFIX:

			/* Two 4-byte names */

			names_index = prefix_length + 1;
			num_segments = 2;
			break;

		case 0:

			/* null_name */

			names_index = 0;
			num_segments = 0;
			break;

		default:

			/* one 4-byte name */

			names_index = prefix_length;
			num_segments = 1;
			break;
		}
	}

	/*
	 * Calculate the length of converted_name, which equals the length
	 * of the prefix, length of all object names, length of any required
	 * punctuation ('.') between object names, plus the NULL terminator.
	 */
	required_length = prefix_length + (4 * num_segments) +
	    ((num_segments > 0) ? (num_segments - 1) : 0) + 1;

	/*
	 * Check to see if we're still in bounds. If not, there's a problem
	 * with internal_name (invalid format).
	 */
	if (required_length > internal_name_length) {
		ACPI_ERROR((AE_INFO, "Invalid internal name"));
		return_ACPI_STATUS(AE_BAD_PATHNAME);
	}

	/* Build the converted_name */

	*converted_name = ACPI_ALLOCATE_ZEROED(required_length);
	if (!(*converted_name)) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	j = 0;

	for (i = 0; i < prefix_length; i++) {
		(*converted_name)[j++] = internal_name[i];
	}

	if (num_segments > 0) {
		for (i = 0; i < num_segments; i++) {
			if (i > 0) {
				(*converted_name)[j++] = '.';
			}

			/* Copy and validate the 4-char name segment */

			ACPI_COPY_NAMESEG(&(*converted_name)[j],
					  &internal_name[names_index]);
			acpi_ut_repair_name(&(*converted_name)[j]);

			j += ACPI_NAMESEG_SIZE;
			names_index += ACPI_NAMESEG_SIZE;
		}
	}

	if (converted_name_length) {
		*converted_name_length = (u32) required_length;
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_validate_handle
 *
 * PARAMETERS:  handle          - Handle to be validated and typecast to a
 *                                namespace node.
 *
 * RETURN:      A pointer to a namespace node
 *
 * DESCRIPTION: Convert a namespace handle to a namespace node. Handles special
 *              cases for the root node.
 *
 * NOTE: Real integer handles would allow for more verification
 *       and keep all pointers within this subsystem - however this introduces
 *       more overhead and has not been necessary to this point. Drivers
 *       holding handles are typically notified before a node becomes invalid
 *       due to a table unload.
 *
 ******************************************************************************/
/**
 * acpi_ns_validate_handle - 验证并转换ACPI句柄为命名空间节点
 * @handle: 要验证的ACPI句柄
 *
 * 功能说明:
 * 1. 处理特殊根节点情况
 * 2. 验证句柄描述符类型
 * 3. 安全转换句柄为节点指针
 */
struct acpi_namespace_node *acpi_ns_validate_handle(acpi_handle handle)
{

	ACPI_FUNCTION_ENTRY();

	/* Parameter validation */

	if ((!handle) || (handle == ACPI_ROOT_OBJECT)) {//检查是否为NULL句柄或根对象
		return (acpi_gbl_root_node);//返回全局根节点指针
	}

	/* We can at least attempt to verify the handle */

	if (ACPI_GET_DESCRIPTOR_TYPE(handle) != ACPI_DESC_TYPE_NAMED) {//检查描述符类型是否为命名节点(ACPI_DESC_TYPE_NAMED)
		return (NULL);
	}

	return (ACPI_CAST_PTR(struct acpi_namespace_node, handle));//安全类型转换并返回节点指针
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_terminate
 *
 * PARAMETERS:  none
 *
 * RETURN:      none
 *
 * DESCRIPTION: free memory allocated for namespace and ACPI table storage.
 *
 ******************************************************************************/

void acpi_ns_terminate(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_terminate);

	/*
	 * Free the entire namespace -- all nodes and all objects
	 * attached to the nodes
	 */
	acpi_ns_delete_namespace_subtree(acpi_gbl_root_node);

	/* Delete any objects attached to the root node */

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_VOID;
	}

	acpi_ns_delete_node(acpi_gbl_root_node);
	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Namespace freed\n"));
	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_opens_scope
 *
 * PARAMETERS:  type        - A valid namespace type
 *
 * RETURN:      NEWSCOPE if the passed type "opens a name scope" according
 *              to the ACPI specification, else 0
 *
 ******************************************************************************/

u32 acpi_ns_opens_scope(acpi_object_type type)
{
	ACPI_FUNCTION_ENTRY();

	if (type > ACPI_TYPE_LOCAL_MAX) {

		/* type code out of range  */

		ACPI_WARNING((AE_INFO, "Invalid Object Type 0x%X", type));
		return (ACPI_NS_NORMAL);
	}

	return (((u32)acpi_gbl_ns_properties[type]) & ACPI_NS_NEWSCOPE);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_node_unlocked
 *
 * PARAMETERS:  *pathname   - Name to be found, in external (ASL) format. The
 *                            \ (backslash) and ^ (carat) prefixes, and the
 *                            . (period) to separate segments are supported.
 *              prefix_node  - Root of subtree to be searched, or NS_ALL for the
 *                            root of the name space. If Name is fully
 *                            qualified (first s8 is '\'), the passed value
 *                            of Scope will not be accessed.
 *              flags       - Used to indicate whether to perform upsearch or
 *                            not.
 *              return_node - Where the Node is returned
 *
 * DESCRIPTION: Look up a name relative to a given scope and return the
 *              corresponding Node. NOTE: Scope can be null.
 *
 * MUTEX:       Doesn't locks namespace
 *
 ******************************************************************************/
/**
 * acpi_ns_get_node_unlocked - 命名空间节点查找核心函数
 * @prefix_node: 搜索起始节点（NULL表示从根节点开始）
 * @pathname: 目标节点路径字符串
 * @flags: 控制搜索行为的标志位
 * @return_node: 返回找到的节点指针
 *
 * 功能说明：
 * 1. 处理空路径和根节点快捷路径
 * 2. 将外部路径格式转换为内部表示
 * 3. 执行命名空间查找
 * 4. 清理资源并返回状态
 */
acpi_status
acpi_ns_get_node_unlocked(struct acpi_namespace_node *prefix_node,
			  const char *pathname,
			  u32 flags, struct acpi_namespace_node **return_node)
{
	union acpi_generic_state scope_info;//查找作用域信息
	acpi_status status;
	char *internal_path;//内部格式路径字符串

	ACPI_FUNCTION_TRACE_PTR(ns_get_node_unlocked,
				ACPI_CAST_PTR(char, pathname));//调试跟踪（记录pathname参数）

	/* Simplest case is a null pathname */

	if (!pathname) {//如果是空路径名（直接返回prefix_node或根节点）
		*return_node = prefix_node;
		if (!prefix_node) {
			*return_node = acpi_gbl_root_node;
		}

		return_ACPI_STATUS(AE_OK);
	}

	/* Quick check for a reference to the root */
	/* 根节点快捷引用（单独处理"\"路径） */
	if (ACPI_IS_ROOT_PREFIX(pathname[0]) && (!pathname[1])) {// 检测首字符是否为'\'并且确认是单字符路径
		*return_node = acpi_gbl_root_node;//返回跟节点
		return_ACPI_STATUS(AE_OK);
	}

	/* Convert path to internal representation */

	status = acpi_ns_internalize_name(pathname, &internal_path);//路径格式转换（外部->内部）
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);//转换失败直接返回
	}

	/* Setup lookup scope (search starting point) */

	scope_info.scope.node = prefix_node;//初始化搜索起点

	/* Lookup the name in the namespace */

	status = acpi_ns_lookup(&scope_info, internal_path, ACPI_TYPE_ANY,
				ACPI_IMODE_EXECUTE,
				(flags | ACPI_NS_DONT_OPEN_SCOPE), NULL,
				return_node);//执行命名空间查找
	if (ACPI_FAILURE(status)) {//错误处理（带调试输出）
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "%s, %s\n",
				  pathname, acpi_format_exception(status)));
	}

	ACPI_FREE(internal_path);//释放内部路径缓冲区
	return_ACPI_STATUS(status);//返回最终状态
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_node
 *
 * PARAMETERS:  *pathname   - Name to be found, in external (ASL) format. The
 *                            \ (backslash) and ^ (carat) prefixes, and the
 *                            . (period) to separate segments are supported.
 *              prefix_node  - Root of subtree to be searched, or NS_ALL for the
 *                            root of the name space. If Name is fully
 *                            qualified (first s8 is '\'), the passed value
 *                            of Scope will not be accessed.
 *              flags       - Used to indicate whether to perform upsearch or
 *                            not.
 *              return_node - Where the Node is returned
 *
 * DESCRIPTION: Look up a name relative to a given scope and return the
 *              corresponding Node. NOTE: Scope can be null.
 *
 * MUTEX:       Locks namespace
 *
 ******************************************************************************/
/**
 * acpi_ns_get_node - 获取指定路径的命名空间节点
 * @prefix_node: 搜索起始节点（若为NULL则从根节点开始）
 * @pathname: 目标节点的路径名（绝对或相对路径）
 * @flags: 控制搜索行为的标志位
 * @return_node: 返回找到的节点指针
 *
 * 功能说明:
 * 1. 获取命名空间互斥锁保证线程安全
 * 2. 调用内部函数执行实际搜索
 * 3. 释放互斥锁并返回状态
 */
acpi_status
acpi_ns_get_node(struct acpi_namespace_node *prefix_node,
		 const char *pathname,
		 u32 flags, struct acpi_namespace_node **return_node)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE_PTR(ns_get_node, ACPI_CAST_PTR(char, pathname));

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);// 获取命名空间互斥锁（防止并发修改命名空间）
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/*
    	 * 核心操作：执行实际搜索
    	 * 参数说明：
    	 * - prefix_node: 相对路径的解析起点
    	 * - pathname: 待解析的路径字符串
    	 * - flags: 控制搜索行为的标志组合，例如：
    	 *   * ACPI_NS_NO_UPSEARCH - 禁止向上搜索
    	 *   * ACPI_NS_DONT_OPEN_SCOPE - 不打开新作用域
    	 *   * ACPI_NS_ERROR_IF_FOUND - 已存在时报错
    	 */
	status = acpi_ns_get_node_unlocked(prefix_node, pathname,
					   flags, return_node);//执行实际搜索

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);//释放命名空间互斥锁（必须与获取锁配对）
	return_ACPI_STATUS(status);//返回状态码
}
