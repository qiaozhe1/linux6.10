// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: exresnte - AML Interpreter object resolution
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acdispat.h"
#include "acinterp.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_EXECUTER
ACPI_MODULE_NAME("exresnte")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_resolve_node_to_value
 *
 * PARAMETERS:  object_ptr      - Pointer to a location that contains
 *                                a pointer to a NS node, and will receive a
 *                                pointer to the resolved object.
 *              walk_state      - Current state. Valid only if executing AML
 *                                code. NULL if simply resolving an object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Resolve a Namespace node to a valued object
 *
 * Note: for some of the data types, the pointer attached to the Node
 * can be either a pointer to an actual internal object or a pointer into the
 * AML stream itself. These types are currently:
 *
 *      ACPI_TYPE_INTEGER
 *      ACPI_TYPE_STRING
 *      ACPI_TYPE_BUFFER
 *      ACPI_TYPE_MUTEX
 *      ACPI_TYPE_PACKAGE
 *
 ******************************************************************************/
acpi_status
acpi_ex_resolve_node_to_value(struct acpi_namespace_node **object_ptr,
			      struct acpi_walk_state *walk_state)
{
	acpi_status status = AE_OK;//初始化返回状态为成功
	union acpi_operand_object *source_desc;//源对象描述符指针
	union acpi_operand_object *obj_desc = NULL;//目标对象描述符指针（初始为NULL）
	struct acpi_namespace_node *node;//本地节点指针
	acpi_object_type entry_type;//节点类型缓存

	ACPI_FUNCTION_TRACE(ex_resolve_node_to_value);

	/*
	 * The stack pointer points to a struct acpi_namespace_node (Node). Get the
	 * object that is attached to the Node.
	 * 栈指针指向一个struct acpi_namespace_node（节点）。获取附加在该节点上的对象。
	 */
	node = *object_ptr;//获取实际节点指针
	source_desc = acpi_ns_get_attached_object(node);//获取节点附加对象（可能为NULL）
	entry_type = acpi_ns_get_type((acpi_handle)node);//获取节点类型（通过ACPI句柄转换）

	ACPI_DEBUG_PRINT((ACPI_DB_EXEC, "Entry=%p SourceDesc=%p [%s]\n",
			  node, source_desc,
			  acpi_ut_get_type_name(entry_type)));

	if ((entry_type == ACPI_TYPE_LOCAL_ALIAS) ||//如果是普通别名类型
	    (entry_type == ACPI_TYPE_LOCAL_METHOD_ALIAS)) {//或者是方法别名类型

		/* There is always exactly one level of indirection */
		/* 总是存在且仅存在一层间接引用 */
		node = ACPI_CAST_PTR(struct acpi_namespace_node, node->object);//通过object字段获取真实节点
		source_desc = acpi_ns_get_attached_object(node);//重新获取真实节点的附加对象
		entry_type = acpi_ns_get_type((acpi_handle)node);//获取真实节点类型
		*object_ptr = node;//更新输入参数指向真实节点
	}

	/*
	 * Several object types require no further processing:
	 * 1) Device/Thermal objects don't have a "real" subobject, return Node
	 * 2) Method locals and arguments have a pseudo-Node
	 * 3) 10/2007: Added method type to assist with Package construction.
	 * 以下几种对象类型不需要进一步处理：
	 * 1) 设备/散热对象没有"真实"子对象，直接返回节点本身
	 * 2) 方法局部变量和参数具有伪节点结构
	 * 3) 2007年10月：添加方法类型以辅助包构造
	 */
	if ((entry_type == ACPI_TYPE_DEVICE) ||//ACPI设备对象
	    (entry_type == ACPI_TYPE_THERMAL) ||//散热对象
	    (entry_type == ACPI_TYPE_METHOD) ||//控制方法
	    (node->flags & (ANOBJ_METHOD_ARG | ANOBJ_METHOD_LOCAL))) {//方法参数或局部变量标志
		return_ACPI_STATUS(AE_OK);//直接返回成功（不修改object_ptr）
	}

	/* 检查节点是否已初始化（必须有附加对象） */
	if (!source_desc) {//如果附加对象为空
		ACPI_ERROR((AE_INFO, "No object attached to node [%4.4s] %p",
			    node->name.ascii, node));
		return_ACPI_STATUS(AE_AML_UNINITIALIZED_NODE);
	}

	/*
	 * Action is based on the type of the Node, which indicates the type
	 * of the attached object or pointer
	 */
	switch (entry_type) {
	case ACPI_TYPE_PACKAGE://处理Package对象（复合对象，类似数组）
		/* 类型安全验证：确保附加对象确实是Package类型 */
		if (source_desc->common.type != ACPI_TYPE_PACKAGE) {//如果不是Package类型
			ACPI_ERROR((AE_INFO, "Object not a Package, type %s",
				    acpi_ut_get_object_type_name(source_desc)));
			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);//返回AML操作数类型错误
		}

		status = acpi_ds_get_package_arguments(source_desc);//展开Package内容，初始化Package参数（例如解析元素列表）
		if (ACPI_SUCCESS(status)) {

			/* Return an additional reference to the object */

			obj_desc = source_desc;//指向已存在的Package对象
			acpi_ut_add_reference(obj_desc);//引用计数+1（防止提前释放）
		}
		break;//结束Package处理分支

	case ACPI_TYPE_BUFFER://处理Buffer对象
		/* 类型验证：确认附加对象为Buffer */
		if (source_desc->common.type != ACPI_TYPE_BUFFER) {
			ACPI_ERROR((AE_INFO, "Object not a Buffer, type %s",
				    acpi_ut_get_object_type_name(source_desc)));
			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);
		}

		status = acpi_ds_get_buffer_arguments(source_desc);//解析Buffer内容，初始化Buffer（例如填充数据指针和长度）
		if (ACPI_SUCCESS(status)) {

			/* Return an additional reference to the object */

			obj_desc = source_desc;//获取对象指针
			acpi_ut_add_reference(obj_desc);//增加引用计数（从0到1）
		}
		break;

	case ACPI_TYPE_STRING:// 处理字符串对象
		/* 类型验证：确保是字符串类型 */
		if (source_desc->common.type != ACPI_TYPE_STRING) {
			ACPI_ERROR((AE_INFO, "Object not a String, type %s",
				    acpi_ut_get_object_type_name(source_desc)));
			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);
		}

		/* Return an additional reference to the object */
		/*  字符串直接传递引用：由于字符串不可变，无需额外处理内容 */
		obj_desc = source_desc;//获取字符串对象指针
		acpi_ut_add_reference(obj_desc);//引用计数+1
		break;

	case ACPI_TYPE_INTEGER:// 处理64位整型对象
		/* 类型验证：确认是整数类型 */
		if (source_desc->common.type != ACPI_TYPE_INTEGER) {
			ACPI_ERROR((AE_INFO, "Object not a Integer, type %s",
				    acpi_ut_get_object_type_name(source_desc)));
			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);
		}

		/* Return an additional reference to the object */
		/* 整型直接传递：无需解析内容 */
		obj_desc = source_desc;//获取整型对象
		acpi_ut_add_reference(obj_desc);//增加引用
		break;

	/* 字段类型处理组（需要硬件访问） */
	case ACPI_TYPE_BUFFER_FIELD://缓冲区字段
	case ACPI_TYPE_LOCAL_REGION_FIELD:// 操作区域字段（如EC空间）
	case ACPI_TYPE_LOCAL_BANK_FIELD://bank字段（多级寄存器访问）
	case ACPI_TYPE_LOCAL_INDEX_FIELD://索引字段（带索引的寄存器）

		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "FieldRead Node=%p SourceDesc=%p Type=%X\n",
				  node, source_desc, entry_type));

		status =
		    acpi_ex_read_data_from_field(walk_state, source_desc,
						 &obj_desc);//从硬件字段读取数据
		break;

		/* For these objects, just return the object attached to the Node */

	/* 直接返回型对象组（无需转换） */
	case ACPI_TYPE_MUTEX://互斥锁对象（同步原语）
	case ACPI_TYPE_POWER://电源资源对象（控制电源状态）
	case ACPI_TYPE_PROCESSOR://处理器对象（CPU相关操作）
	case ACPI_TYPE_EVENT://事件对象（通知机制）
	case ACPI_TYPE_REGION:// 操作区域对象（内存/IO空间映射）

		/* Return an additional reference to the object */

		obj_desc = source_desc;//获取对象指针
		acpi_ut_add_reference(obj_desc);//增加引用计数
		break;

		/* TYPE_ANY is untyped, and thus there is no object associated with it */

	case ACPI_TYPE_ANY://未定义类型（通常表示初始化错误）

		ACPI_ERROR((AE_INFO,
			    "Untyped entry %p, no attached object!", node));

		return_ACPI_STATUS(AE_AML_OPERAND_TYPE);//返回AML操作数类型错误，不能使用AE_TYPE，需符合ACPI错误处理规范

	case ACPI_TYPE_LOCAL_REFERENCE://引用类型（类似指针）

		/* 根据引用子类型处理 */
		switch (source_desc->reference.class) {//检查引用类别
		case ACPI_REFCLASS_TABLE:	// 数据表引用（如SSDT/XSDT）
		case ACPI_REFCLASS_REFOF://对象引用（指向另一个对象）
		case ACPI_REFCLASS_INDEX://索引引用（Package/Buffer索引）

			/* Return an additional reference to the object */

			obj_desc = source_desc;//获取引用对象
			acpi_ut_add_reference(obj_desc);//增加引用
			break;

		default://不支持的引用类型

			/* No named references are allowed here */

			ACPI_ERROR((AE_INFO,
				    "Unsupported Reference type 0x%X",
				    source_desc->reference.class));

			return_ACPI_STATUS(AE_AML_OPERAND_TYPE);//返回AML操作数类型错误
		}
		break;

	default://未知类型处理分支

		/* Default case is for unknown types */

		ACPI_ERROR((AE_INFO,
			    "Node %p - Unknown object type 0x%X",
			    node, entry_type));

		return_ACPI_STATUS(AE_AML_OPERAND_TYPE);//返回标准操作数类型错误

	}			/* switch (entry_type) */

	/* Return the object descriptor */

	*object_ptr = (void *)obj_desc;//返回找到的对象
	return_ACPI_STATUS(status);
}
