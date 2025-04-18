// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nsinit - namespace initialization
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acdispat.h"
#include "acinterp.h"
#include "acevents.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsinit")

/* Local prototypes */
static acpi_status
acpi_ns_init_one_object(acpi_handle obj_handle,
			u32 level, void *context, void **return_value);

static acpi_status
acpi_ns_init_one_device(acpi_handle obj_handle,
			u32 nesting_level, void *context, void **return_value);

static acpi_status
acpi_ns_find_ini_methods(acpi_handle obj_handle,
			 u32 nesting_level, void *context, void **return_value);

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_initialize_objects
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Walk the entire namespace and perform any necessary
 *              initialization on the objects found therein
 *
 ******************************************************************************/
/*
 * acpi_ns_initialize_objects - 初始化ACPI命名空间中的所有对象
 *
 * 功能：
 * 1. 遍历整个ACPI命名空间
 * 2. 对每个对象执行初始化操作
 * 3. 收集命名空间统计信息
 * 4. 输出调试信息
 *
 * 返回值：
 *   AE_OK - 操作成功完成
 *   其他ACPI状态码表示错误情况
 */
acpi_status acpi_ns_initialize_objects(void)
{
	acpi_status status;
	struct acpi_init_walk_info info;//遍历命名空间时使用的信息结构体

	ACPI_FUNCTION_TRACE(ns_initialize_objects);

	/* 调试信息输出 - 显示初始化开始信息 */
	ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
			  "[Init] Completing Initialization of ACPI Objects\n"));
	ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
			  "**** Starting initialization of namespace objects ****\n"));
	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
			      "Final data object initialization: "));

	/* 第一步：初始化info信息结构体 */
	memset(&info, 0, sizeof(struct acpi_init_walk_info));//将info结构体所有成员清零

        /* 第二步：遍历整个命名空间并初始化对象 */
        /*
         * 参数说明：
         * ACPI_TYPE_ANY - 遍历所有类型的对象
         * ACPI_ROOT_OBJECT - 从命名空间根节点开始遍历
         * ACPI_UINT32_MAX - 最大遍历深度(无限制)
         * acpi_ns_init_one_object - 每个对象的初始化回调函数
         * NULL - 上下文参数(未使用)
         * &info - 传递给回调函数的信息结构体指针
         * NULL - 返回值指针(未使用)
         *
         * 注意：当前使用ACPI_TYPE_ANY，但未来可能会改为ACPI_TYPE_PACKAGE，
         * 因为只有Package类型的对象支持延迟初始化(前向引用)
         */
	status = acpi_walk_namespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT,
				     ACPI_UINT32_MAX, acpi_ns_init_one_object,
				     NULL, &info, NULL);
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status, "During WalkNamespace"));
	}

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
			      "Namespace contains %u (0x%X) objects\n",
			      info.object_count, info.object_count));//输出命名空间对象总数

	ACPI_DEBUG_PRINT((ACPI_DB_DISPATCH,
			  "%u Control Methods found\n%u Op Regions found\n",
			  info.method_count, info.op_region_count));//输出控制方法和操作区域的统计信息

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_initialize_devices
 *
 * PARAMETERS:  None
 *
 * RETURN:      acpi_status
 *
 * DESCRIPTION: Walk the entire namespace and initialize all ACPI devices.
 *              This means running _INI on all present devices.
 *
 *              Note: We install PCI config space handler on region access,
 *              not here.
 *
 ******************************************************************************/

acpi_status acpi_ns_initialize_devices(u32 flags)//初始化ACPI命名空间中的设备对象
{
	acpi_status status = AE_OK;
	struct acpi_device_walk_info info;//设备遍历信息结构体，用于收集初始化过程中的统计信息
	acpi_handle handle;//临时节点句柄，用于存储查找的ACPI对象

	ACPI_FUNCTION_TRACE(ns_initialize_devices);

	if (!(flags & ACPI_NO_DEVICE_INIT)) {//检查是否跳过设备初始化阶段,ACPI_NO_DEVICE_INIT标志表示不执行设备初始化
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "[Init] Initializing ACPI Devices\n"));

		/* 初始化遍历计数器 */
		info.device_count = 0;//已检查的设备总数
		info.num_STA = 0;//已执行的_STA方法计数
		info.num_INI = 0;//已执行的_INI方法计数

		ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
				      "Initializing Device/Processor/Thermal objects "
				      "and executing _INI/_STA methods:\n"));

		/* Tree analysis: find all subtrees that contain _INI methods */
		/* 第一阶段：遍历ACPI命名空间,查找所有包含_INI方法的子树,并标记存在_INI方法 */
		status = acpi_ns_walk_namespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT,
						ACPI_UINT32_MAX, FALSE,
						acpi_ns_find_ini_methods, NULL,
						&info, NULL);
		if (ACPI_FAILURE(status)) {//如果遍历失败，跳转到错误处理
			goto error_exit;
		}

		/* 为方法评估分配信息结构体,使用ACPI内存分配器，并初始化为全零 */
		info.evaluate_info =
		    ACPI_ALLOCATE_ZEROED(sizeof(struct acpi_evaluate_info));
		if (!info.evaluate_info) {
			status = AE_NO_MEMORY;
			goto error_exit;
		}

		/* 执行根节点的全局_INI方法（Windows兼容性支持）
		 * 注意：这不是标准ACPI规范的一部分
		 */
		info.evaluate_info->prefix_node = acpi_gbl_root_node;//设置根节点
		info.evaluate_info->relative_pathname = METHOD_NAME__INI;//_INI方法名
		info.evaluate_info->parameters = NULL;//无输入参数
		info.evaluate_info->flags = ACPI_IGNORE_RETURN_VALUE;//忽略返回值

		status = acpi_ns_evaluate(info.evaluate_info);//执行方法
		if (ACPI_SUCCESS(status)) {
			info.num_INI++;//成功执行则增加计数器
		}

		/*
		 * Execute \_SB._INI.
		 * There appears to be a strict order requirement for \_SB._INI,
		 * which should be evaluated before any _REG evaluations.
		 */
		/*
		 * 执行\_SB._INI方法（系统总线初始化）
		 * 注意：这里存在严格的执行顺序要求,必须在任何_REG评估之前执行
		 * */
		status = acpi_get_handle(NULL, "\\_SB", &handle);//获取\_SB节点的句柄
		if (ACPI_SUCCESS(status)) {
			memset(info.evaluate_info, 0,
			       sizeof(struct acpi_evaluate_info));//重置评估信息结构
			info.evaluate_info->prefix_node = handle;//设置SB节点
			info.evaluate_info->relative_pathname =
			    METHOD_NAME__INI;//设置方法名
			info.evaluate_info->parameters = NULL;//无参数
			info.evaluate_info->flags = ACPI_IGNORE_RETURN_VALUE;//忽略返回值

			status = acpi_ns_evaluate(info.evaluate_info);//执行方法
			if (ACPI_SUCCESS(status)) {
				info.num_INI++;
			}
		}
	}

	/*
	 * Run all _REG methods
	 *
	 * Note: Any objects accessed by the _REG methods will be automatically
	 * initialized, even if they contain executable AML (see the call to
	 * acpi_ns_initialize_objects below).
	 *
	 * Note: According to the ACPI specification, we actually needn't execute
	 * _REG for system_memory/system_io operation regions, but for PCI_Config
	 * operation regions, it is required to evaluate _REG for those on a PCI
	 * root bus that doesn't contain _BBN object. So this code is kept here
	 * in order not to break things.
	 */
	/*
	 * 第二阶段：执行所有_REG方法（操作区域注册）
	 * 注意：
	 * 1. 任何被_REG方法访问的对象都会自动初始化
	 * 2. 根据规范，系统内存/IO区域理论上不需要_REG
	 * 3. 但PCI配置区域需要_REG方法
	 * 4. 保留此代码以确保最大兼容性
	 * */
	if (!(flags & ACPI_NO_ADDRESS_SPACE_INIT)) {
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "[Init] Executing _REG OpRegion methods\n"));

		status = acpi_ev_initialize_op_regions();//调用操作区域初始化函数
		if (ACPI_FAILURE(status)) {
			goto error_exit;
		}
	}

	/* 第三阶段：设备初始化主流程 */
	if (!(flags & ACPI_NO_DEVICE_INIT)) {

		/* Walk namespace to execute all _INIs on present devices */
		/* 再次遍历命名空间,这次执行所有存在设备的_INI方法,
		 * 使用acpi_ns_init_one_device作为回调函数 
		 */
		status = acpi_ns_walk_namespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT,
						ACPI_UINT32_MAX, FALSE,
						acpi_ns_init_one_device, NULL,
						&info, NULL);

		/*
		 * Any _OSI requests should be completed by now. If the BIOS has
		 * requested any Windows OSI strings, we will always truncate
		 * I/O addresses to 16 bits -- for Windows compatibility.
		 * Windows兼容性处理：
		 */
		if (acpi_gbl_osi_data >= ACPI_OSI_WIN_2000) {
			acpi_gbl_truncate_io_addresses = TRUE;
		}

		ACPI_FREE(info.evaluate_info);// 释放之前分配的评估信息内存 
		if (ACPI_FAILURE(status)) {//检查初始化是否成功
			goto error_exit;
		}

		ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
				      "    Executed %u _INI methods requiring %u _STA executions "
				      "(examined %u objects)\n",
				      info.num_INI, info.num_STA,
				      info.device_count));
	}

	return_ACPI_STATUS(status);//正常执行路径，返回状态码

error_exit:
	ACPI_EXCEPTION((AE_INFO, status, "During device initialization"));
	return_ACPI_STATUS(status);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_init_one_package
 *
 * PARAMETERS:  obj_handle      - Node
 *              level           - Current nesting level
 *              context         - Not used
 *              return_value    - Not used
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Callback from acpi_walk_namespace. Invoked for every package
 *              within the namespace. Used during dynamic load of an SSDT.
 *
 ******************************************************************************/
/*
 * acpi_ns_init_one_package - 初始化ACPI包对象
 * 
 * 此函数负责完成包对象的延迟初始化，处理可能的前向引用情况
 * 
 * @obj_handle: 要初始化的包对象的命名空间节点句柄
 * @level: 命名空间树中的深度(未使用)
 * @context: 上下文参数(未使用)
 * @return_value: 返回值指针(未使用)
 * 
 * 返回值: 始终返回AE_OK，即使初始化失败也允许继续执行
 */
acpi_status
acpi_ns_init_one_package(acpi_handle obj_handle,
			 u32 level, void *context, void **return_value)
{
	acpi_status status;
	union acpi_operand_object *obj_desc;//包对象描述符
	struct acpi_namespace_node *node =
	    (struct acpi_namespace_node *)obj_handle;//转换为命名空间节点

	obj_desc = acpi_ns_get_attached_object(node);//获取附加到节点的对象描述符
	if (!obj_desc) {
		return (AE_OK);
	}

	/* Exit if package is already initialized */

	if (obj_desc->package.flags & AOPOBJ_DATA_VALID) {// 检查包是否已经初始化完成 
		return (AE_OK);
	}

	status = acpi_ds_get_package_arguments(obj_desc);//获取包参数(元素数量和类型),对于预定义包，这会解析包的固定结构
	if (ACPI_FAILURE(status)) {
		return (AE_OK);
	}

	status =
	    acpi_ut_walk_package_tree(obj_desc, NULL,
				      acpi_ds_init_package_element, NULL);//遍历包树,初始化每个元素.acpi_ds_init_package_element: 元素初始化回调
	if (ACPI_FAILURE(status)) {
		return (AE_OK);
	}

	obj_desc->package.flags |= AOPOBJ_DATA_VALID;//标记包对象为已初始化完成 
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_init_one_object
 *
 * PARAMETERS:  obj_handle      - Node
 *              level           - Current nesting level
 *              context         - Points to a init info struct
 *              return_value    - Not used
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Callback from acpi_walk_namespace. Invoked for every object
 *              within the namespace.
 *
 *              Currently, the only objects that require initialization are:
 *              1) Methods
 *              2) Op Regions
 *
 ******************************************************************************/
/*
 * acpi_ns_init_one_object - 初始化ACPI命名空间中的单个对象
 * 这是命名空间遍历的回调函数，用于初始化ACPI对象
 */
static acpi_status
acpi_ns_init_one_object(acpi_handle obj_handle,//当前空间节点句柄
			u32 level, void *context, void **return_value)
{
	acpi_object_type type;//存储当前对象的类型
	acpi_status status = AE_OK;
	struct acpi_init_walk_info *info =
	    (struct acpi_init_walk_info *)context;//转换上下文为信息结构
	struct acpi_namespace_node *node =
	    (struct acpi_namespace_node *)obj_handle;//转换句柄为命名空间节点
	union acpi_operand_object *obj_desc;//对象描述符指针

	ACPI_FUNCTION_NAME(ns_init_one_object);

	info->object_count++;//增加总对象计数器 ,统计遍历过的所有对象

	/* And even then, we are only interested in a few object types */
	/* 只处理我们感兴趣的特定对象类型,其他类型直接跳过 */
	type = acpi_ns_get_type(obj_handle);//获取命名空间类型代码
	obj_desc = acpi_ns_get_attached_object(node);//获取命名空间节点关联的对象描述符
	if (!obj_desc) {
		return (AE_OK);//如果没有附加对象，直接返回成功
	}

	/* 根据不同类型增加相应的计数器 */
	switch (type) {
	case ACPI_TYPE_REGION://操作区域对象

		info->op_region_count++;//增加操作区域计数
		break;

	case ACPI_TYPE_BUFFER_FIELD://缓冲区字段对象

		info->field_count++;//增加字段计数
		break;

	case ACPI_TYPE_LOCAL_BANK_FIELD://bank字段对象

		info->field_count++;//增加字段计数(与缓冲区字段合并统计)
		break;

	case ACPI_TYPE_BUFFER://缓冲区对象

		info->buffer_count++;//增加缓冲区计数
		break;

	case ACPI_TYPE_PACKAGE://包对象

		info->package_count++;//增加包对象计数
		break;

	default:

		/* No init required, just exit now */

		return (AE_OK);//其他类型不处理,直接返回成功
	}

	/* If the object is already initialized, nothing else to do */

	if (obj_desc->common.flags & AOPOBJ_DATA_VALID) {//检查对象是否已经初始化,AOPOBJ_DATA_VALID标志表示已完成初始化
		return (AE_OK);
	}

	/* Must lock the interpreter before executing AML code */

	acpi_ex_enter_interpreter();//执行AML代码前必须获取解释器锁,防止多线程环境下的竞争条件

	/*
	 * 对象初始化处理开关,只有特定类型需要在这里处理
	 */
	switch (type) {
	case ACPI_TYPE_LOCAL_BANK_FIELD://bank字段初始化

		/* TBD: bank_fields do not require deferred init, remove this code */

		info->field_init++;//增加已初始化字段计数
		status = acpi_ds_get_bank_field_arguments(obj_desc);//获取银行字段参数
		break;

	case ACPI_TYPE_PACKAGE://包对象初始化

		/* Complete the initialization/resolution of the package object */

		info->package_init++;//增加已初始化包计数
		status =
		    acpi_ns_init_one_package(obj_handle, level, NULL, NULL);//调用包对象初始化函数
		break;

	default:

		/* No other types should get here */
		/* 理论上不应该执行到这里,因为前面已经过滤了不需要处理的对象类型 */
		status = AE_TYPE;//设置类型错误状态
		ACPI_EXCEPTION((AE_INFO, status,
				"Opcode is not deferred [%4.4s] (%s)",
				acpi_ut_get_node_name(node),
				acpi_ut_get_type_name(type)));
		break;
	}

	/* 错误处理部分 */
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"Could not execute arguments for [%4.4s] (%s)",
				acpi_ut_get_node_name(node),
				acpi_ut_get_type_name(type)));
	}

	/*
	 * We ignore errors from above, and always return OK, since we don't want
	 * to abort the walk on any single error.
	 */
	acpi_ex_exit_interpreter();//释放解释器锁
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_find_ini_methods
 *
 * PARAMETERS:  acpi_walk_callback
 *
 * RETURN:      acpi_status
 *
 * DESCRIPTION: Called during namespace walk. Finds objects named _INI under
 *              device/processor/thermal objects, and marks the entire subtree
 *              with a SUBTREE_HAS_INI flag. This flag is used during the
 *              subsequent device initialization walk to avoid entire subtrees
 *              that do not contain an _INI.
 *
 ******************************************************************************/
/*
 * acpi_ns_find_ini_methods - 遍历ACPI命名空间查找_INI方法的回调函数
 * @obj_handle: 当前遍历到的ACPI对象句柄
 * @nesting_level: 当前命名空间层级深度
 * @context: 传入的用户上下文(这里指向acpi_device_walk_info结构)
 * @return_value: 用于返回值的指针(未使用)
 * 返回值: 始终返回AE_OK以继续遍历
 */
static acpi_status
acpi_ns_find_ini_methods(acpi_handle obj_handle,
			 u32 nesting_level, void *context, void **return_value)
{
	struct acpi_device_walk_info *info =
	    ACPI_CAST_PTR(struct acpi_device_walk_info, context);//将上下文转换为设备遍历信息结构指针
	struct acpi_namespace_node *node;
	struct acpi_namespace_node *parent_node;

	/* 统计设备类对象数量 */
	node = ACPI_CAST_PTR(struct acpi_namespace_node, obj_handle);//将对象句柄转换为命名空间节点指针
	if ((node->type == ACPI_TYPE_DEVICE) ||//ACPI设备对象
	    (node->type == ACPI_TYPE_PROCESSOR) ||//处理器对象
	    (node->type == ACPI_TYPE_THERMAL)) {//散热对象 
		info->device_count++;//增加设备计数器
		return (AE_OK);
	}

	/* We are only looking for methods named _INI */
	/* 只处理名为_INI的方法 */
	if (!ACPI_COMPARE_NAMESEG(node->name.ascii, METHOD_NAME__INI)) {
		return (AE_OK);//非_INI方法直接跳过
	}

	/*
	 * 标记有效的_INI方法
	 * 只关心位于Device/Processor/Thermal对象下的_INI方法
	 */
	parent_node = node->parent;//获取父节点
	switch (parent_node->type) {//检查父节点类型
	case ACPI_TYPE_DEVICE://设备对象
	case ACPI_TYPE_PROCESSOR://处理器对象
	case ACPI_TYPE_THERMAL://散热对象

		/* Mark parent and bubble up the INI present flag to the root */
		/* 标记父节点及所有祖先节点
		 * 设置ANOBJ_SUBTREE_HAS_INI标志位
		 * 这样上层可以知道该子树包含需要执行的_INI方法
		 */
		while (parent_node) {
			parent_node->flags |= ANOBJ_SUBTREE_HAS_INI;
			parent_node = parent_node->parent;
		}
		break;

	default:

		break;
	}

	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_init_one_device
 *
 * PARAMETERS:  acpi_walk_callback
 *
 * RETURN:      acpi_status
 *
 * DESCRIPTION: This is called once per device soon after ACPI is enabled
 *              to initialize each device. It determines if the device is
 *              present, and if so, calls _INI.
 *
 ******************************************************************************/
/*
 * acpi_ns_init_one_device - 初始化单个ACPI设备的回调函数
 * @obj_handle: 当前设备对象的句柄
 * @nesting_level: 当前命名空间层级深度
 * @context: 指向acpi_device_walk_info结构的指针
 * @return_value: 未使用的返回值指针
 * 返回值: 状态码，控制命名空间遍历行为
 */
static acpi_status
acpi_ns_init_one_device(acpi_handle obj_handle,
			u32 nesting_level, void *context, void **return_value)
{
	struct acpi_device_walk_info *walk_info =
	    ACPI_CAST_PTR(struct acpi_device_walk_info, context);//获取设备遍历上下文
	struct acpi_evaluate_info *info = walk_info->evaluate_info;//方法评估信息结构
	u32 flags;
	acpi_status status;
	struct acpi_namespace_node *device_node;//当前设备节点指针

	ACPI_FUNCTION_TRACE(ns_init_one_device);

	/* We are interested in Devices, Processors and thermal_zones only */

	device_node = ACPI_CAST_PTR(struct acpi_namespace_node, obj_handle);//将对象句柄转换为命名空间节点指针
	/* 检查节点类型，只处理设备/处理器/散热区三种类型 */
	if ((device_node->type != ACPI_TYPE_DEVICE) &&//非设备对象
	    (device_node->type != ACPI_TYPE_PROCESSOR) &&//非处理器对象
	    (device_node->type != ACPI_TYPE_THERMAL)) {//非散热区对象
		return_ACPI_STATUS(AE_OK);
	}

	/*
	 * Because of an earlier namespace analysis, all subtrees that contain an
	 * _INI method are tagged.
	 *
	 * If this device subtree does not contain any _INI methods, we
	 * can exit now and stop traversing this entire subtree.
	 * 如果该子树不包含_INI方法(ANOBJ_SUBTREE_HAS_INI未设置),则跳过整个子树的遍历
	 */
	if (!(device_node->flags & ANOBJ_SUBTREE_HAS_INI)) {
		return_ACPI_STATUS(AE_CTRL_DEPTH);
	}

	/*
	 * Run _STA to determine if this device is present and functioning. We
	 * must know this information for two important reasons (from ACPI spec):
	 *
	 * 1) We can only run _INI if the device is present.
	 * 2) We must abort the device tree walk on this subtree if the device is
	 *    not present and is not functional (we will not examine the children)
	 *
	 * The _STA method is not required to be present under the device, we
	 * assume the device is present if _STA does not exist.
	 */
	ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
			(ACPI_TYPE_METHOD, device_node, METHOD_NAME__STA));

	status = acpi_ut_execute_STA(device_node, &flags);//执行_STA方法获取设备状态
	if (ACPI_FAILURE(status)) {

		/* Ignore error and move on to next device */

		return_ACPI_STATUS(AE_OK);//忽略_STA执行错误(如方法不存在) ,继续处理下一个设备
	}

	/*
	 * Flags == -1 means that _STA was not found. In this case, we assume that
	 * the device is both present and functional.
	 *
	 * From the ACPI spec, description of _STA:
	 *
	 * "If a device object (including the processor object) does not have an
	 * _STA object, then OSPM assumes that all of the above bits are set (in
	 * other words, the device is present, ..., and functioning)"
	 * 处理_STA返回值：
	 * 如果flags不是最大值(0xFFFFFFFF)，说明_STA方法存在且已执行
	 */
	if (flags != ACPI_UINT32_MAX) {
		walk_info->num_STA++;//增加_STA执行计数器
	}

	/*
	 * Examine the PRESENT and FUNCTIONING status bits
	 *
	 * Note: ACPI spec does not seem to specify behavior for the present but
	 * not functioning case, so we assume functioning if present.
	 */
        /*
         * 设备状态检查：
         * ACPI_STA_DEVICE_PRESENT - 位0表示设备存在
         * ACPI_STA_DEVICE_FUNCTIONING - 位3表示设备功能正常
         */
	if (!(flags & ACPI_STA_DEVICE_PRESENT)) {//检查设备存在位

		/* Device is not present, we must examine the Functioning bit */

		if (flags & ACPI_STA_DEVICE_FUNCTIONING) {//检查功能正常位
			/*
			 * Device is not present but is "functioning". In this case,
			 * we will not run _INI, but we continue to examine the children
			 * of this device.
			 *
			 * From the ACPI spec, description of _STA: (note - no mention
			 * of whether to run _INI or not on the device in question)
			 *
			 * "_STA may return bit 0 clear (not present) with bit 3 set
			 * (device is functional). This case is used to indicate a valid
			 * device for which no device driver should be loaded (for example,
			 * a bridge device.) Children of this device may be present and
			 * valid. OSPM should continue enumeration below a device whose
			 * _STA returns this bit combination"
			 */
			return_ACPI_STATUS(AE_OK);//设备不存在但标记为功能正常(如桥设备),不执行_INI但继续检查子设备
		} else {
			/*
			 * Device is not present and is not functioning. We must abort the
			 * walk of this subtree immediately -- don't look at the children
			 * of such a device.
			 *
			 * From the ACPI spec, description of _INI:
			 *
			 * "If the _STA method indicates that the device is not present,
			 * OSPM will not run the _INI and will not examine the children
			 * of the device for _INI methods"
			 * 设备不存在且不工作：跳过整个子树的遍历
			 */
			return_ACPI_STATUS(AE_CTRL_DEPTH);
		}
	}

	/*
	 * The device is present or is assumed present if no _STA exists.
	 * Run the _INI if it exists (not required to exist)
	 *
	 * Note: We know there is an _INI within this subtree, but it may not be
	 * under this particular device, it may be lower in the branch.
	 */
        /*
         * 执行_INI方法的条件检查：
         * 1. 当前设备不是_SB_设备 或 
         * 2. 不是根节点的直接子节点
         * (避免重复执行已在acpi_ns_initialize_devices中处理过的_SB_._INI)
         */
	if (!ACPI_COMPARE_NAMESEG(device_node->name.ascii, "_SB_") ||//检查设备名
	    device_node->parent != acpi_gbl_root_node) {// 检查父节点
		ACPI_DEBUG_EXEC(acpi_ut_display_init_pathname
				(ACPI_TYPE_METHOD, device_node,
				 METHOD_NAME__INI));

		memset(info, 0, sizeof(struct acpi_evaluate_info));//清空评估信息结构体
		info->prefix_node = device_node;//设置当前节点
		info->relative_pathname = METHOD_NAME__INI;//设置方法名
		info->parameters = NULL;//无输入参数
		info->flags = ACPI_IGNORE_RETURN_VALUE;//忽略返回值

		status = acpi_ns_evaluate(info);//调用命名空间评估函数,执行_INI方法
		if (ACPI_SUCCESS(status)) {
			walk_info->num_INI++;//执行成功,增加_INI执行计数器
		}
#ifdef ACPI_DEBUG_OUTPUT//调试模式下的错误处理
		else if (status != AE_NOT_FOUND) {//忽略"方法未找到"错误

			/* Ignore error and move on to next device */

			char *scope_name =
			    acpi_ns_get_normalized_pathname(device_node, TRUE);//获取设备路径用于错误报告

			ACPI_EXCEPTION((AE_INFO, status,
					"during %s._INI execution",
					scope_name));
			ACPI_FREE(scope_name);//释放路径字符串内存
		}
#endif
	}

	/* Ignore errors from above */

	status = AE_OK;//重置状态为成功，忽略之前可能出现的错误

	/*
	 * The _INI method has been run if present; call the Global Initialization
	 * Handler for this device.
	 */
	if (acpi_gbl_init_handler) {//检查是否注册了全局初始化处理程序
		status =
		    acpi_gbl_init_handler(device_node, ACPI_INIT_DEVICE_INI);//调用全局初始化处理程序
	}

	return_ACPI_STATUS(status);
}
