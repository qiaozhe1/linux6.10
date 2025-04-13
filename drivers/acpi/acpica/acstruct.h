/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/******************************************************************************
 *
 * Name: acstruct.h - Internal structs
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#ifndef __ACSTRUCT_H__
#define __ACSTRUCT_H__

/* acpisrc:struct_defs -- for acpisrc conversion */

/*****************************************************************************
 *
 * Tree walking typedefs and structs
 *
 ****************************************************************************/

/*
 * Walk state - current state of a parse tree walk. Used for both a leisurely
 * stroll through the tree (for whatever reason), and for control method
 * execution.
 */
#define ACPI_NEXT_OP_DOWNWARD       1
#define ACPI_NEXT_OP_UPWARD         2

/*
 * Groups of definitions for walk_type used for different implementations of
 * walkers (never simultaneously) - flags for interpreter:
 */
#define ACPI_WALK_NON_METHOD        0
#define ACPI_WALK_METHOD            0x01
#define ACPI_WALK_METHOD_RESTART    0x02
/*
 * struct acpi_walk_state - AML解析/执行状态控制结构（核心上下文管理器）
 * 
 * 核心作用：
 * 1. 维护AML解析过程的完整上下文（操作码、操作数、作用域等）
 * 2. 管理控制方法执行时的参数传递和局部变量存储
 * 3. 跟踪嵌套的控制结构（条件判断/循环等）
 * 4. 实现解释器的状态暂存与恢复
 *
 * 设计特点：
 * - 每个控制方法执行都会创建独立的walk_state
 * - 支持方法嵌套调用（通过next指针形成链表）
 * - 携带完整的调试信息（断点、单步执行等）
 */
struct acpi_walk_state {
	struct acpi_walk_state *next;	/* 链表指针，用于嵌套方法调用时链接多个walk_state */
	u8 descriptor_type;	//描述符类型标识（区分不同内核对象）
	u8 walk_type;		//遍历类型标识（如ACPI_WALK_METHOD）
	u16 opcode;		//当前正在处理的AML操作码（如0x5B=DeviceOp）
	u8 next_op_info;	//下一个操作码的附加信息（解析阶段使用）
	u8 num_operands;	//操作数栈总容量（ACPI_OBJ_NUM_OPERANDS定义）
	u8 operand_index;	//当前操作数栈索引（用于acpi_ds_obj_stack_push）
	acpi_owner_id owner_id;	//资源所有者ID（跟踪对象创建关系）
	u8 last_predicate;	//最近一次谓词计算结果（用于条件判断）
	u8 current_result;	//当前操作结果状态码
	u8 return_used;		//标识方法是否有返回值被使用
	u8 scope_depth;		//命名空间作用域嵌套深度（防止栈溢出）
	u8 pass_number;		//解析阶段编号（表加载时区分多次解析）
	u8 namespace_override;	//覆盖已存在命名空间对象的标志位
	u8 result_size;		//结果栈总容量
	u8 result_count;	//结果栈当前元素计数
	u8 *aml;		//当前解析位置指针（指向AML字节流）
	u32 arg_types;		//当前方法的参数类型位图（4字节编码）
	u32 method_breakpoint;	//方法断点偏移量（用于调试器单步执行）
	u32 user_breakpoint;	//用户定义的AML断点位置
	u32 parse_flags;	//解析标志位（如ACPI_PARSE_MODULE_LEVEL）

	/* 解析器状态管理 */
	struct acpi_parse_state parser_state;//当前解析状态（包含AML指针/剩余长度等）
	u32 prev_arg_types;//前一个操作的参数类型记录
	u32 arg_count;		//当前参数计数器（固定/可变参数处理）
	u16 method_nesting_depth;//方法调用嵌套深度（防无限递归）
	u8 method_is_nested;//当前方法是否为嵌套调用标志

	/* 方法参数与局部变量存储 */
	struct acpi_namespace_node arguments[ACPI_METHOD_NUM_ARGS];//方法参数节点数组（最多7个参数）
	struct acpi_namespace_node local_variables[ACPI_METHOD_NUM_LOCALS];//方法局部变量节点数组（最多8个）
	
	/* 运行时栈管理 */
	union acpi_operand_object *operands[ACPI_OBJ_NUM_OPERANDS + 1];//操作数对象指针数组（带NULL终止符）
	union acpi_operand_object **params;//当前参数指针数组（动态分配）

	/* 流程控制相关 */
	u8 *aml_last_while;//最近While循环的AML起始地址（用于break/continue）
	union acpi_operand_object **caller_return_desc;//调用方返回描述符指针（返回值传递）
	union acpi_generic_state *control_state;//控制状态栈指针（管理if/else/while嵌套）
	struct acpi_namespace_node *deferred_node;//延迟执行的节点指针（用于Fatal等操作）
	union acpi_operand_object *implicit_return_obj;//隐式返回对象（自动构造的返回值）
	
	/* 方法执行上下文 */
	struct acpi_namespace_node *method_call_node;//被调用方法节点指针
	union acpi_parse_object *method_call_op;//方法调用操作符的解析节点
	union acpi_operand_object *method_desc;	//当前执行的方法描述符对象
	struct acpi_namespace_node *method_node;//当前方法关联的命名空间节点
	char *method_pathname;	//方法全路径名（如"\\_SB.PCI0._STA"）
	
	/* 解析树操作 */
	union acpi_parse_object *op;//当前处理的解析节点（ParseOp）
	const struct acpi_opcode_info *op_info;//当前操作码的解析信息（来自全局Opcode表）
	union acpi_parse_object *origin;//遍历起始节点（已废弃字段）
	union acpi_operand_object *result_obj;//当前操作结果对象
	union acpi_generic_state *results;//结果对象栈指针（累积表达式结果）
	union acpi_operand_object *return_desc;	//方法返回描述符对象
	union acpi_generic_state *scope_info;//作用域信息栈（管理Scope/Device等嵌套）
	union acpi_parse_object *prev_op;//前一个处理的解析节点
	union acpi_parse_object *next_op;//下一个待处理的解析节点
		
	/* 线程与回调管理 */
	struct acpi_thread_state *thread;//关联的线程状态指针（多线程支持）
	acpi_parse_downwards descending_callback;// 向下解析回调函数（解析阶段使用）
	acpi_parse_upwards ascending_callback;//向上解析回调函数（执行阶段使用）
};

/* Info used by acpi_ns_initialize_objects and acpi_ds_initialize_objects */

struct acpi_init_walk_info {
	u32 table_index;
	u32 object_count;
	u32 method_count;
	u32 serial_method_count;
	u32 non_serial_method_count;
	u32 serialized_method_count;
	u32 device_count;
	u32 op_region_count;
	u32 field_count;
	u32 buffer_count;
	u32 package_count;
	u32 op_region_init;
	u32 field_init;
	u32 buffer_init;
	u32 package_init;
	acpi_owner_id owner_id;
};

struct acpi_get_devices_info {
	acpi_walk_callback user_function;
	void *context;
	const char *hid;
};

union acpi_aml_operands {
	union acpi_operand_object *operands[7];

	struct {
		struct acpi_object_integer *type;
		struct acpi_object_integer *code;
		struct acpi_object_integer *argument;

	} fatal;

	struct {
		union acpi_operand_object *source;
		struct acpi_object_integer *index;
		union acpi_operand_object *target;

	} index;

	struct {
		union acpi_operand_object *source;
		struct acpi_object_integer *index;
		struct acpi_object_integer *length;
		union acpi_operand_object *target;

	} mid;
};

/*
 * Structure used to pass object evaluation information and parameters.
 * Purpose is to reduce CPU stack use.
 * 用于传递对象评估信息和参数的结构。目的是减少CPU栈的使用。
 */
struct acpi_evaluate_info {
	/* The first 3 elements are passed by the caller to acpi_ns_evaluate */

	struct acpi_namespace_node *prefix_node;	/* Input: starting node */
	const char *relative_pathname;	/* Input: path relative to prefix_node */
	union acpi_operand_object **parameters;	/* Input: argument list */

	struct acpi_namespace_node *node;	/* Resolved node (prefix_node:relative_pathname) */
	union acpi_operand_object *obj_desc;	/* Object attached to the resolved node */
	char *full_pathname;	/* Full pathname of the resolved node */

	const union acpi_predefined_info *predefined;	/* Used if Node is a predefined name */
	union acpi_operand_object *return_object;	/* Object returned from the evaluation */
	union acpi_operand_object *parent_package;	/* Used if return object is a Package */

	u32 return_flags;	/* Used for return value analysis */
	u32 return_btype;	/* Bitmapped type of the returned object */
	u16 param_count;	/* Count of the input argument list */
	u16 node_flags;		/* Same as Node->Flags */
	u8 pass_number;		/* Parser pass number */
	u8 return_object_type;	/* Object type of the returned object */
	u8 flags;		/* General flags */
};

/* Values for Flags above */

#define ACPI_IGNORE_RETURN_VALUE    1

/* Defines for return_flags field above */

#define ACPI_OBJECT_REPAIRED        1
#define ACPI_OBJECT_WRAPPED         2

/* Info used by acpi_ns_initialize_devices */

struct acpi_device_walk_info {
	struct acpi_table_desc *table_desc;
	struct acpi_evaluate_info *evaluate_info;
	u32 device_count;
	u32 num_STA;
	u32 num_INI;
};

/* Info used by Acpi  acpi_db_display_fields */

struct acpi_region_walk_info {
	u32 debug_level;
	u32 count;
	acpi_owner_id owner_id;
	u8 display_type;
	u32 address_space_id;
};

/* TBD: [Restructure] Merge with struct above */

struct acpi_walk_info {
	u32 debug_level;
	u32 count;
	acpi_owner_id owner_id;
	u8 display_type;
};

/* Display Types */

#define ACPI_DISPLAY_SUMMARY        (u8) 0
#define ACPI_DISPLAY_OBJECTS        (u8) 1
#define ACPI_DISPLAY_MASK           (u8) 1

#define ACPI_DISPLAY_SHORT          (u8) 2

#endif
