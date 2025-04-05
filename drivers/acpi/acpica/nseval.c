// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: nseval - Object evaluation, includes control method execution
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acparser.h"
#include "acinterp.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nseval")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_evaluate
 *
 * PARAMETERS:  info            - Evaluation info block, contains these fields
 *                                and more:
 *                  prefix_node     - Prefix or Method/Object Node to execute
 *                  relative_path   - Name of method to execute, If NULL, the
 *                                    Node is the object to execute
 *                  parameters      - List of parameters to pass to the method,
 *                                    terminated by NULL. Params itself may be
 *                                    NULL if no parameters are being passed.
 *                  parameter_type  - Type of Parameter list
 *                  return_object   - Where to put method's return value (if
 *                                    any). If NULL, no value is returned.
 *                  flags           - ACPI_IGNORE_RETURN_VALUE to delete return
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute a control method or return the current value of an
 *              ACPI namespace object.
 *
 * MUTEX:       Locks interpreter
 *
 ******************************************************************************/
/* 处理从简单数据对象到复杂控制方法的所有评估请求。 */
acpi_status acpi_ns_evaluate(struct acpi_evaluate_info *info)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_evaluate);

	if (!info) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	if (!info->node) {//如果未指定目标节点
		/*
		 * Get the actual namespace node for the target object if we
		 * need to. Handles these cases:
		 *
		 * 1) Null node, valid pathname from root (absolute path)
		 * 2) Node and valid pathname (path relative to Node)
		 * 3) Node, Null pathname
		 * 获取目标对象的实际命名空间节点（处理以下情况）：
		 * 1) 空节点 + 从根开始的绝对路径
		 * 2) 节点 + 相对路径
		 * 3) 节点 + 空路径名
		 */
		status =
		    acpi_ns_get_node(info->prefix_node, info->relative_pathname,
				     ACPI_NS_NO_UPSEARCH, &info->node);
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	}

	/*
	 * For a method alias, we must grab the actual method node so that
	 * proper scoping context will be established before execution.
	 */
	if (acpi_ns_get_type(info->node) == ACPI_TYPE_LOCAL_METHOD_ALIAS) {//处理方法别名：获取实际方法节点以建立正确的作用域上下文
		info->node =
		    ACPI_CAST_PTR(struct acpi_namespace_node,
				  info->node->object);//解析别名
	}

	/* Complete the info block initialization */

	info->return_object = NULL;//初始化返回对象
	info->node_flags = info->node->flags;//保存节点标志
	info->obj_desc = acpi_ns_get_attached_object(info->node);//获取附加对象

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES, "%s [%p] Value %p\n",
			  info->relative_pathname, info->node,
			  acpi_ns_get_attached_object(info->node)));

	/* Get info if we have a predefined name (_HID, etc.) */

	info->predefined =
	    acpi_ut_match_predefined_method(info->node->name.ascii);//检查是否为预定义名称（如_HID）

	/* Get the full pathname to the object, for use in warning messages */

	info->full_pathname = acpi_ns_get_normalized_pathname(info->node, TRUE);//获取完整路径名（用于警告消息）
	if (!info->full_pathname) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  %s (%s)\n", "   Enter evaluation",
			      &info->full_pathname[1],
			      acpi_ut_get_type_name(info->node->type)));

	/* Count the number of arguments being passed in */

	info->param_count = 0;//统计传入参数数量
	if (info->parameters) {// 如果有参数数组
		while (info->parameters[info->param_count]) {//遍历直到NULL终止符
			info->param_count++;
		}

		/* Warn on impossible argument count */

		if (info->param_count > ACPI_METHOD_NUM_ARGS) {//检查参数数量是否超过最大值（7个）
			ACPI_WARN_PREDEFINED((AE_INFO, info->full_pathname,
					      ACPI_WARN_ALWAYS,
					      "Excess arguments (%u) - using only %u",
					      info->param_count,
					      ACPI_METHOD_NUM_ARGS));

			info->param_count = ACPI_METHOD_NUM_ARGS;//截断多余参数
		}
	}

	/*
	 * For predefined names: Check that the declared argument count
	 * matches the ACPI spec -- otherwise this is a BIOS error.
	 */
	acpi_ns_check_acpi_compliance(info->full_pathname, info->node,
				      info->predefined);//对预定义名称：检查参数数量是否符合ACPI规范

	/*
	 * For all names: Check that the incoming argument count for
	 * this method/object matches the actual ASL/AML definition.
	 */
	acpi_ns_check_argument_count(info->full_pathname, info->node,
				     info->param_count, info->predefined);//对所有名称：检查参数数量是否匹配ASL/AML定义

	/* For predefined names: Typecheck all incoming arguments */

	acpi_ns_check_argument_types(info);//对预定义名称：类型检查所有传入参数

	/*
	 * Three major evaluation cases:
	 *
	 * 1) Object types that cannot be evaluated by definition
	 * 2) The object is a control method -- execute it
	 * 3) The object is not a method -- just return it's current value
	 * 三大评估场景分类：
	 *
	 * 1) 按定义不可评估的对象类型
	 * 2) 控制方法对象 —— 执行它
	 * 3) 非方法对象 —— 直接返回其当前值
	 */
	switch (acpi_ns_get_type(info->node)) {
	case ACPI_TYPE_ANY:
	case ACPI_TYPE_DEVICE://仅表示设备容器
	case ACPI_TYPE_EVENT:
	case ACPI_TYPE_MUTEX://同步原语
	case ACPI_TYPE_REGION://硬件访问入口，非数据容器
	case ACPI_TYPE_THERMAL:
	case ACPI_TYPE_LOCAL_SCOPE://命名空间组织单元
		/*
		 * 1) Disallow evaluation of these object types. For these,
		 *    object evaluation is undefined.
		 *  场景1：禁止评估这些对象类型,因为这些类型本质上不存储可评估数据
		 */
		ACPI_ERROR((AE_INFO,
			    "%s: This object type [%s] "
			    "never contains data and cannot be evaluated",
			    info->full_pathname,
			    acpi_ut_get_type_name(info->node->type)));

		status = AE_TYPE;//返回类型错误
		goto cleanup;//跳转到清理流程

	case ACPI_TYPE_METHOD:
		/*
		 *  2) 控制方法 - 执行它
		 */

		/* Verify that there is a method object associated with this node */

		if (!info->obj_desc) {//检查方法对象是否存在
			ACPI_ERROR((AE_INFO,
				    "%s: Method has no attached sub-object",
				    info->full_pathname));
			status = AE_NULL_OBJECT;//空对象错误
			goto cleanup;
		}

		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,//调试输出方法执行信息
				  "**** Execute method [%s] at AML address %p length %X\n",
				  info->full_pathname,
				  info->obj_desc->method.aml_start + 1,//AML代码起始地址（跳过Opcode字节）
				  info->obj_desc->method.aml_length - 1));//有效代码长度（排除Opcode和EndOp）

		/*
		 * Any namespace deletion must acquire both the namespace and
		 * interpreter locks to ensure that no thread is using the portion of
		 * the namespace that is being deleted.
		 *
		 * Execute the method via the interpreter. The interpreter is locked
		 * here before calling into the AML parser
		 */
		/* 锁定解释器后执行AML方法 */
		acpi_ex_enter_interpreter();//获取解释器锁
		status = acpi_ps_execute_method(info);//核心方法执行函数
		acpi_ex_exit_interpreter();//释放解释器锁
		break;

	default:
		/*
		 * 3) 普通对象值获取
		 */

		/*
		 * Some objects require additional resolution steps (e.g., the Node
		 * may be a field that must be read, etc.) -- we can't just grab
		 * the object out of the node.
		 *
		 * Use resolve_node_to_value() to get the associated value.
		 *
		 * NOTE: we can get away with passing in NULL for a walk state because
		 * the Node is guaranteed to not be a reference to either a method
		 * local or a method argument (because this interface is never called
		 * from a running method.)
		 *
		 * Even though we do not directly invoke the interpreter for object
		 * resolution, we must lock it because we could access an op_region.
		 * The op_region access code assumes that the interpreter is locked.
		 */
		/*
		 * 特殊对象处理说明：
		 * - 字段(Field)对象需要实际读取硬件
		 * - 索引字段(Index Field)需要额外解析
		 * - 银行字段(Bank Field)涉及寄存器切换
		 * 因此不能直接从节点获取对象值
		 * */
		acpi_ex_enter_interpreter();//锁定解释器（可能访问op_region）

		/* TBD: resolve_node_to_value has a strange interface, fix */

		info->return_object =
		    ACPI_CAST_PTR(union acpi_operand_object, info->node);//通过节点解析获取值（特殊接口）

		status =
		    acpi_ex_resolve_node_to_value(ACPI_CAST_INDIRECT_PTR
						  (struct acpi_namespace_node,
						   &info->return_object), NULL);//无walk_state表示非方法上下文
		acpi_ex_exit_interpreter();//释放解释器锁

		if (ACPI_FAILURE(status)) {
			info->return_object = NULL;//失败时清除返回对象
			goto cleanup;
		}

		ACPI_DEBUG_PRINT((ACPI_DB_NAMES, "Returned object %p [%s]\n",
				  info->return_object,
				  acpi_ut_get_object_type_name(info->
							       return_object)));//调试输出返回值信息

		status = AE_CTRL_RETURN_VALUE;	/* 标记有返回值 */
		break;
	}

	/*
	 * For predefined names, check the return value against the ACPI
	 * specification. Some incorrect return value types are repaired.
	 */
	(void)acpi_ns_check_return_value(info->node, info, info->param_count,
					 status, &info->return_object);//对预定义名称：检查返回值是否符合规范

	/* Check if there is a return value that must be dealt with */
	/*  返回值处理逻辑 */
	if (status == AE_CTRL_RETURN_VALUE) {// 如果方法正常执行且有返回值

		/* If caller does not want the return value, delete it */
		
		if (info->flags & ACPI_IGNORE_RETURN_VALUE) {//如果调用方声明忽略返回值时
			acpi_ut_remove_reference(info->return_object);//减少对象引用计数（可能释放对象）
			info->return_object = NULL;//清空返回指针
		}

		/* Map AE_CTRL_RETURN_VALUE to AE_OK, we are done with it */

		status = AE_OK;// 将内部状态码转换为标准成功码
	} else if (ACPI_FAILURE(status)) {//如果评估过程出现错误

		/* If return_object exists, delete it */

		/*  错误时清理残留的返回对象：
		 *  - 方法可能部分执行成功
		 *  - 某些错误路径会创建临时对象
		 */
		if (info->return_object) {
			acpi_ut_remove_reference(info->return_object);
			info->return_object = NULL;
		}
	}

	ACPI_DEBUG_PRINT((ACPI_DB_NAMES,
			  "*** Completed evaluation of object %s ***\n",
			  info->relative_pathname));//调试输出评估完成信息

cleanup:
	/* Optional object evaluation log */

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_EVALUATION,
			      "%-26s:  %s\n", "   Exit evaluation",
			      &info->full_pathname[1]));//评估结束日志（显示在ACPI_DEBUG_PRINT_RAW输出中）

	/*
	 * Namespace was unlocked by the handling acpi_ns* function, so we
	 * just free the pathname and return
	 */
	ACPI_FREE(info->full_pathname);//释放动态分配的完整路径名
	info->full_pathname = NULL;//防止野指针
	return_ACPI_STATUS(status);//最终状态返回（带调试跟踪）
}
