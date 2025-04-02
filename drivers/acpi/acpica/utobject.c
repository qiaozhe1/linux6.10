// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: utobject - ACPI object create/delete/size/cache routines
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include <linux/kmemleak.h>
#include "accommon.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utobject")

/* Local prototypes */
static acpi_status
acpi_ut_get_simple_object_size(union acpi_operand_object *obj,
			       acpi_size *obj_length);

static acpi_status
acpi_ut_get_package_object_size(union acpi_operand_object *obj,
				acpi_size *obj_length);

static acpi_status
acpi_ut_get_element_length(u8 object_type,
			   union acpi_operand_object *source_object,
			   union acpi_generic_state *state, void *context);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_internal_object_dbg
 *
 * PARAMETERS:  module_name         - Source file name of caller
 *              line_number         - Line number of caller
 *              component_id        - Component type of caller
 *              type                - ACPI Type of the new object
 *
 * RETURN:      A new internal object, null on failure
 *
 * DESCRIPTION: Create and initialize a new internal object.
 *
 * NOTE:        We always allocate the worst-case object descriptor because
 *              these objects are cached, and we want them to be
 *              one-size-satisfies-any-request. This in itself may not be
 *              the most memory efficient, but the efficiency of the object
 *              cache should more than make up for this!
 *
 ******************************************************************************/
/**
 * acpi_ut_create_internal_object_dbg - 创建ACPI内部对象
 * @module_name: 调用模块名(用于调试)
 * @line_number: 调用行号(用于调试)
 * @component_id: 组件ID(用于调试)
 * @type: 要创建的ACPI对象类型
 *
 * 返回值:
 *   union acpi_operand_object* - 新创建的对象指针
 *   NULL - 创建失败
 *
 * 功能说明:
 * 1. 分配对象描述符内存
 * 2. 处理需要二级对象的特殊类型
 * 3. 初始化对象类型和引用计数
 * 4. 调试信息记录
 */
union acpi_operand_object *acpi_ut_create_internal_object_dbg(const char
							      *module_name,
							      u32 line_number,
							      u32 component_id,
							      acpi_object_type
							      type)
{
	union acpi_operand_object *object;//定义一级对象指针(主对象)
	union acpi_operand_object *second_object;//定义二级对象指针(用于特殊对象类型)

	ACPI_FUNCTION_TRACE_STR(ut_create_internal_object_dbg,
				acpi_ut_get_type_name(type));

	/* Allocate the raw object descriptor */

	object =
	    acpi_ut_allocate_object_desc_dbg(module_name, line_number,
					     component_id);//分配主对象内存,使用带调试信息的分配函数，便于问题追踪
	if (!object) {//检查内存分配是否成功
		return_PTR(NULL);//分配失败返回NULL，并通过return_PTR宏记录调试信息
	}
	kmemleak_not_leak(object);

	switch (type) {//特殊对象类型处理 - 需要额外分配二级对象,这些对象类型需要额外的存储空间来维护其状态
	case ACPI_TYPE_REGION://操作区域对象(用于硬件寄存器访问)
	case ACPI_TYPE_BUFFER_FIELD://缓冲区字段对象(用于数据缓冲区管理)    
	case ACPI_TYPE_LOCAL_BANK_FIELD://bank字段对象(用于bank切换场景)

		/* These types require a secondary object */

		second_object =
		    acpi_ut_allocate_object_desc_dbg(module_name, line_number,
						     component_id);//为特殊对象类型分配二级对象
		if (!second_object) {//检查二级对象分配是否成功
			acpi_ut_delete_object_desc(object);//分配失败时释放已分配的主对象
			return_PTR(NULL);//返回NULL表示失败
		}

		/*
		 * 初始化二级对象
		 * 类型设置为LOCAL_EXTRA表示是扩展对象
		 * */
		second_object->common.type = ACPI_TYPE_LOCAL_EXTRA;
		second_object->common.reference_count = 1;//初始化引用计数为1(由主对象持有)

		/* Link the second object to the first */

		object->common.next_object = second_object;//将二级对象链接到主对象的next_object指针
		break;

	default://默认情况：不需要二级对象的普通类型

		/* All others have no secondary object */
		break;//空操作，仅用于语法完整性
	}

	/* Save the object type in the object descriptor */

	object->common.type = (u8) type;//设置主对象的类型字段,使用传入的类型参数，强制转换为u8类型

	/* Init the reference count */

	object->common.reference_count = 1;//初始化主对象的引用计数,新创建对象的初始引用计数为1(由创建者持有)

	/* Any per-type initialization should go here */

	return_PTR(object);//返回创建的对象指针,使用return_PTR宏以便在调试模式下记录返回信息
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_package_object
 *
 * PARAMETERS:  count               - Number of package elements
 *
 * RETURN:      Pointer to a new Package object, null on failure
 *
 * DESCRIPTION: Create a fully initialized package object
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ut_create_package_object(u32 count)
{
	union acpi_operand_object *package_desc;
	union acpi_operand_object **package_elements;

	ACPI_FUNCTION_TRACE_U32(ut_create_package_object, count);

	/* Create a new Package object */

	package_desc = acpi_ut_create_internal_object(ACPI_TYPE_PACKAGE);
	if (!package_desc) {
		return_PTR(NULL);
	}

	/*
	 * Create the element array. Count+1 allows the array to be null
	 * terminated.
	 */
	package_elements = ACPI_ALLOCATE_ZEROED(((acpi_size)count +
						 1) * sizeof(void *));
	if (!package_elements) {
		ACPI_FREE(package_desc);
		return_PTR(NULL);
	}

	package_desc->package.count = count;
	package_desc->package.elements = package_elements;
	return_PTR(package_desc);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_integer_object
 *
 * PARAMETERS:  initial_value       - Initial value for the integer
 *
 * RETURN:      Pointer to a new Integer object, null on failure
 *
 * DESCRIPTION: Create an initialized integer object
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ut_create_integer_object(u64 initial_value)
{
	union acpi_operand_object *integer_desc;

	ACPI_FUNCTION_TRACE(ut_create_integer_object);

	/* Create and initialize a new integer object */

	integer_desc = acpi_ut_create_internal_object(ACPI_TYPE_INTEGER);
	if (!integer_desc) {
		return_PTR(NULL);
	}

	integer_desc->integer.value = initial_value;
	return_PTR(integer_desc);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_buffer_object
 *
 * PARAMETERS:  buffer_size            - Size of buffer to be created
 *
 * RETURN:      Pointer to a new Buffer object, null on failure
 *
 * DESCRIPTION: Create a fully initialized buffer object
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ut_create_buffer_object(acpi_size buffer_size)
{
	union acpi_operand_object *buffer_desc;
	u8 *buffer = NULL;

	ACPI_FUNCTION_TRACE_U32(ut_create_buffer_object, buffer_size);

	/* Create a new Buffer object */

	buffer_desc = acpi_ut_create_internal_object(ACPI_TYPE_BUFFER);
	if (!buffer_desc) {
		return_PTR(NULL);
	}

	/* Create an actual buffer only if size > 0 */

	if (buffer_size > 0) {

		/* Allocate the actual buffer */

		buffer = ACPI_ALLOCATE_ZEROED(buffer_size);
		if (!buffer) {
			ACPI_ERROR((AE_INFO, "Could not allocate size %u",
				    (u32)buffer_size));

			acpi_ut_remove_reference(buffer_desc);
			return_PTR(NULL);
		}
	}

	/* Complete buffer object initialization */

	buffer_desc->buffer.flags |= AOPOBJ_DATA_VALID;
	buffer_desc->buffer.pointer = buffer;
	buffer_desc->buffer.length = (u32) buffer_size;

	/* Return the new buffer descriptor */

	return_PTR(buffer_desc);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_string_object
 *
 * PARAMETERS:  string_size         - Size of string to be created. Does not
 *                                    include NULL terminator, this is added
 *                                    automatically.
 *
 * RETURN:      Pointer to a new String object
 *
 * DESCRIPTION: Create a fully initialized string object
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ut_create_string_object(acpi_size string_size)
{
	union acpi_operand_object *string_desc;
	char *string;

	ACPI_FUNCTION_TRACE_U32(ut_create_string_object, string_size);

	/* Create a new String object */

	string_desc = acpi_ut_create_internal_object(ACPI_TYPE_STRING);
	if (!string_desc) {
		return_PTR(NULL);
	}

	/*
	 * Allocate the actual string buffer -- (Size + 1) for NULL terminator.
	 * NOTE: Zero-length strings are NULL terminated
	 */
	string = ACPI_ALLOCATE_ZEROED(string_size + 1);
	if (!string) {
		ACPI_ERROR((AE_INFO, "Could not allocate size %u",
			    (u32)string_size));

		acpi_ut_remove_reference(string_desc);
		return_PTR(NULL);
	}

	/* Complete string object initialization */

	string_desc->string.pointer = string;
	string_desc->string.length = (u32) string_size;

	/* Return the new string descriptor */

	return_PTR(string_desc);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_valid_internal_object
 *
 * PARAMETERS:  object              - Object to be validated
 *
 * RETURN:      TRUE if object is valid, FALSE otherwise
 *
 * DESCRIPTION: Validate a pointer to be of type union acpi_operand_object
 *
 ******************************************************************************/

u8 acpi_ut_valid_internal_object(void *object)
{

	ACPI_FUNCTION_NAME(ut_valid_internal_object);

	/* Check for a null pointer */

	if (!object) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "**** Null Object Ptr\n"));
		return (FALSE);
	}

	/* Check the descriptor type field */

	switch (ACPI_GET_DESCRIPTOR_TYPE(object)) {
	case ACPI_DESC_TYPE_OPERAND:

		/* The object appears to be a valid union acpi_operand_object */

		return (TRUE);

	default:

		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "%p is not an ACPI operand obj [%s]\n",
				  object, acpi_ut_get_descriptor_name(object)));
		break;
	}

	return (FALSE);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_allocate_object_desc_dbg
 *
 * PARAMETERS:  module_name         - Caller's module name (for error output)
 *              line_number         - Caller's line number (for error output)
 *              component_id        - Caller's component ID (for error output)
 *
 * RETURN:      Pointer to newly allocated object descriptor. Null on error
 *
 * DESCRIPTION: Allocate a new object descriptor. Gracefully handle
 *              error conditions.
 *
 ******************************************************************************/
/**
 * acpi_ut_allocate_object_desc_dbg - 分配ACPI操作对象描述符
 * @module_name: 调用模块名称(用于错误报告)
 * @line_number: 源代码行号(用于错误报告)
 * @component_id: 组件标识符(用于调试分类)
 *
 * 返回值:
 *  成功: 返回新分配的操作对象指针
 *  失败: 返回NULL指针
 *
 * 功能说明:
 * 1. 从ACPI操作对象缓存池中分配内存
 * 2. 设置对象描述符类型标记
 * 3. 记录分配调试信息
 * 4. 错误情况下记录详细错误日志
 */
void *acpi_ut_allocate_object_desc_dbg(const char *module_name,
				       u32 line_number, u32 component_id)
{
	union acpi_operand_object *object;//声明操作对象指针

	ACPI_FUNCTION_TRACE(ut_allocate_object_desc_dbg);

	object = acpi_os_acquire_object(acpi_gbl_operand_cache);//从ACPI操作对象缓存池中获取对象,acpi_gbl_operand_cache是全局对象缓存
	if (!object) {//检查对象分配是否成功
		ACPI_ERROR((module_name, line_number,
			    "Could not allocate an object descriptor"));//分配失败记录错误信息,包含模块名、行号等调试信息

		return_PTR(NULL);
	}

	/* Mark the descriptor type */

	ACPI_SET_DESCRIPTOR_TYPE(object, ACPI_DESC_TYPE_OPERAND);//设置对象描述符类型,标记为ACPI_DESC_TYPE_OPERAND表示标准操作对象

	ACPI_DEBUG_PRINT((ACPI_DB_ALLOCATIONS, "%p Size %X\n",
			  object, (u32) sizeof(union acpi_operand_object)));

	return_PTR(object);//返回分配的对象指针
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_delete_object_desc
 *
 * PARAMETERS:  object          - An Acpi internal object to be deleted
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Free an ACPI object descriptor or add it to the object cache
 *
 ******************************************************************************/

void acpi_ut_delete_object_desc(union acpi_operand_object *object)
{
	ACPI_FUNCTION_TRACE_PTR(ut_delete_object_desc, object);

	/* Object must be of type union acpi_operand_object */

	if (ACPI_GET_DESCRIPTOR_TYPE(object) != ACPI_DESC_TYPE_OPERAND) {
		ACPI_ERROR((AE_INFO,
			    "%p is not an ACPI Operand object [%s]", object,
			    acpi_ut_get_descriptor_name(object)));
		return_VOID;
	}

	(void)acpi_os_release_object(acpi_gbl_operand_cache, object);
	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_get_simple_object_size
 *
 * PARAMETERS:  internal_object    - An ACPI operand object
 *              obj_length         - Where the length is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: This function is called to determine the space required to
 *              contain a simple object for return to an external user.
 *
 *              The length includes the object structure plus any additional
 *              needed space.
 *
 ******************************************************************************/

static acpi_status
acpi_ut_get_simple_object_size(union acpi_operand_object *internal_object,
			       acpi_size *obj_length)
{
	acpi_size length;
	acpi_size size;
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE_PTR(ut_get_simple_object_size, internal_object);

	/* Start with the length of the (external) Acpi object */

	length = sizeof(union acpi_object);

	/* A NULL object is allowed, can be a legal uninitialized package element */

	if (!internal_object) {
	/*
		 * Object is NULL, just return the length of union acpi_object
		 * (A NULL union acpi_object is an object of all zeroes.)
	 */
		*obj_length = ACPI_ROUND_UP_TO_NATIVE_WORD(length);
		return_ACPI_STATUS(AE_OK);
	}

	/* A Namespace Node should never appear here */

	if (ACPI_GET_DESCRIPTOR_TYPE(internal_object) == ACPI_DESC_TYPE_NAMED) {

		/* A namespace node should never get here */

		ACPI_ERROR((AE_INFO,
			    "Received a namespace node [%4.4s] "
			    "where an operand object is required",
			    ACPI_CAST_PTR(struct acpi_namespace_node,
					  internal_object)->name.ascii));
		return_ACPI_STATUS(AE_AML_INTERNAL);
	}

	/*
	 * The final length depends on the object type
	 * Strings and Buffers are packed right up against the parent object and
	 * must be accessed bytewise or there may be alignment problems on
	 * certain processors
	 */
	switch (internal_object->common.type) {
	case ACPI_TYPE_STRING:

		length += (acpi_size)internal_object->string.length + 1;
		break;

	case ACPI_TYPE_BUFFER:

		length += (acpi_size)internal_object->buffer.length;
		break;

	case ACPI_TYPE_INTEGER:
	case ACPI_TYPE_PROCESSOR:
	case ACPI_TYPE_POWER:

		/* No extra data for these types */

		break;

	case ACPI_TYPE_LOCAL_REFERENCE:

		switch (internal_object->reference.class) {
		case ACPI_REFCLASS_NAME:
			/*
			 * Get the actual length of the full pathname to this object.
			 * The reference will be converted to the pathname to the object
			 */
			size =
			    acpi_ns_get_pathname_length(internal_object->
							reference.node);
			if (!size) {
				return_ACPI_STATUS(AE_BAD_PARAMETER);
			}

			length += ACPI_ROUND_UP_TO_NATIVE_WORD(size);
			break;

		default:
			/*
			 * No other reference opcodes are supported.
			 * Notably, Locals and Args are not supported, but this may be
			 * required eventually.
			 */
			ACPI_ERROR((AE_INFO,
				    "Cannot convert to external object - "
				    "unsupported Reference Class [%s] 0x%X in object %p",
				    acpi_ut_get_reference_name(internal_object),
				    internal_object->reference.class,
				    internal_object));
			status = AE_TYPE;
			break;
		}
		break;

	default:

		ACPI_ERROR((AE_INFO, "Cannot convert to external object - "
			    "unsupported type [%s] 0x%X in object %p",
			    acpi_ut_get_object_type_name(internal_object),
			    internal_object->common.type, internal_object));
		status = AE_TYPE;
		break;
	}

	/*
	 * Account for the space required by the object rounded up to the next
	 * multiple of the machine word size. This keeps each object aligned
	 * on a machine word boundary. (preventing alignment faults on some
	 * machines.)
	 */
	*obj_length = ACPI_ROUND_UP_TO_NATIVE_WORD(length);
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_get_element_length
 *
 * PARAMETERS:  acpi_pkg_callback
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Get the length of one package element.
 *
 ******************************************************************************/

static acpi_status
acpi_ut_get_element_length(u8 object_type,
			   union acpi_operand_object *source_object,
			   union acpi_generic_state *state, void *context)
{
	acpi_status status = AE_OK;
	struct acpi_pkg_info *info = (struct acpi_pkg_info *)context;
	acpi_size object_space;

	switch (object_type) {
	case ACPI_COPY_TYPE_SIMPLE:
		/*
		 * Simple object - just get the size (Null object/entry is handled
		 * here also) and sum it into the running package length
		 */
		status =
		    acpi_ut_get_simple_object_size(source_object,
						   &object_space);
		if (ACPI_FAILURE(status)) {
			return (status);
		}

		info->length += object_space;
		break;

	case ACPI_COPY_TYPE_PACKAGE:

		/* Package object - nothing much to do here, let the walk handle it */

		info->num_packages++;
		state->pkg.this_target_obj = NULL;
		break;

	default:

		/* No other types allowed */

		return (AE_BAD_PARAMETER);
	}

	return (status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_get_package_object_size
 *
 * PARAMETERS:  internal_object     - An ACPI internal object
 *              obj_length          - Where the length is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: This function is called to determine the space required to
 *              contain a package object for return to an external user.
 *
 *              This is moderately complex since a package contains other
 *              objects including packages.
 *
 ******************************************************************************/

static acpi_status
acpi_ut_get_package_object_size(union acpi_operand_object *internal_object,
				acpi_size *obj_length)
{
	acpi_status status;
	struct acpi_pkg_info info;

	ACPI_FUNCTION_TRACE_PTR(ut_get_package_object_size, internal_object);

	info.length = 0;
	info.object_space = 0;
	info.num_packages = 1;

	status =
	    acpi_ut_walk_package_tree(internal_object, NULL,
				      acpi_ut_get_element_length, &info);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/*
	 * We have handled all of the objects in all levels of the package.
	 * just add the length of the package objects themselves.
	 * Round up to the next machine word.
	 */
	info.length +=
	    ACPI_ROUND_UP_TO_NATIVE_WORD(sizeof(union acpi_object)) *
	    (acpi_size)info.num_packages;

	/* Return the total package length */

	*obj_length = info.length;
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_get_object_size
 *
 * PARAMETERS:  internal_object     - An ACPI internal object
 *              obj_length          - Where the length will be returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: This function is called to determine the space required to
 *              contain an object for return to an API user.
 *
 ******************************************************************************/

acpi_status
acpi_ut_get_object_size(union acpi_operand_object *internal_object,
			acpi_size *obj_length)
{
	acpi_status status;

	ACPI_FUNCTION_ENTRY();

	if ((ACPI_GET_DESCRIPTOR_TYPE(internal_object) ==
	     ACPI_DESC_TYPE_OPERAND) &&
	    (internal_object->common.type == ACPI_TYPE_PACKAGE)) {
		status =
		    acpi_ut_get_package_object_size(internal_object,
						    obj_length);
	} else {
		status =
		    acpi_ut_get_simple_object_size(internal_object, obj_length);
	}

	return (status);
}
