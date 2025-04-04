// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: evregion - Operation Region support
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acevents.h"
#include "acnamesp.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_EVENTS
ACPI_MODULE_NAME("evregion")

extern u8 acpi_gbl_default_address_spaces[];

/* Local prototypes */

static acpi_status
acpi_ev_reg_run(acpi_handle obj_handle,
		u32 level, void *context, void **return_value);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_initialize_op_regions
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute _REG methods for all Operation Regions that have
 *              an installed default region handler.
 *
 ******************************************************************************/

acpi_status acpi_ev_initialize_op_regions(void)
{
	acpi_status status;
	u32 i;

	ACPI_FUNCTION_TRACE(ev_initialize_op_regions);

	status = acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Run the _REG methods for op_regions in each default address space */

	for (i = 0; i < ACPI_NUM_DEFAULT_SPACES; i++) {
		/*
		 * Make sure the installed handler is the DEFAULT handler. If not the
		 * default, the _REG methods will have already been run (when the
		 * handler was installed)
		 */
		if (acpi_ev_has_default_handler(acpi_gbl_root_node,
						acpi_gbl_default_address_spaces
						[i])) {
			acpi_ev_execute_reg_methods(acpi_gbl_root_node,
						    acpi_gbl_default_address_spaces
						    [i], ACPI_REG_CONNECT);
		}
	}

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_address_space_dispatch
 *
 * PARAMETERS:  region_obj          - Internal region object
 *              field_obj           - Corresponding field. Can be NULL.
 *              function            - Read or Write operation
 *              region_offset       - Where in the region to read or write
 *              bit_width           - Field width in bits (8, 16, 32, or 64)
 *              value               - Pointer to in or out value, must be
 *                                    a full 64-bit integer
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Dispatch an address space or operation region access to
 *              a previously installed handler.
 *
 * NOTE: During early initialization, we always install the default region
 * handlers for Memory, I/O and PCI_Config. This ensures that these operation
 * region address spaces are always available as per the ACPI specification.
 * This is especially needed in order to support the execution of
 * module-level AML code during loading of the ACPI tables.
 *
 ******************************************************************************/

acpi_status
acpi_ev_address_space_dispatch(union acpi_operand_object *region_obj,
			       union acpi_operand_object *field_obj,
			       u32 function,
			       u32 region_offset, u32 bit_width, u64 *value)
{
	acpi_status status;
	acpi_adr_space_handler handler;
	acpi_adr_space_setup region_setup;
	union acpi_operand_object *handler_desc;
	union acpi_operand_object *region_obj2;
	void *region_context = NULL;
	struct acpi_connection_info *context;
	acpi_mutex context_mutex;
	u8 context_locked;
	acpi_physical_address address;

	ACPI_FUNCTION_TRACE(ev_address_space_dispatch);

	region_obj2 = acpi_ns_get_secondary_object(region_obj);
	if (!region_obj2) {
		return_ACPI_STATUS(AE_NOT_EXIST);
	}

	/* Ensure that there is a handler associated with this region */

	handler_desc = region_obj->region.handler;
	if (!handler_desc) {
		ACPI_ERROR((AE_INFO,
			    "No handler for Region [%4.4s] (%p) [%s]",
			    acpi_ut_get_node_name(region_obj->region.node),
			    region_obj,
			    acpi_ut_get_region_name(region_obj->region.
						    space_id)));

		return_ACPI_STATUS(AE_NOT_EXIST);
	}

	context = handler_desc->address_space.context;
	context_mutex = handler_desc->address_space.context_mutex;
	context_locked = FALSE;

	/*
	 * It may be the case that the region has never been initialized.
	 * Some types of regions require special init code
	 */
	if (!(region_obj->region.flags & AOPOBJ_SETUP_COMPLETE)) {

		/* This region has not been initialized yet, do it */

		region_setup = handler_desc->address_space.setup;
		if (!region_setup) {

			/* No initialization routine, exit with error */

			ACPI_ERROR((AE_INFO,
				    "No init routine for region(%p) [%s]",
				    region_obj,
				    acpi_ut_get_region_name(region_obj->region.
							    space_id)));
			return_ACPI_STATUS(AE_NOT_EXIST);
		}

		if (region_obj->region.space_id == ACPI_ADR_SPACE_PLATFORM_COMM) {
			struct acpi_pcc_info *ctx =
			    handler_desc->address_space.context;

			ctx->internal_buffer =
			    field_obj->field.internal_pcc_buffer;
			ctx->length = (u16)region_obj->region.length;
			ctx->subspace_id = (u8)region_obj->region.address;
		}

		if (region_obj->region.space_id ==
		    ACPI_ADR_SPACE_FIXED_HARDWARE) {
			struct acpi_ffh_info *ctx =
			    handler_desc->address_space.context;

			ctx->length = region_obj->region.length;
			ctx->offset = region_obj->region.address;
		}

		/*
		 * We must exit the interpreter because the region setup will
		 * potentially execute control methods (for example, the _REG method
		 * for this region)
		 */
		acpi_ex_exit_interpreter();

		status = region_setup(region_obj, ACPI_REGION_ACTIVATE,
				      context, &region_context);

		/* Re-enter the interpreter */

		acpi_ex_enter_interpreter();

		/* Check for failure of the Region Setup */

		if (ACPI_FAILURE(status)) {
			ACPI_EXCEPTION((AE_INFO, status,
					"During region initialization: [%s]",
					acpi_ut_get_region_name(region_obj->
								region.
								space_id)));
			return_ACPI_STATUS(status);
		}

		/* Region initialization may have been completed by region_setup */

		if (!(region_obj->region.flags & AOPOBJ_SETUP_COMPLETE)) {
			region_obj->region.flags |= AOPOBJ_SETUP_COMPLETE;

			/*
			 * Save the returned context for use in all accesses to
			 * the handler for this particular region
			 */
			if (!(region_obj2->extra.region_context)) {
				region_obj2->extra.region_context =
				    region_context;
			}
		}
	}

	/* We have everything we need, we can invoke the address space handler */

	handler = handler_desc->address_space.handler;
	address = (region_obj->region.address + region_offset);

	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "Handler %p (@%p) Address %8.8X%8.8X [%s]\n",
			  &region_obj->region.handler->address_space, handler,
			  ACPI_FORMAT_UINT64(address),
			  acpi_ut_get_region_name(region_obj->region.
						  space_id)));

	if (!(handler_desc->address_space.handler_flags &
	      ACPI_ADDR_HANDLER_DEFAULT_INSTALLED)) {
		/*
		 * For handlers other than the default (supplied) handlers, we must
		 * exit the interpreter because the handler *might* block -- we don't
		 * know what it will do, so we can't hold the lock on the interpreter.
		 */
		acpi_ex_exit_interpreter();
	}

	/*
	 * Special handling for generic_serial_bus and general_purpose_io:
	 * There are three extra parameters that must be passed to the
	 * handler via the context:
	 *   1) Connection buffer, a resource template from Connection() op
	 *   2) Length of the above buffer
	 *   3) Actual access length from the access_as() op
	 *
	 * Since we pass these extra parameters via the context, which is
	 * shared between threads, we must lock the context to avoid these
	 * parameters being changed from another thread before the handler
	 * has completed running.
	 *
	 * In addition, for general_purpose_io, the Address and bit_width fields
	 * are defined as follows:
	 *   1) Address is the pin number index of the field (bit offset from
	 *      the previous Connection)
	 *   2) bit_width is the actual bit length of the field (number of pins)
	 */
	if ((region_obj->region.space_id == ACPI_ADR_SPACE_GSBUS ||
	     region_obj->region.space_id == ACPI_ADR_SPACE_GPIO) &&
	    context && field_obj) {

		status =
		    acpi_os_acquire_mutex(context_mutex, ACPI_WAIT_FOREVER);
		if (ACPI_FAILURE(status)) {
			goto re_enter_interpreter;
		}

		context_locked = TRUE;

		/* Get the Connection (resource_template) buffer */

		context->connection = field_obj->field.resource_buffer;
		context->length = field_obj->field.resource_length;
		context->access_length = field_obj->field.access_length;

		if (region_obj->region.space_id == ACPI_ADR_SPACE_GPIO) {
			address = field_obj->field.pin_number_index;
			bit_width = field_obj->field.bit_length;
		}
	}

	/* Call the handler */

	status = handler(function, address, bit_width, value, context,
			 region_obj2->extra.region_context);

	if (context_locked) {
		acpi_os_release_mutex(context_mutex);
	}

	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "Returned by Handler for [%s]",
				acpi_ut_get_region_name(region_obj->region.
							space_id)));

		/*
		 * Special case for an EC timeout. These are seen so frequently
		 * that an additional error message is helpful
		 */
		if ((region_obj->region.space_id == ACPI_ADR_SPACE_EC) &&
		    (status == AE_TIME)) {
			ACPI_ERROR((AE_INFO,
				    "Timeout from EC hardware or EC device driver"));
		}
	}

re_enter_interpreter:
	if (!(handler_desc->address_space.handler_flags &
	      ACPI_ADDR_HANDLER_DEFAULT_INSTALLED)) {
		/*
		 * We just returned from a non-default handler, we must re-enter the
		 * interpreter
		 */
		acpi_ex_enter_interpreter();
	}

	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_detach_region
 *
 * PARAMETERS:  region_obj          - Region Object
 *              acpi_ns_is_locked   - Namespace Region Already Locked?
 *
 * RETURN:      None
 *
 * DESCRIPTION: Break the association between the handler and the region
 *              this is a two way association.
 *
 ******************************************************************************/

void
acpi_ev_detach_region(union acpi_operand_object *region_obj,
		      u8 acpi_ns_is_locked)
{
	union acpi_operand_object *handler_obj;
	union acpi_operand_object *obj_desc;
	union acpi_operand_object *start_desc;
	union acpi_operand_object **last_obj_ptr;
	acpi_adr_space_setup region_setup;
	void **region_context;
	union acpi_operand_object *region_obj2;
	acpi_status status;

	ACPI_FUNCTION_TRACE(ev_detach_region);

	region_obj2 = acpi_ns_get_secondary_object(region_obj);
	if (!region_obj2) {
		return_VOID;
	}
	region_context = &region_obj2->extra.region_context;

	/* Get the address handler from the region object */

	handler_obj = region_obj->region.handler;
	if (!handler_obj) {

		/* This region has no handler, all done */

		return_VOID;
	}

	/* Find this region in the handler's list */

	obj_desc = handler_obj->address_space.region_list;
	start_desc = obj_desc;
	last_obj_ptr = &handler_obj->address_space.region_list;

	while (obj_desc) {

		/* Is this the correct Region? */

		if (obj_desc == region_obj) {
			ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
					  "Removing Region %p from address handler %p\n",
					  region_obj, handler_obj));

			/* This is it, remove it from the handler's list */

			*last_obj_ptr = obj_desc->region.next;
			obj_desc->region.next = NULL;	/* Must clear field */

			if (acpi_ns_is_locked) {
				status =
				    acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);
				if (ACPI_FAILURE(status)) {
					return_VOID;
				}
			}

			/* Now stop region accesses by executing the _REG method */

			status =
			    acpi_ev_execute_reg_method(region_obj,
						       ACPI_REG_DISCONNECT);
			if (ACPI_FAILURE(status)) {
				ACPI_EXCEPTION((AE_INFO, status,
						"from region _REG, [%s]",
						acpi_ut_get_region_name
						(region_obj->region.space_id)));
			}

			if (acpi_ns_is_locked) {
				status =
				    acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
				if (ACPI_FAILURE(status)) {
					return_VOID;
				}
			}

			/*
			 * If the region has been activated, call the setup handler with
			 * the deactivate notification
			 */
			if (region_obj->region.flags & AOPOBJ_SETUP_COMPLETE) {
				region_setup = handler_obj->address_space.setup;
				status =
				    region_setup(region_obj,
						 ACPI_REGION_DEACTIVATE,
						 handler_obj->address_space.
						 context, region_context);

				/*
				 * region_context should have been released by the deactivate
				 * operation. We don't need access to it anymore here.
				 */
				if (region_context) {
					*region_context = NULL;
				}

				/* Init routine may fail, Just ignore errors */

				if (ACPI_FAILURE(status)) {
					ACPI_EXCEPTION((AE_INFO, status,
							"from region handler - deactivate, [%s]",
							acpi_ut_get_region_name
							(region_obj->region.
							 space_id)));
				}

				region_obj->region.flags &=
				    ~(AOPOBJ_SETUP_COMPLETE);
			}

			/*
			 * Remove handler reference in the region
			 *
			 * NOTE: this doesn't mean that the region goes away, the region
			 * is just inaccessible as indicated to the _REG method
			 *
			 * If the region is on the handler's list, this must be the
			 * region's handler
			 */
			region_obj->region.handler = NULL;
			acpi_ut_remove_reference(handler_obj);

			return_VOID;
		}

		/* Walk the linked list of handlers */

		last_obj_ptr = &obj_desc->region.next;
		obj_desc = obj_desc->region.next;

		/* Prevent infinite loop if list is corrupted */

		if (obj_desc == start_desc) {
			ACPI_ERROR((AE_INFO,
				    "Circular handler list in region object %p",
				    region_obj));
			return_VOID;
		}
	}

	/* If we get here, the region was not in the handler's region list */

	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "Cannot remove region %p from address handler %p\n",
			  region_obj, handler_obj));

	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_attach_region
 *
 * PARAMETERS:  handler_obj         - Handler Object
 *              region_obj          - Region Object
 *              acpi_ns_is_locked   - Namespace Region Already Locked?
 *
 * RETURN:      None
 *
 * DESCRIPTION: Create the association between the handler and the region
 *              this is a two way association.
 *
 ******************************************************************************/
/* 将ACPI区域对象（如内存/IO区域）与指定的地址空间处理程序关联 */
acpi_status
acpi_ev_attach_region(union acpi_operand_object *handler_obj,//要关联的地址空间处理程序对象
		      union acpi_operand_object *region_obj,//需要绑定的ACPI区域对象
		      u8 acpi_ns_is_locked)
{

	ACPI_FUNCTION_TRACE(ev_attach_region);

	/* Install the region's handler */

	if (region_obj->region.handler) {// 如果区域已关联处理程序
		return_ACPI_STATUS(AE_ALREADY_EXISTS);//返回错误（已存在）
	}

	ACPI_DEBUG_PRINT((ACPI_DB_OPREGION,
			  "Adding Region [%4.4s] %p to address handler %p [%s]\n",
			  acpi_ut_get_node_name(region_obj->region.node),
			  region_obj, handler_obj,
			  acpi_ut_get_region_name(region_obj->region.
						  space_id)));

	/* Link this region to the front of the handler's list */

	region_obj->region.next = handler_obj->address_space.region_list;// 将区域的next指向原链表头
	handler_obj->address_space.region_list = region_obj;//新区域成为链表头
	region_obj->region.handler = handler_obj;//双向关联：区域指向处理程序
	acpi_ut_add_reference(handler_obj);//增加处理程序对象的引用计数（防止提前释放）

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_execute_reg_method
 *
 * PARAMETERS:  region_obj          - Region object
 *              function            - Passed to _REG: On (1) or Off (0)
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute _REG method for a region
 *
 ******************************************************************************/

acpi_status
acpi_ev_execute_reg_method(union acpi_operand_object *region_obj, u32 function)
{
	struct acpi_evaluate_info *info;//ACPI方法评估信息结构体指针
	union acpi_operand_object *args[3];//方法参数数组（最多3个参数）
	union acpi_operand_object *region_obj2;//区域对象的二级对象指针
	const acpi_name *reg_name_ptr =
	    ACPI_CAST_PTR(acpi_name, METHOD_NAME__REG);//指向方法名"_REG"的指针
	struct acpi_namespace_node *method_node;//命名空间节点（存放找到的_REG方法）
	struct acpi_namespace_node *node;//临时命名空间节点指针
	acpi_status status;//操作状态返回值

	ACPI_FUNCTION_TRACE(ev_execute_reg_method);//ACPI调试宏，记录函数入口

	if (!acpi_gbl_namespace_initialized ||
	    region_obj->region.handler == NULL) {//检查全局命名空间是否初始化或区域对象是否有处理程序
		return_ACPI_STATUS(AE_OK);//如果未初始化或无处理程序，直接返回成功
	}

	region_obj2 = acpi_ns_get_secondary_object(region_obj);//获取区域对象的二级对象（用于存储额外信息）
	if (!region_obj2) {
		return_ACPI_STATUS(AE_NOT_EXIST);//返回"不存在"错误
	}

	/*
	 * Find any "_REG" method associated with this region definition.
	 * The method should always be updated as this function may be
	 * invoked after a namespace change.
	 */
	/*
	 * 查找与此区域定义关联的任何"_REG"方法。该方法应始终保持更新，因为
	 * 可能在命名空间更改后调用此函数。
	 * */
	node = region_obj->region.node->parent;//获取区域对象的父节点
	status =
	    acpi_ns_search_one_scope(*reg_name_ptr, node, ACPI_TYPE_METHOD,
				     &method_node);//在当前作用域搜索_REG方法
	if (ACPI_SUCCESS(status)) {//如果成功找到_REG方法
		/*
		 * The _REG method is optional and there can be only one per
		 * region definition. This will be executed when the handler is
		 * attached or removed.
		 */
		/* _REG方法是可选的，每个区域定义只能有一个。当处理程序附加或移除时执行。 */
		region_obj2->extra.method_REG = method_node;//将找到的方法节点存入二级对象
	}
	if (region_obj2->extra.method_REG == NULL) {//如果没有_REG方法
		return_ACPI_STATUS(AE_OK);//直接返回成功
	}

	/* _REG(DISCONNECT) should be paired with _REG(CONNECT) */
	/* _REG(DISCONNECT) 应该与 _REG(CONNECT) 成对出现 */
	//检查连接/断开状态是否匹配（避免重复操作）
	if ((function == ACPI_REG_CONNECT &&
	     region_obj->common.flags & AOPOBJ_REG_CONNECTED) ||
	    (function == ACPI_REG_DISCONNECT &&
	     !(region_obj->common.flags & AOPOBJ_REG_CONNECTED))) {
		return_ACPI_STATUS(AE_OK);//如果状态已匹配，直接返回
	}

	/* Allocate and initialize the evaluation information block */
	/* 分配并初始化评估信息块 */
	info = ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_evaluate_info));//分配并清零内存
	if (!info) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	info->prefix_node = region_obj2->extra.method_REG;//设置方法节点
	info->relative_pathname = NULL;//相对路径名为空
	info->parameters = args;// 设置参数指针
	info->flags = ACPI_IGNORE_RETURN_VALUE;//忽略返回值标志

	/*
	 * The _REG method has two arguments:
	 *
	 * arg0 - Integer:
	 *  Operation region space ID Same value as region_obj->Region.space_id
	 *
	 * arg1 - Integer:
	 *  connection status 1 for connecting the handler, 0 for disconnecting
	 *  the handler (Passed as a parameter)
	 *  _REG方法有两个参数：
	 *   arg0 - 整数：操作区域空间ID（与region_obj->Region.space_id相同）
	 *
	 *   arg1 - 整数：连接状态 1表示连接处理程序，0表示断开处理程序（通过参数传入）
	 */
	args[0] =
	    acpi_ut_create_integer_object((u64)region_obj->region.space_id);//创建空间ID参数
	if (!args[0]) {
		status = AE_NO_MEMORY;//设置内存不足状态
		goto cleanup1;
	}

	args[1] = acpi_ut_create_integer_object((u64)function);//创建功能参数（连接/断开）
	if (!args[1]) {
		status = AE_NO_MEMORY;//设置内存不足状态
		goto cleanup2;
	}

	args[2] = NULL;		//参数列表终止符

	/* Execute the method, no return value */
	/* 执行方法（无返回值） */
	ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
			(ACPI_TYPE_METHOD, info->prefix_node, NULL));

	status = acpi_ns_evaluate(info);//实际执行_REG方法
	acpi_ut_remove_reference(args[1]);//减少参数1的引用计数

	if (ACPI_FAILURE(status)) {
		goto cleanup2;
	}

	if (function == ACPI_REG_CONNECT) {//根据操作类型更新连接状态标志
		region_obj->common.flags |= AOPOBJ_REG_CONNECTED;//设置连接标志
	} else {
		region_obj->common.flags &= ~AOPOBJ_REG_CONNECTED;//清除连接标志
	}

cleanup2:
	acpi_ut_remove_reference(args[0]);//减少参数0的引用计数

cleanup1:
	ACPI_FREE(info);//释放评估信息结构体
	return_ACPI_STATUS(status);//返回最终状态
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_execute_reg_methods
 *
 * PARAMETERS:  node            - Namespace node for the device
 *              space_id        - The address space ID
 *              function        - Passed to _REG: On (1) or Off (0)
 *
 * RETURN:      None
 *
 * DESCRIPTION: Run all _REG methods for the input Space ID;
 *              Note: assumes namespace is locked, or system init time.
 *
 ******************************************************************************/
/**
 * acpi_ev_execute_reg_methods - 执行指定地址空间的_REG方法
 * @node: 命名空间起始节点(通常为设备节点)
 * @space_id: 地址空间ID(ACPI_ADR_SPACE_*)
 * @function: 操作类型(ACPI_REG_CONNECT或ACPI_REG_DISCONNECT)
 *
 * 功能说明:
 * 1. 跳过不需要_REG方法的特殊地址空间
 * 2. 遍历命名空间执行匹配的_REG方法
 * 3. 处理EC和GPIO空间的特殊"孤儿"_REG方法
 * 4. 记录调试信息
 */
void
acpi_ev_execute_reg_methods(struct acpi_namespace_node *node,
			    acpi_adr_space_type space_id, u32 function)
{
	struct acpi_reg_walk_info info;//定义_REG方法执行上下文结构

	ACPI_FUNCTION_TRACE(ev_execute_reg_methods);

	/*
	 * 这些地址空间不需要调用_REG，因为ACPI规范将它们定义为：“必须始终可访问”。
	 * 由于它们的状态永远不会改变（永远不会变得不可用），因此无需在这些地址空
	 * 间上调用_REG。此外，数据表不是一个“真正的”地址空间，所以也不要调用_REG。
	 */
	/*
    	 * 跳过始终可访问的地址空间:
    	 * - 系统内存空间(ACPI_ADR_SPACE_SYSTEM_MEMORY)
    	 * - 系统IO空间(ACPI_ADR_SPACE_SYSTEM_IO)
    	 * - 数据表空间(ACPI_ADR_SPACE_DATA_TABLE)
    	 */
	if ((space_id == ACPI_ADR_SPACE_SYSTEM_MEMORY) ||
	    (space_id == ACPI_ADR_SPACE_SYSTEM_IO) ||
	    (space_id == ACPI_ADR_SPACE_DATA_TABLE)) {
		return_VOID;//直接返回不执行_REG方法
	}

	/* 初始化_REG执行上下文 */
	info.space_id = space_id;//设置目标地址空间ID
	info.function = function;//设置连接/断开操作类型
	info.reg_run_count = 0;//初始化_REG方法执行计数器

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_NAMES,
			      "    Running _REG methods for SpaceId %s\n",
			      acpi_ut_get_region_name(info.space_id)));

	/*
	 * 为这个空间ID运行所有操作区域的所有_REG方法。这是一个单独的遍历过程，
	 * 以便处理各个区域之间的依赖关系以及_REG方法。（即，我们必须先为这个空间
	 * ID 的所有区域安装处理程序，然后才能运行任何 _REG 方法）
	 */
	/*
    	 * 遍历命名空间执行_REG方法:
    	 * - 起始节点: 传入的node参数
    	 * - 最大深度: 无限制(ACPI_UINT32_MAX)
    	 * - 标志: 不持有命名空间锁(ACPI_NS_WALK_UNLOCK)
    	 * - 回调函数: acpi_ev_reg_run(实际执行_REG方法)
    	 * - 上下文: &info结构传递参数
    	 */
	(void)acpi_ns_walk_namespace(ACPI_TYPE_ANY, node, ACPI_UINT32_MAX,
				     ACPI_NS_WALK_UNLOCK, acpi_ev_reg_run, NULL,
				     &info, NULL);

	/*
	 * Special case for EC and GPIO: handle "orphan" _REG methods with
	 * no region.
	 */
	/*
    	 * 特殊处理EC和GPIO空间:
    	 * 执行没有关联区域的"孤儿"_REG方法
    	 */
	if (space_id == ACPI_ADR_SPACE_EC || space_id == ACPI_ADR_SPACE_GPIO) {
		acpi_ev_execute_orphan_reg_method(node, space_id);
	}

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_NAMES,
			      "    Executed %u _REG methods for SpaceId %s\n",
			      info.reg_run_count,
			      acpi_ut_get_region_name(info.space_id)));

	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_reg_run
 *
 * PARAMETERS:  walk_namespace callback
 *
 * DESCRIPTION: Run _REG method for region objects of the requested spaceID
 *
 ******************************************************************************/
/**
 * acpi_ev_reg_run - 执行_REG方法的命名空间遍历回调函数
 * @obj_handle: 当前节点的对象句柄
 * @level: 当前遍历深度
 * @context: 传递的上下文信息(acpi_reg_walk_info结构)
 * @return_value: 用于返回值的指针(未使用)
 *
 * 返回值:
 *  AE_OK - 继续遍历
 *  AE_BAD_PARAMETER - 参数错误
 *  其他 - _REG方法执行失败
 *
 * 功能说明:
 * 1. 验证节点有效性
 * 2. 过滤非区域节点
 * 3. 检查地址空间匹配
 * 4. 执行匹配的_REG方法
 */
static acpi_status
acpi_ev_reg_run(acpi_handle obj_handle,
		u32 level, void *context, void **return_value)
{
	union acpi_operand_object *obj_desc;//节点关联的操作对象
	struct acpi_namespace_node *node;//命名空间节点
	acpi_status status;//操作状态
	struct acpi_reg_walk_info *info;//遍历上下文信息

	info = ACPI_CAST_PTR(struct acpi_reg_walk_info, context);//获取上下文信息

	/* Convert and validate the device handle */

	node = acpi_ns_validate_handle(obj_handle);//验证并转换节点句柄
	if (!node) {
		return (AE_BAD_PARAMETER);//返回无效节点句柄
	}

	/*
     	 * 节点类型过滤:
    	 * 只处理操作区域(ACPI_TYPE_REGION)和根节点
    	 * 其他类型节点直接跳过
    	 */
	if ((node->type != ACPI_TYPE_REGION) && (node != acpi_gbl_root_node)) {
		return (AE_OK);
	}

	/* Check for an existing internal object */

	obj_desc = acpi_ns_get_attached_object(node);//获取节点关联的操作对象
	if (!obj_desc) {

		/* No object, just exit */

		return (AE_OK);//无关联对象，跳过处理
	}

	/* Object is a Region */

	if (obj_desc->region.space_id != info->space_id) {//检查地址空间匹配性

		/* This region is for a different address space, just ignore it */

		return (AE_OK);//不匹配目标地址空间，跳过
	}

	info->reg_run_count++;//增加_REG方法执行计数器
	status = acpi_ev_execute_reg_method(obj_desc, info->function);//实际执行_REG方法
	return (status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ev_execute_orphan_reg_method
 *
 * PARAMETERS:  device_node         - Namespace node for an ACPI device
 *              space_id            - The address space ID
 *
 * RETURN:      None
 *
 * DESCRIPTION: Execute an "orphan" _REG method that appears under an ACPI
 *              device. This is a _REG method that has no corresponding region
 *              within the device's scope. ACPI tables depending on these
 *              "orphan" _REG methods have been seen for both EC and GPIO
 *              Operation Regions. Presumably the Windows ACPI implementation
 *              always calls the _REG method independent of the presence of
 *              an actual Operation Region with the correct address space ID.
 *
 *  MUTEX:      Assumes the namespace is locked
 *
 ******************************************************************************/

void
acpi_ev_execute_orphan_reg_method(struct acpi_namespace_node *device_node,
				  acpi_adr_space_type space_id)
{
	acpi_handle reg_method;
	struct acpi_namespace_node *next_node;
	acpi_status status;
	struct acpi_object_list args;
	union acpi_object objects[2];

	ACPI_FUNCTION_TRACE(ev_execute_orphan_reg_method);

	if (!device_node) {
		return_VOID;
	}

	/* Namespace is currently locked, must release */

	(void)acpi_ut_release_mutex(ACPI_MTX_NAMESPACE);

	/* Get a handle to a _REG method immediately under the EC device */

	status = acpi_get_handle(device_node, METHOD_NAME__REG, &reg_method);
	if (ACPI_FAILURE(status)) {
		goto exit;	/* There is no _REG method present */
	}

	/*
	 * Execute the _REG method only if there is no Operation Region in
	 * this scope with the Embedded Controller space ID. Otherwise, it
	 * will already have been executed. Note, this allows for Regions
	 * with other space IDs to be present; but the code below will then
	 * execute the _REG method with the embedded_control space_ID argument.
	 */
	next_node = acpi_ns_get_next_node(device_node, NULL);
	while (next_node) {
		if ((next_node->type == ACPI_TYPE_REGION) &&
		    (next_node->object) &&
		    (next_node->object->region.space_id == space_id)) {
			goto exit;	/* Do not execute the _REG */
		}

		next_node = acpi_ns_get_next_node(device_node, next_node);
	}

	/* Evaluate the _REG(space_id,Connect) method */

	args.count = 2;
	args.pointer = objects;
	objects[0].type = ACPI_TYPE_INTEGER;
	objects[0].integer.value = space_id;
	objects[1].type = ACPI_TYPE_INTEGER;
	objects[1].integer.value = ACPI_REG_CONNECT;

	(void)acpi_evaluate_object(reg_method, NULL, &args, NULL);

exit:
	/* We ignore all errors from above, don't care */

	(void)acpi_ut_acquire_mutex(ACPI_MTX_NAMESPACE);
	return_VOID;
}
