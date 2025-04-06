// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: nsaccess - Top-level functions for accessing ACPI namespace
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "amlcode.h"
#include "acnamesp.h"
#include "acdispat.h"

#ifdef ACPI_ASL_COMPILER
#include "acdisasm.h"
#endif

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsaccess")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_root_initialize
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Allocate and initialize the default root named objects
 *
 * MUTEX:       Locks namespace for entire execution
 *
 ******************************************************************************/
acpi_status acpi_ns_root_initialize(void)
{
	acpi_status status;
	const struct acpi_predefined_names *init_val = NULL;
	struct acpi_namespace_node *new_node;
	struct acpi_namespace_node *prev_node = NULL;
	union acpi_operand_object *obj_desc;
	acpi_string val = NULL;

	ACPI_FUNCTION_TRACE(ns_root_initialize);

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/*
	 * The global root ptr is initially NULL, so a non-NULL value indicates
	 * that acpi_ns_root_initialize() has already been called; just return.
	 */
	if (acpi_gbl_root_node) {
		status = AE_OK;
		goto unlock_and_exit;
	}

	/*
	 * Tell the rest of the subsystem that the root is initialized
	 * (This is OK because the namespace is locked)
	 */
	acpi_gbl_root_node = &acpi_gbl_root_node_struct;

	/* Enter the predefined names in the name table */

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "Entering predefined entries into namespace\n"));

	/*
	 * Create the initial (default) namespace.
	 * This namespace looks like something similar to this:
	 *
	 *   ACPI Namespace (from Namespace Root):
	 *    0  _GPE Scope        00203160 00
	 *    0  _PR_ Scope        002031D0 00
	 *    0  _SB_ Device       00203240 00 Notify Object: 0020ADD8
	 *    0  _SI_ Scope        002032B0 00
	 *    0  _TZ_ Device       00203320 00
	 *    0  _REV Integer      00203390 00 = 0000000000000002
	 *    0  _OS_ String       00203488 00 Len 14 "Microsoft Windows NT"
	 *    0  _GL_ Mutex        00203580 00 Object 002035F0
	 *    0  _OSI Method       00203678 00 Args 1 Len 0000 Aml 00000000
	 */
	for (init_val = acpi_gbl_pre_defined_names; init_val->name; init_val++) {
		status = AE_OK;

		/* _OSI is optional for now, will be permanent later */

		if (!strcmp(init_val->name, "_OSI")
		    && !acpi_gbl_create_osi_method) {
			continue;
		}

		/*
		 * Create, init, and link the new predefined name
		 * Note: No need to use acpi_ns_lookup here because all the
		 * predefined names are at the root level. It is much easier to
		 * just create and link the new node(s) here.
		 */
		new_node =
		    acpi_ns_create_node(*ACPI_CAST_PTR(u32, init_val->name));
		if (!new_node) {
			status = AE_NO_MEMORY;
			goto unlock_and_exit;
		}

		new_node->descriptor_type = ACPI_DESC_TYPE_NAMED;
		new_node->type = init_val->type;

		if (!prev_node) {
			acpi_gbl_root_node_struct.child = new_node;
		} else {
			prev_node->peer = new_node;
		}

		new_node->parent = &acpi_gbl_root_node_struct;
		prev_node = new_node;

		/*
		 * Name entered successfully. If entry in pre_defined_names[] specifies
		 * an initial value, create the initial value.
		 */
		if (init_val->val) {
			status = acpi_os_predefined_override(init_val, &val);
			if (ACPI_FAILURE(status)) {
				ACPI_ERROR((AE_INFO,
					    "Could not override predefined %s",
					    init_val->name));
			}

			if (!val) {
				val = init_val->val;
			}

			/*
			 * Entry requests an initial value, allocate a
			 * descriptor for it.
			 */
			obj_desc =
			    acpi_ut_create_internal_object(init_val->type);
			if (!obj_desc) {
				status = AE_NO_MEMORY;
				goto unlock_and_exit;
			}

			/*
			 * Convert value string from table entry to
			 * internal representation. Only types actually
			 * used for initial values are implemented here.
			 */
			switch (init_val->type) {
			case ACPI_TYPE_METHOD:

				obj_desc->method.param_count =
				    (u8) ACPI_TO_INTEGER(val);
				obj_desc->common.flags |= AOPOBJ_DATA_VALID;

#if defined (ACPI_ASL_COMPILER)

				/* Save the parameter count for the iASL compiler */

				new_node->value = obj_desc->method.param_count;
#else
				/* Mark this as a very SPECIAL method (_OSI) */

				obj_desc->method.info_flags =
				    ACPI_METHOD_INTERNAL_ONLY;
				obj_desc->method.dispatch.implementation =
				    acpi_ut_osi_implementation;
#endif
				break;

			case ACPI_TYPE_INTEGER:

				obj_desc->integer.value = ACPI_TO_INTEGER(val);
				break;

			case ACPI_TYPE_STRING:

				/* Build an object around the static string */

				obj_desc->string.length = (u32)strlen(val);
				obj_desc->string.pointer = val;
				obj_desc->common.flags |= AOPOBJ_STATIC_POINTER;
				break;

			case ACPI_TYPE_MUTEX:

				obj_desc->mutex.node = new_node;
				obj_desc->mutex.sync_level =
				    (u8) (ACPI_TO_INTEGER(val) - 1);

				/* Create a mutex */

				status =
				    acpi_os_create_mutex(&obj_desc->mutex.
							 os_mutex);
				if (ACPI_FAILURE(status)) {
					acpi_ut_remove_reference(obj_desc);
					goto unlock_and_exit;
				}

				/* Special case for ACPI Global Lock */

				if (strcmp(init_val->name, "_GL_") == 0) {
					acpi_gbl_global_lock_mutex = obj_desc;

					/* Create additional counting semaphore for global lock */

					status =
					    acpi_os_create_semaphore(1, 0,
								     &acpi_gbl_global_lock_semaphore);
					if (ACPI_FAILURE(status)) {
						acpi_ut_remove_reference
						    (obj_desc);
						goto unlock_and_exit;
					}
				}
				break;

			default:

				ACPI_ERROR((AE_INFO,
					    "Unsupported initial type value 0x%X",
					    init_val->type));
				acpi_ut_remove_reference(obj_desc);
				obj_desc = NULL;
				continue;
			}

			/* Store pointer to value descriptor in the Node */

			status = acpi_ns_attach_object(new_node, obj_desc,
						       obj_desc->common.type);

			/* Remove local reference to the object */

			acpi_ut_remove_reference(obj_desc);
		}
	}

unlock_and_exit:
	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);

	/* Save a handle to "_GPE", it is always present */

	if (ACPI_SUCCESS(status)) {
		status = acpi_ns_get_node(NULL, "\\_GPE", ACPI_NS_NO_UPSEARCH,
					  &acpi_gbl_fadt_gpe_device);
	}

	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_lookup
 *
 * PARAMETERS:  scope_info      - Current scope info block
 *              pathname        - Search pathname, in internal format
 *                                (as represented in the AML stream)
 *              type            - Type associated with name
 *              interpreter_mode - IMODE_LOAD_PASS2 => add name if not found
 *              flags           - Flags describing the search restrictions
 *              walk_state      - Current state of the walk
 *              return_node     - Where the Node is placed (if found
 *                                or created successfully)
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Find or enter the passed name in the name space.
 *              Log an error if name not found in Exec mode.
 *
 * MUTEX:       Assumes namespace is locked.
 *
 ******************************************************************************/
/**
 * acpi_ns_lookup - 命名空间查找核心函数
 * @scope_info: 作用域信息（包含搜索起点节点）
 * @pathname: 要查找的路径名（内部格式）
 * @type: 期望的节点类型
 * @interpreter_mode: 解释器模式（加载/执行）
 * @flags: 控制标志位（ACPI_NS_*系列标志）
 * @walk_state: 当前walk状态（可为NULL）
 * @return_node: 返回找到的节点
 *
 * 功能说明：
 * 1. 解析路径名中的前缀（根/父级）
 * 2. 确定路径段数量
 * 3. 逐段搜索命名空间
 * 4. 处理类型检查和作用域管理
 */
acpi_status
acpi_ns_lookup(union acpi_generic_state *scope_info,
	       char *pathname,
	       acpi_object_type type,
	       acpi_interpreter_mode interpreter_mode,
	       u32 flags,
	       struct acpi_walk_state *walk_state,
	       struct acpi_namespace_node **return_node)
{
	acpi_status status;
	char *path = pathname;//当前解析位置指针
	char *external_path;//外部格式路径（用于错误报告）
	struct acpi_namespace_node *prefix_node;//实际搜索起点节点
	struct acpi_namespace_node *current_node = NULL;//当前处理节点
	struct acpi_namespace_node *this_node = NULL;//临时节点指针
	u32 num_segments;//路径段数
	u32 num_carats;//父级前缀(^)计数
	acpi_name simple_name;//当前处理的4字节名称段
	acpi_object_type type_to_check_for;//保存原始类型参数
	acpi_object_type this_search_type;//当前搜索类型
	u32 search_parent_flag = ACPI_NS_SEARCH_PARENT;//父级搜索标志
	u32 local_flags;//处理后的本地标志
	acpi_interpreter_mode local_interpreter_mode;//适配后的解释器模式

	ACPI_FUNCTION_TRACE(ns_lookup);

	if (!return_node) {//必须提供有效的返回节点指针
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

    	/* 
    	 * 初始化本地标志位：
    	 * 保留原始标志位，但清除以下控制位：
    	 * - ACPI_NS_ERROR_IF_FOUND (0x0004)
    	 * - ACPI_NS_OVERRIDE_IF_FOUND (0x0008)
    	 * - ACPI_NS_SEARCH_PARENT (0x0001)
    	 * 这些标志将在后续处理中按需设置
    	 */
	local_flags = flags &
	    ~(ACPI_NS_ERROR_IF_FOUND | ACPI_NS_OVERRIDE_IF_FOUND |
	      ACPI_NS_SEARCH_PARENT);
	*return_node = ACPI_ENTRY_NOT_FOUND;//初始化返回节点为无效值
	acpi_gbl_ns_lookup_count++;//全局查找计数器递增（用于性能分析）

	if (!acpi_gbl_root_node) {//命名空间根节点检查
	        /* 
        	 * 命名空间未初始化时的处理：
        	 * - 通常在ACPI初始化早期调用时发生
        	 * - 返回错误码0x0005（AE_NO_NAMESPACE）
        	 */
		return_ACPI_STATUS(AE_NO_NAMESPACE);
	}

	/* Get the prefix scope. A null scope means use the root scope */
	/* 确定搜索的起始节点（前缀节点） */
	if ((!scope_info) || (!scope_info->scope.node)) {//当scope_info为空或其node字段为空时，使用根节点作为起点
		ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
				  "Null scope prefix, using root node (%p)\n",
				  acpi_gbl_root_node));

		prefix_node = acpi_gbl_root_node;
	} else {
		prefix_node = scope_info->scope.node;//否则使用scope_info中提供的节点
		if (ACPI_GET_DESCRIPTOR_TYPE(prefix_node) !=
		    ACPI_DESC_TYPE_NAMED) {//节点描述符类型验证（必须为命名节点）
			ACPI_ERROR((AE_INFO, "%p is not a namespace node [%s]",
				    prefix_node,
				    acpi_ut_get_descriptor_name(prefix_node)));
			return_ACPI_STATUS(AE_AML_INTERNAL); //返回AML内部错误(0x000A)
		}

		/* 
   		 * 作用域节点自动修正：
    		 * 当未设置ACPI_NS_PREFIX_IS_SCOPE标志时，
    		 * 需要确保prefix_node是有效的作用域节点
    		 */
		if (!(flags & ACPI_NS_PREFIX_IS_SCOPE)) {
			/*
			 * This node might not be a actual "scope" node (such as a
			 * Device/Method, etc.)  It could be a Package or other object
			 * node. Backup up the tree to find the containing scope node.
			 */
			while (!acpi_ns_opens_scope(prefix_node->type) &&//检查当前节点是否开启作用域
			       prefix_node->type != ACPI_TYPE_ANY) {//类型检查终止条件
				prefix_node = prefix_node->parent;//移动到父节点 
			}
		}
	}

	/* Save type. TBD: may be no longer necessary */
	/*  保存原始类型参数（待确定是否仍需保留） */
	type_to_check_for = type;

	/*
	 * Begin examination of the actual pathname
	 * 开始解析路径名：
	 * 分为空路径、绝对路径（以'\'开头）和相对路径三种情况
	 */
	if (!pathname) {//如果是空路径，默认指向根节点

		/* A Null name_path is allowed and refers to the root */

		num_segments = 0;//路径段数为0
		this_node = acpi_gbl_root_node;//设为全局根节点
		path = "";//空路径指针

		ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
				  "Null Pathname (Zero segments), Flags=%X\n",
				  flags));
	} else {
		/*
		 * Name pointer is valid (and must be in internal name format)
		 *
		 * Check for scope prefixes:
		 *
		 * As represented in the AML stream, a namepath consists of an
		 * optional scope prefix followed by a name segment part.
		 *
		 * If present, the scope prefix is either a Root Prefix (in
		 * which case the name is fully qualified), or one or more
		 * Parent Prefixes (in which case the name's scope is relative
		 * to the current scope).
		 * 有效路径名处理（必须为AML内部格式）：
		 * 路径组成 = [作用域前缀] + 名称段部分
		 * 作用域前缀可以是：
		 * - 根前缀（\）表示绝对路径
		 * - 父前缀（^）表示相对上级作用域
		 */
		if (*path == (u8) AML_ROOT_PREFIX) {// 绝对路径处理（从根节点开始）

			/* Pathname is fully qualified, start from the root */

			this_node = acpi_gbl_root_node;//设为根节点
			search_parent_flag = ACPI_NS_NO_UPSEARCH;//禁用向上搜索

			/* Point to name segment part */

			path++;//跳过根前缀字符

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Path is absolute from root [%p]\n",
					  this_node));
		} else {//相对路径处理（从当前作用域开始）
			/* Pathname is relative to current scope, start there */

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Searching relative to prefix scope [%4.4s] (%p)\n",
					  acpi_ut_get_node_name(prefix_node),
					  prefix_node));

			/*
			 * Handle multiple Parent Prefixes (carat) by just getting
			 * the parent node for each prefix instance.
			 *
			 * 处理父前缀（^）: 每遇到一个^就向上一级作用域
			 */
			this_node = prefix_node;//从prefix_node开始
			num_carats = 0;//父前缀计数器
			while (*path == (u8) AML_PARENT_PREFIX) {//循环直到不是父前缀

				/* Name is fully qualified, no search rules apply */

				search_parent_flag = ACPI_NS_NO_UPSEARCH;//有父前缀时禁用向上搜索

				/*
				 * Point past this prefix to the name segment
				 * part or the next Parent Prefix
				 */
				path++;//跳过当前父级前缀字符'^'，移动路径指针到下一个字符

				/* Backup to the parent node */

				num_carats++;//父前缀计数，录遇到的'^'数量，用于调试和验证
				this_node = this_node->parent;//将当前节点指向其父节点
				if (!this_node) {//如果父节点不存在
					/*
					 * Current scope has no parent scope. Externalize
					 * the internal path for error message.
					 */
					status =
					    acpi_ns_externalize_name
					    (ACPI_UINT32_MAX, pathname, NULL,
					     &external_path);//将AML内部路径转换为外部格式（用于错误消息）
					if (ACPI_SUCCESS(status)) {//如果转换成功，输出错误信息
						ACPI_ERROR((AE_INFO,
							    "%s: Path has too many parent prefixes (^)",
							    external_path));

						ACPI_FREE(external_path);//释放转换后的路径字符串
					}

					return_ACPI_STATUS(AE_NOT_FOUND);//返回"未找到"错误(0x0005)
				}
			}

			if (search_parent_flag == ACPI_NS_NO_UPSEARCH) {//当搜索父级标志被设置为ACPI_NS_NO_UPSEARCH时，输出当前搜索作用域和父前缀数量的调试信息
				ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
						  "Search scope is [%4.4s], path has %u carat(s)\n",
						  acpi_ut_get_node_name
						  (this_node), num_carats));
			}
		}

		/*
		 * 路径段数解析逻辑：
		 * 根据AML规范确定路径中包含的名称段数量
		 * 
		 * AML路径名格式规范：
		 * 1. 空段 (0x00)  - 0个名称段
		 * 2. 双名前缀 (0x2E) - 2个名称段
		 * 3. 多名前缀 (0x2F) - 后跟1字节计数+N个名称段
		 * 4. 无前缀 - 默认1个名称段
		 *
		 * 检查名称前缀操作码（如果有的话），以确定段的数量。
		 */
		switch (*path) {//检查路径的第一个字节
		case 0://空名称段情况
			/*
			 * 在根或父级前缀后没有名称。我们已经有了正确的目标节点，且没有名称段。
			 */
			num_segments = 0;//无名称段
			type = this_node->type;//继承当前节点类型

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Prefix-only Pathname (Zero name segments), Flags=%X\n",
					  flags));
			break;

		case AML_DUAL_NAME_PREFIX://双名前缀 (0x2E)

			/* More than one name_seg, search rules do not apply */
			/* 超过一个名称分隔符，搜索规则不适用 */
			search_parent_flag = ACPI_NS_NO_UPSEARCH;//多段路径禁用向上搜索

			/* Two segments, point to first name segment */

			num_segments = 2;//固定2个名称段
			path++;//跳过前缀

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Dual Pathname (2 segments, Flags=%X)\n",
					  flags));
			break;

		case AML_MULTI_NAME_PREFIX://多名前缀 (0x2F)

			/* More than one name_seg, search rules do not apply */

			search_parent_flag = ACPI_NS_NO_UPSEARCH;//多段路径禁用向上搜索

			/* Extract segment count, point to first name segment */

			path++;//跳过多名前缀
			num_segments = (u32) (u8) * path;//读取段数(1字节)
			path++;// 跳过计数字节

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Multi Pathname (%u Segments, Flags=%X)\n",
					  num_segments, flags));
			break;

		default://无前缀单名称段
			/*
			 * Not a Null name, no Dual or Multi prefix, hence there is
			 * only one name segment and Pathname is already pointing to it.
			 */
			num_segments = 1;//默认1个名称段

			ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
					  "Simple Pathname (1 segment, Flags=%X)\n",
					  flags));
			break;
		}

		ACPI_DEBUG_EXEC(acpi_ns_print_pathname(num_segments, path));
	}

	/*
	 * 在命名空间中搜索名称的每个段。循环处理并验证（或添加到命名空间）每个名称段。
	 * 对象类型仅在最后一个名称段时才有意义。（我们不关心路径上的类型，只关心最终
	 * 目标对象的类型。）
	 */
	this_search_type = ACPI_TYPE_ANY;//初始化搜索类型为任意类型（中间节点不检查类型）
	current_node = this_node;//从当前节点开始搜索

	while (num_segments && current_node) {//循环处理每个路径段（num_segments为剩余待处理的段数）
		num_segments--;//递减剩余段计数器
		if (!num_segments) {//如果是最后一个路径段

			/* This is the last segment, enable typechecking */
			/*  这是最后一个段，启用类型检查 */
			this_search_type = type;//使用调用者指定的目标类型

			/*
			 * 仅当调用者请求且我们处理的是单个非完全限定名称时，
			 * 才允许自动父级搜索（搜索规则）
			 */
			if ((search_parent_flag != ACPI_NS_NO_UPSEARCH) &&
			    (flags & ACPI_NS_SEARCH_PARENT)) {
				local_flags |= ACPI_NS_SEARCH_PARENT;//启用父级搜索标志
			}

			/* 根据调用者设置错误标志 */
			if (flags & ACPI_NS_ERROR_IF_FOUND) {
				local_flags |= ACPI_NS_ERROR_IF_FOUND;//如果已存在则报错
			}

			/* 根据调用者设置覆盖标志 */
			if (flags & ACPI_NS_OVERRIDE_IF_FOUND) {
				local_flags |= ACPI_NS_OVERRIDE_IF_FOUND;//如果已存在则覆盖
			}
		}

		/* Handle opcodes that create a new name_seg via a full name_path */
		/* 处理通过完整路径创建新名称段的操作码 */
		local_interpreter_mode = interpreter_mode;//保存当前解释器模式
		if ((flags & ACPI_NS_PREFIX_MUST_EXIST) && (num_segments > 0)) {

			/* Every element of the path must exist (except for the final name_seg) */

			local_interpreter_mode = ACPI_IMODE_EXECUTE;//强制设为执行模式
		}

		/* Extract one ACPI name from the front of the pathname */

		ACPI_MOVE_32_TO_32(&simple_name, path);//安全拷贝4字节名称

		/* Try to find the single (4 character) ACPI name */

		status =
		    acpi_ns_search_and_enter(simple_name, walk_state,
					     current_node,
					     local_interpreter_mode,
					     this_search_type, local_flags,
					     &this_node);//尝试查找单个（4字符）ACPI名称
		if (ACPI_FAILURE(status)) {
			if (status == AE_NOT_FOUND) {//处理未找到节点的情况
#if !defined ACPI_ASL_COMPILER	/* 注意：iASL自己报告此错误，此处不需要 */
				if (flags & ACPI_NS_PREFIX_MUST_EXIST) {
					acpi_os_printf(ACPI_MSG_BIOS_ERROR
						       "Object does not exist: %4.4s\n",
						       (char *)&simple_name);
				}
#endif
				/* 在ACPI命名空间中未找到名称 */

				ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
						  "Name [%4.4s] not found in scope [%4.4s] %p\n",
						  (char *)&simple_name,
						  (char *)&current_node->name,
						  current_node));
			}
#ifdef ACPI_EXEC_APP
			/* ACPI执行环境的特殊处理：忽略早期初始化节点的存在错误 */
			if ((status == AE_ALREADY_EXISTS) &&
			    (this_node->flags & ANOBJ_NODE_EARLY_INIT)) {
				this_node->flags &= ~ANOBJ_NODE_EARLY_INIT;
				status = AE_OK;
			}
#endif

#ifdef ACPI_ASL_COMPILER
			/*
			 * 如果此ACPI名称已作为外部声明存在于命名空间中，则将该外部声明标记
			 * 为冲突声明，并继续处理当前节点，就像它不存在于命名空间中一样。
			 * 如果不按常规处理此节点，可能会由于未能打开新作用域而导致命名空
			 * 间解析错误。
			 */
			if (acpi_gbl_disasm_flag &&//如果反汇编模式启用
			    (status == AE_ALREADY_EXISTS) &&//且节点已存在
			    ((this_node->flags & ANOBJ_IS_EXTERNAL) ||//节点是外部声明
			     (walk_state//或当前正在处理
			      && walk_state->opcode == AML_EXTERNAL_OP))) {// EXTERNAL_OP操作码
				this_node->flags &= ~ANOBJ_IS_EXTERNAL;//清除外部声明标志
				this_node->type = (u8)this_search_type;//强制更新节点类型
				if (walk_state->opcode != AML_EXTERNAL_OP) {
					acpi_dm_mark_external_conflict
					    (this_node);//标记冲突
				}
				break;
			}
#endif

			*return_node = this_node;//返回冲突节点指针
			return_ACPI_STATUS(status);//返回存在错误状态
		}

		/* More segments to follow? */

		if (num_segments > 0) {//如果还有未处理的路径段
			/*
			 * If we have an alias to an object that opens a scope (such as a
			 * device or processor), we need to dereference the alias here so
			 * that we can access any children of the original node (via the
			 * remaining segments).
			 * 如果当前节点是打开作用域的对象的别名（例如设备或处理器），
			 * 我们需要在这里解引用别名，以便通过剩余路径段访问原始节点的子节点
			 */
			if (this_node->type == ACPI_TYPE_LOCAL_ALIAS) {//检查节点类型是否为别名
				if (!this_node->object) {//检查别名是否指向有效对象
					return_ACPI_STATUS(AE_NOT_EXIST);//无效别名返回不存在错误
				}

				/* 检查别名指向的对象类型是否会打开新的作用域 */
				if (acpi_ns_opens_scope
				    (((struct acpi_namespace_node *)
				      this_node->object)->type)) {//判断对象类型是否打开作用域
					this_node =
					    (struct acpi_namespace_node *)
					    this_node->object;//获取别名指向的实际节点
				}
			}
		}

		/*  对最后一个路径段的特殊处理（num_segments == 0）*/

		else {
			/*
			 * 对目标对象进行健全性类型检查：
			 * 当满足以下所有条件时，表示类型不匹配：
			 * 1. 这是最后一个路径段
			 * 2. 正在查找特定类型（非TYPE_ANY）
			 * 3. 查找的类型不是别名类型
			 * 4. 查找的类型不是局部作用域类型
			 * 5. 目标对象的类型已知（非TYPE_ANY）
			 * 6. 目标对象类型与查找类型不匹配
			 * 这种情况下只发出警告并继续处理
			 */
			if ((type_to_check_for != ACPI_TYPE_ANY) &&//检查是否指定了具体类型
			    (type_to_check_for != ACPI_TYPE_LOCAL_ALIAS) &&//排除别名类型
			    (type_to_check_for != ACPI_TYPE_LOCAL_METHOD_ALIAS)// 排除方法别名
			    && (type_to_check_for != ACPI_TYPE_LOCAL_SCOPE)//排除局部作用域
			    && (this_node->type != ACPI_TYPE_ANY)//目标对象类型是否已知
			    && (this_node->type != type_to_check_for)) {//类型是否匹配

				/* 类型不匹配时的警告处理 */

				ACPI_WARNING((AE_INFO,
					      "NsLookup: Type mismatch on %4.4s (%s), searching for (%s)",
					      ACPI_CAST_PTR(char, &simple_name),
					      acpi_ut_get_type_name(this_node->
								    type),
					      acpi_ut_get_type_name
					      (type_to_check_for)));
			}

			/*
			 * 如果是最后一个路径段且没有指定特定类型（查找TYPE_ANY），
			 * 但找到的对象类型已知，则使用该类型来判断是否打开作用域
			 */
			if (type == ACPI_TYPE_ANY) {//检查是否接受任何类型
				type = this_node->type;//使用找到的节点类型更新查找类型
			}
		}

		/* Point to next name segment and make this node current */

		path += ACPI_NAMESEG_SIZE;//移动到下一个名称段，并将当前节点更新为找到的节点
		current_node = this_node;//更新当前节点指针
	}

	/* Always check if we need to open a new scope */

	if (!(flags & ACPI_NS_DONT_OPEN_SCOPE) && (walk_state)) {// 检查是否需要且可以打开作用域
		/*
		 * If entry is a type which opens a scope, push the new scope on the
		 * scope stack.
		 * 如果当前节点类型是需要打开作用域的类型，则将新作用域压入作用域堆栈
		 */
		if (acpi_ns_opens_scope(type)) {//检查节点类型是否需要打开作用域
			status =
			    acpi_ds_scope_stack_push(this_node, type,
						     walk_state);//压入作用域堆栈
			if (ACPI_FAILURE(status)) {//检查压栈操作是否成功
				return_ACPI_STATUS(status);//失败时返回错误状态
			}
		}
	}
#ifdef ACPI_EXEC_APP
	if (flags & ACPI_NS_EARLY_INIT) {//检查是否需要标记为早期初始化节点
		this_node->flags |= ANOBJ_NODE_EARLY_INIT;//设置早期初始化标志位
	}
#endif

	*return_node = this_node;// 通过输出参数返回找到的节点指针
	return_ACPI_STATUS(AE_OK);
}
