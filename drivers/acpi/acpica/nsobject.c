// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: nsobject - Utilities for objects attached to namespace
 *                         table entries
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsobject")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_attach_object
 *
 * PARAMETERS:  node                - Parent Node
 *              object              - Object to be attached
 *              type                - Type of object, or ACPI_TYPE_ANY if not
 *                                    known
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Record the given object as the value associated with the
 *              name whose acpi_handle is passed. If Object is NULL
 *              and Type is ACPI_TYPE_ANY, set the name as having no value.
 *              Note: Future may require that the Node->Flags field be passed
 *              as a parameter.
 *
 * MUTEX:       Assumes namespace is locked
 *
 ******************************************************************************/
/**
 * acpi_ns_attach_object - 将ACPI对象附加到命名空间节点
 * @node: 目标命名空间节点
 * @object: 要附加的对象(可以是操作对象或命名节点)
 * @type: 对象类型(当object为NULL时必须为ACPI_TYPE_ANY)
 *
 * 返回值:
 *  AE_OK - 操作成功
 *  AE_BAD_PARAMETER - 参数无效
 *
 * 功能说明:
 * 1. 参数验证和错误检查
 * 2. 处理不同类型对象的附加逻辑
 * 3. 管理对象引用计数
 * 4. 处理对象链表关系
 */
acpi_status
acpi_ns_attach_object(struct acpi_namespace_node *node,
		      union acpi_operand_object *object, acpi_object_type type)
{
	union acpi_operand_object *obj_desc;//实际要附加的对象
	union acpi_operand_object *last_obj_desc;//对象链表的最后一个对象
	acpi_object_type object_type = ACPI_TYPE_ANY;//最终对象类型

	ACPI_FUNCTION_TRACE(ns_attach_object);

	/*
	 * Parameter validation
	 */
	if (!node) {

		/* Invalid handle */

		ACPI_ERROR((AE_INFO, "Null NamedObj handle"));
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	if (!object && (ACPI_TYPE_ANY != type)) {//检查对象指针和类型是否匹配(不允许NULL对象+非ANY类型)

		/* Null object */

		ACPI_ERROR((AE_INFO,
			    "Null object, but type not ACPI_TYPE_ANY"));
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	if (ACPI_GET_DESCRIPTOR_TYPE(node) != ACPI_DESC_TYPE_NAMED) {//验证节点描述符类型是否为命名节点

		/* Not a name handle */

		ACPI_ERROR((AE_INFO, "Invalid handle %p [%s]",
			    node, acpi_ut_get_descriptor_name(node)));
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* Check if this object is already attached */

	if (node->object == object) {//检查是否已经附加了相同的对象
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "Obj %p already installed in NameObj %p\n",
				  object, node));

		return_ACPI_STATUS(AE_OK);
	}

	/* If null object, we will just install it */

	if (!object) {//处理NULL对象的情况(仅清除现有对象)
		obj_desc = NULL;
		object_type = ACPI_TYPE_ANY;
	}

	/*
	 * If the source object is a namespace Node with an attached object,
	 * we will use that (attached) object
	 * 处理传递的是命名节点的情况
	 */
	else if ((ACPI_GET_DESCRIPTOR_TYPE(object) == ACPI_DESC_TYPE_NAMED) &&
		 ((struct acpi_namespace_node *)object)->object) {
		/*
		 * Value passed is a name handle and that name has a
		 * non-null value. Use that name's value and type.
		 */
		//使用命名节点关联的实际对象
		obj_desc = ((struct acpi_namespace_node *)object)->object;
		object_type = ((struct acpi_namespace_node *)object)->type;
	}

	/*
	 * Otherwise, we will use the parameter object, but we must type
	 * it first
	 */
	else {//处理普通操作对象的情况 
		obj_desc = (union acpi_operand_object *)object;

		/* Use the given type */

		object_type = type;//使用传入的类型参数
	}

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Installing %p into Node %p [%4.4s]\n",
			  obj_desc, node, acpi_ut_get_node_name(node)));

	/* Detach an existing attached object if present */

	if (node->object) {//如果节点已有附加对象，先分离它
		acpi_ns_detach_object(node);
	}

	if (obj_desc) {//处理新对象的附加逻辑
		/*
		 * Must increment the new value's reference count
		 * (if it is an internal object)
		 */
		acpi_ut_add_reference(obj_desc);//增加新对象的引用计数 

		/*
		 * Handle objects with multiple descriptors - walk
		 * to the end of the descriptor list
		 */
		/* 处理多对象链表(找到链表末尾) */
		last_obj_desc = obj_desc;
		while (last_obj_desc->common.next_object) {
			last_obj_desc = last_obj_desc->common.next_object;
		}

		/* Install the object at the front of the object list */

		last_obj_desc->common.next_object = node->object;//将原有对象链接到新对象链表的末尾
	}

	/* 更新节点的类型和对象指针 */
	node->type = (u8) object_type;
	node->object = obj_desc;

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_detach_object
 *
 * PARAMETERS:  node           - A Namespace node whose object will be detached
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Detach/delete an object associated with a namespace node.
 *              if the object is an allocated object, it is freed.
 *              Otherwise, the field is simply cleared.
 *
 ******************************************************************************/

void acpi_ns_detach_object(struct acpi_namespace_node *node)
{
	union acpi_operand_object *obj_desc;

	ACPI_FUNCTION_TRACE(ns_detach_object);

	obj_desc = node->object;

	if (!obj_desc || (obj_desc->common.type == ACPI_TYPE_LOCAL_DATA)) {
		return_VOID;
	}

	if (node->flags & ANOBJ_ALLOCATED_BUFFER) {

		/* Free the dynamic aml buffer */

		if (obj_desc->common.type == ACPI_TYPE_METHOD) {
			ACPI_FREE(obj_desc->method.aml_start);
		}
	}

	if (obj_desc->common.type == ACPI_TYPE_REGION) {
		acpi_ut_remove_address_range(obj_desc->region.space_id, node);
	}

	/* Clear the Node entry in all cases */

	node->object = NULL;
	if (ACPI_GET_DESCRIPTOR_TYPE(obj_desc) == ACPI_DESC_TYPE_OPERAND) {

		/* Unlink object from front of possible object list */

		node->object = obj_desc->common.next_object;

		/* Handle possible 2-descriptor object */

		if (node->object &&
		    (node->object->common.type != ACPI_TYPE_LOCAL_DATA)) {
			node->object = node->object->common.next_object;
		}

		/*
		 * Detach the object from any data objects (which are still held by
		 * the namespace node)
		 */
		if (obj_desc->common.next_object &&
		    ((obj_desc->common.next_object)->common.type ==
		     ACPI_TYPE_LOCAL_DATA)) {
			obj_desc->common.next_object = NULL;
		}
	}

	/* Reset the node type to untyped */

	node->type = ACPI_TYPE_ANY;

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES, "Node %p [%4.4s] Object %p\n",
			  node, acpi_ut_get_node_name(node), obj_desc));

	/* Remove one reference on the object (and all subobjects) */

	acpi_ut_remove_reference(obj_desc);
	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_attached_object
 *
 * PARAMETERS:  node             - Namespace node
 *
 * RETURN:      Current value of the object field from the Node whose
 *              handle is passed
 *
 * DESCRIPTION: Obtain the object attached to a namespace node.
 *
 ******************************************************************************/
/**
 * acpi_ns_get_attached_object - 获取命名空间节点关联的操作对象
 * @node: 要查询的ACPI命名空间节点指针
 *
 * 返回值:
 *   union acpi_operand_object* - 有效的操作对象指针
 *   NULL - 节点无效或没有关联有效对象
 *
 * 功能说明:
 * 1. 参数有效性检查
 * 2. 对象描述符类型验证
 * 3. 特殊类型过滤(ACPI_TYPE_LOCAL_DATA)
 * 4. 返回关联对象的引用(不增加引用计数)
 */
union acpi_operand_object *acpi_ns_get_attached_object(struct
						       acpi_namespace_node
						       *node)
{
	ACPI_FUNCTION_TRACE_PTR(ns_get_attached_object, node);

	if (!node) {
		ACPI_WARNING((AE_INFO, "Null Node ptr"));
		return_PTR(NULL);
	}

	if (!node->object ||//对象指针为空
	    /* 检查描述符类型有效性 */
	    ((ACPI_GET_DESCRIPTOR_TYPE(node->object) != ACPI_DESC_TYPE_OPERAND)
	     && (ACPI_GET_DESCRIPTOR_TYPE(node->object) !=
		 ACPI_DESC_TYPE_NAMED))
	    || ((node->object)->common.type == ACPI_TYPE_LOCAL_DATA)) {//过滤特殊数据类型
		return_PTR(NULL);//返回空指针
	}

	return_PTR(node->object);//返回关联的对象指针
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_secondary_object
 *
 * PARAMETERS:  node             - Namespace node
 *
 * RETURN:      Current value of the object field from the Node whose
 *              handle is passed.
 *
 * DESCRIPTION: Obtain a secondary object associated with a namespace node.
 *
 ******************************************************************************/

union acpi_operand_object *acpi_ns_get_secondary_object(union
							acpi_operand_object
							*obj_desc)
{
	ACPI_FUNCTION_TRACE_PTR(ns_get_secondary_object, obj_desc);

	if ((!obj_desc) ||
	    (obj_desc->common.type == ACPI_TYPE_LOCAL_DATA) ||
	    (!obj_desc->common.next_object) ||
	    ((obj_desc->common.next_object)->common.type ==
	     ACPI_TYPE_LOCAL_DATA)) {
		return_PTR(NULL);
	}

	return_PTR(obj_desc->common.next_object);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_attach_data
 *
 * PARAMETERS:  node            - Namespace node
 *              handler         - Handler to be associated with the data
 *              data            - Data to be attached
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Low-level attach data. Create and attach a Data object.
 *
 ******************************************************************************/

acpi_status
acpi_ns_attach_data(struct acpi_namespace_node *node,
		    acpi_object_handler handler, void *data)
{
	union acpi_operand_object *prev_obj_desc;
	union acpi_operand_object *obj_desc;
	union acpi_operand_object *data_desc;

	/* We only allow one attachment per handler */

	prev_obj_desc = NULL;
	obj_desc = node->object;
	while (obj_desc) {
		if ((obj_desc->common.type == ACPI_TYPE_LOCAL_DATA) &&
		    (obj_desc->data.handler == handler)) {
			return (AE_ALREADY_EXISTS);
		}

		prev_obj_desc = obj_desc;
		obj_desc = obj_desc->common.next_object;
	}

	/* Create an internal object for the data */

	data_desc = acpi_ut_create_internal_object(ACPI_TYPE_LOCAL_DATA);
	if (!data_desc) {
		return (AE_NO_MEMORY);
	}

	data_desc->data.handler = handler;
	data_desc->data.pointer = data;

	/* Install the data object */

	if (prev_obj_desc) {
		prev_obj_desc->common.next_object = data_desc;
	} else {
		node->object = data_desc;
	}

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_detach_data
 *
 * PARAMETERS:  node            - Namespace node
 *              handler         - Handler associated with the data
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Low-level detach data. Delete the data node, but the caller
 *              is responsible for the actual data.
 *
 ******************************************************************************/

acpi_status
acpi_ns_detach_data(struct acpi_namespace_node *node,
		    acpi_object_handler handler)
{
	union acpi_operand_object *obj_desc;
	union acpi_operand_object *prev_obj_desc;

	prev_obj_desc = NULL;
	obj_desc = node->object;
	while (obj_desc) {
		if ((obj_desc->common.type == ACPI_TYPE_LOCAL_DATA) &&
		    (obj_desc->data.handler == handler)) {
			if (prev_obj_desc) {
				prev_obj_desc->common.next_object =
				    obj_desc->common.next_object;
			} else {
				node->object = obj_desc->common.next_object;
			}

			acpi_ut_remove_reference(obj_desc);
			return (AE_OK);
		}

		prev_obj_desc = obj_desc;
		obj_desc = obj_desc->common.next_object;
	}

	return (AE_NOT_FOUND);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_get_attached_data
 *
 * PARAMETERS:  node            - Namespace node
 *              handler         - Handler associated with the data
 *              data            - Where the data is returned
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Low level interface to obtain data previously associated with
 *              a namespace node.
 *
 ******************************************************************************/

acpi_status
acpi_ns_get_attached_data(struct acpi_namespace_node *node,
			  acpi_object_handler handler, void **data)
{
	union acpi_operand_object *obj_desc;

	obj_desc = node->object;
	while (obj_desc) {
		if ((obj_desc->common.type == ACPI_TYPE_LOCAL_DATA) &&
		    (obj_desc->data.handler == handler)) {
			*data = obj_desc->data.pointer;
			return (AE_OK);
		}

		obj_desc = obj_desc->common.next_object;
	}

	return (AE_NOT_FOUND);
}
