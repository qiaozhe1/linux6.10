/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/******************************************************************************
 *
 * Name: aclocal.h - Internal data types used across the ACPI subsystem
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#ifndef __ACLOCAL_H__
#define __ACLOCAL_H__

/* acpisrc:struct_defs -- for acpisrc conversion */

#define ACPI_SERIALIZED                 0xFF

typedef u32 acpi_mutex_handle;
#define ACPI_GLOBAL_LOCK                (acpi_semaphore) (-1)

/* Total number of aml opcodes defined */

#define AML_NUM_OPCODES                 0x83

/* Forward declarations */

struct acpi_walk_state;
struct acpi_obj_mutex;
union acpi_parse_object;

/*****************************************************************************
 *
 * Mutex typedefs and structs
 *
 ****************************************************************************/

/*
 * Predefined handles for the mutex objects used within the subsystem
 * All mutex objects are automatically created by acpi_ut_mutex_initialize.
 *
 * The acquire/release ordering protocol is implied via this list. Mutexes
 * with a lower value must be acquired before mutexes with a higher value.
 *
 * NOTE: any changes here must be reflected in the acpi_gbl_mutex_names
 * table below also!
 */
#define ACPI_MTX_INTERPRETER            0	/* AML Interpreter, main lock */
#define ACPI_MTX_NAMESPACE              1	/* ACPI Namespace */
#define ACPI_MTX_TABLES                 2	/* Data for ACPI tables */
#define ACPI_MTX_EVENTS                 3	/* Data for ACPI events */
#define ACPI_MTX_CACHES                 4	/* Internal caches, general purposes */
#define ACPI_MTX_MEMORY                 5	/* Debug memory tracking lists */

#define ACPI_MAX_MUTEX                  5
#define ACPI_NUM_MUTEX                  (ACPI_MAX_MUTEX+1)

/* Lock structure for reader/writer interfaces */

struct acpi_rw_lock {
	acpi_mutex writer_mutex;
	acpi_mutex reader_mutex;
	u32 num_readers;
};

/*
 * Predefined handles for spinlocks used within the subsystem.
 * These spinlocks are created by acpi_ut_mutex_initialize
 */
#define ACPI_LOCK_GPES                  0
#define ACPI_LOCK_HARDWARE              1

#define ACPI_MAX_LOCK                   1
#define ACPI_NUM_LOCK                   (ACPI_MAX_LOCK+1)

/* This Thread ID means that the mutex is not in use (unlocked) */

#define ACPI_MUTEX_NOT_ACQUIRED         ((acpi_thread_id) 0)

/* This Thread ID means an invalid thread ID */

#ifdef ACPI_OS_INVALID_THREAD_ID
#define ACPI_INVALID_THREAD_ID          ACPI_OS_INVALID_THREAD_ID
#else
#define ACPI_INVALID_THREAD_ID          ((acpi_thread_id) 0xFFFFFFFF)
#endif

/* Table for the global mutexes */

struct acpi_mutex_info {
	acpi_mutex mutex;
	u32 use_count;
	acpi_thread_id thread_id;
};

/* Lock flag parameter for various interfaces */

#define ACPI_MTX_DO_NOT_LOCK            0
#define ACPI_MTX_LOCK                   1

/* Field access granularities */

#define ACPI_FIELD_BYTE_GRANULARITY     1
#define ACPI_FIELD_WORD_GRANULARITY     2
#define ACPI_FIELD_DWORD_GRANULARITY    4
#define ACPI_FIELD_QWORD_GRANULARITY    8

#define ACPI_ENTRY_NOT_FOUND            NULL

/*****************************************************************************
 *
 * Namespace typedefs and structs
 *
 ****************************************************************************/

/* Operational modes of the AML interpreter/scanner */

typedef enum {
	ACPI_IMODE_LOAD_PASS1 = 0x01,
	ACPI_IMODE_LOAD_PASS2 = 0x02,
	ACPI_IMODE_EXECUTE = 0x03
} acpi_interpreter_mode;

/*
 * The Namespace Node describes a named object that appears in the AML.
 * descriptor_type is used to differentiate between internal descriptors.
 *
 * The node is optimized for both 32-bit and 64-bit platforms:
 * 20 bytes for the 32-bit case, 32 bytes for the 64-bit case.
 *
 * Note: The descriptor_type and Type fields must appear in the identical
 * position in both the struct acpi_namespace_node and union acpi_operand_object
 * structures.
 */
/**
 * struct acpi_namespace_node - ACPI命名空间节点结构
 *
 * 表示ACPI命名空间中的一个节点，构成ACPI命名树的树状结构。
 * 每个节点可以关联操作对象或作为路径节点存在。
 */
struct acpi_namespace_node {
	union acpi_operand_object *object;//节点关联的操作对象，没有则为NULL
	u8 descriptor_type;//描述符类型标识，用于区分不同描述符类型
	u8 type;//节点类型，对应ACPI对象类型
	u16 flags;//节点状态标志
	union acpi_name_union name;//节点名称(4字节ACPI名称)
	struct acpi_namespace_node *parent;//父节点指针
	struct acpi_namespace_node *child;//第一个子节点
	struct acpi_namespace_node *peer;//同级节点(兄弟节点)
	acpi_owner_id owner_id;//所有者ID

	/*
	 * The following fields are used by the ASL compiler and disassembler only
	 */
#ifdef ACPI_LARGE_NAMESPACE_NODE//以下字段仅ASL编译器和反汇编器使用
	union acpi_parse_object *op;//关联的解析树节点
	void *method_locals;//方法本地变量存储
	void *method_args;//方法参数存储
	u32 value;//常数值
	u32 length;//数据长度
	u8 arg_count;//方法参数计数

#endif
};

/* Namespace Node flags */

#define ANOBJ_RESERVED                  0x01	/* Available for use */
#define ANOBJ_TEMPORARY                 0x02	/* Node is create by a method and is temporary */
#define ANOBJ_METHOD_ARG                0x04	/* Node is a method argument */
#define ANOBJ_METHOD_LOCAL              0x08	/* Node is a method local */
#define ANOBJ_SUBTREE_HAS_INI           0x10	/* Used to optimize device initialization */
#define ANOBJ_EVALUATED                 0x20	/* Set on first evaluation of node */
#define ANOBJ_ALLOCATED_BUFFER          0x40	/* Method AML buffer is dynamic (install_method) */
#define ANOBJ_NODE_EARLY_INIT           0x80	/* acpi_exec only: Node was create via init file (-fi) */

#define ANOBJ_IS_EXTERNAL               0x08	/* iASL only: This object created via External() */
#define ANOBJ_METHOD_NO_RETVAL          0x10	/* iASL only: Method has no return value */
#define ANOBJ_METHOD_SOME_NO_RETVAL     0x20	/* iASL only: Method has at least one return value */
#define ANOBJ_IS_REFERENCED             0x80	/* iASL only: Object was referenced */

/* Internal ACPI table management - master table list */

struct acpi_table_list {
	struct acpi_table_desc *tables;	/* Table descriptor array */
	u32 current_table_count;	/* Tables currently in the array */
	u32 max_table_count;	/* Max tables array will hold */
	u8 flags;
};

/* Flags for above */

#define ACPI_ROOT_ORIGIN_UNKNOWN        (0)	/* ~ORIGIN_ALLOCATED */
#define ACPI_ROOT_ORIGIN_ALLOCATED      (1)
#define ACPI_ROOT_ALLOW_RESIZE          (2)

/* List to manage incoming ACPI tables */

struct acpi_new_table_desc {
	struct acpi_table_header *table;
	struct acpi_new_table_desc *next;
};

/* Predefined table indexes */

#define ACPI_INVALID_TABLE_INDEX        (0xFFFFFFFF)

struct acpi_find_context {
	char *search_for;
	acpi_handle *list;
	u32 *count;
};

struct acpi_ns_search_data {
	struct acpi_namespace_node *node;
};

/* Object types used during package copies */

#define ACPI_COPY_TYPE_SIMPLE           0
#define ACPI_COPY_TYPE_PACKAGE          1

/* Info structure used to convert external<->internal namestrings */

struct acpi_namestring_info {
	const char *external_name;
	const char *next_external_char;
	char *internal_name;
	u32 length;
	u32 num_segments;
	u32 num_carats;
	u8 fully_qualified;
};

/* Field creation info */

struct acpi_create_field_info {
	struct acpi_namespace_node *region_node;
	struct acpi_namespace_node *field_node;
	struct acpi_namespace_node *register_node;
	struct acpi_namespace_node *data_register_node;
	struct acpi_namespace_node *connection_node;
	u8 *resource_buffer;
	u32 bank_value;
	u32 field_bit_position;
	u32 field_bit_length;
	u16 resource_length;
	u16 pin_number_index;
	u8 field_flags;
	u8 attribute;
	u8 field_type;
	u8 access_length;
};

typedef
acpi_status (*acpi_internal_method) (struct acpi_walk_state * walk_state);

/*
 * Bitmapped ACPI types. Used internally only
 */
#define ACPI_BTYPE_ANY                  0x00000000
#define ACPI_BTYPE_INTEGER              0x00000001
#define ACPI_BTYPE_STRING               0x00000002
#define ACPI_BTYPE_BUFFER               0x00000004
#define ACPI_BTYPE_PACKAGE              0x00000008
#define ACPI_BTYPE_FIELD_UNIT           0x00000010
#define ACPI_BTYPE_DEVICE               0x00000020
#define ACPI_BTYPE_EVENT                0x00000040
#define ACPI_BTYPE_METHOD               0x00000080
#define ACPI_BTYPE_MUTEX                0x00000100
#define ACPI_BTYPE_REGION               0x00000200
#define ACPI_BTYPE_POWER                0x00000400
#define ACPI_BTYPE_PROCESSOR            0x00000800
#define ACPI_BTYPE_THERMAL              0x00001000
#define ACPI_BTYPE_BUFFER_FIELD         0x00002000
#define ACPI_BTYPE_DDB_HANDLE           0x00004000
#define ACPI_BTYPE_DEBUG_OBJECT         0x00008000
#define ACPI_BTYPE_REFERENCE_OBJECT     0x00010000	/* From Index(), ref_of(), etc (type6_opcodes) */
#define ACPI_BTYPE_RESOURCE             0x00020000
#define ACPI_BTYPE_NAMED_REFERENCE      0x00040000	/* Generic unresolved Name or Namepath */

#define ACPI_BTYPE_COMPUTE_DATA         (ACPI_BTYPE_INTEGER | ACPI_BTYPE_STRING | ACPI_BTYPE_BUFFER)

#define ACPI_BTYPE_DATA                 (ACPI_BTYPE_COMPUTE_DATA  | ACPI_BTYPE_PACKAGE)

	/* Used by Copy, de_ref_of, Store, Printf, Fprintf */

#define ACPI_BTYPE_DATA_REFERENCE       (ACPI_BTYPE_DATA | ACPI_BTYPE_REFERENCE_OBJECT | ACPI_BTYPE_DDB_HANDLE)
#define ACPI_BTYPE_DEVICE_OBJECTS       (ACPI_BTYPE_DEVICE | ACPI_BTYPE_THERMAL | ACPI_BTYPE_PROCESSOR)
#define ACPI_BTYPE_OBJECTS_AND_REFS     0x0001FFFF	/* ARG or LOCAL */
#define ACPI_BTYPE_ALL_OBJECTS          0x0000FFFF

#pragma pack(1)

/*
 * Information structure for ACPI predefined names.
 * Each entry in the table contains the following items:
 *
 * name                 - The ACPI reserved name
 * param_count          - Number of arguments to the method
 * expected_return_btypes - Allowed type(s) for the return value
 */
struct acpi_name_info {
	char name[ACPI_NAMESEG_SIZE];
	u16 argument_list;
	u8 expected_btypes;
};

/*
 * Secondary information structures for ACPI predefined objects that return
 * package objects. This structure appears as the next entry in the table
 * after the NAME_INFO structure above.
 *
 * The reason for this is to minimize the size of the predefined name table.
 */

/*
 * Used for ACPI_PTYPE1_FIXED, ACPI_PTYPE1_VAR, ACPI_PTYPE2,
 * ACPI_PTYPE2_MIN, ACPI_PTYPE2_PKG_COUNT, ACPI_PTYPE2_COUNT,
 * ACPI_PTYPE2_FIX_VAR
 */
struct acpi_package_info {
	u8 type;
	u8 object_type1;
	u8 count1;
	u8 object_type2;
	u8 count2;
	u16 reserved;
};

/* Used for ACPI_PTYPE2_FIXED */

struct acpi_package_info2 {
	u8 type;
	u8 count;
	u8 object_type[4];
	u8 reserved;
};

/* Used for ACPI_PTYPE1_OPTION */

struct acpi_package_info3 {
	u8 type;
	u8 count;
	u8 object_type[2];
	u8 tail_object_type;
	u16 reserved;
};

struct acpi_package_info4 {
	u8 type;
	u8 object_type1;
	u8 count1;
	u8 sub_object_types;
	u8 pkg_count;
	u16 reserved;
};

union acpi_predefined_info {
	struct acpi_name_info info;
	struct acpi_package_info ret_info;
	struct acpi_package_info2 ret_info2;
	struct acpi_package_info3 ret_info3;
	struct acpi_package_info4 ret_info4;
};

/* Reset to default packing */

#pragma pack()

/* Return object auto-repair info */

typedef acpi_status (*acpi_object_converter) (struct acpi_namespace_node *
					      scope,
					      union acpi_operand_object *
					      original_object,
					      union acpi_operand_object **
					      converted_object);

struct acpi_simple_repair_info {
	char name[ACPI_NAMESEG_SIZE];
	u32 unexpected_btypes;
	u32 package_index;
	acpi_object_converter object_converter;
};

/*
 * Bitmapped return value types
 * Note: the actual data types must be contiguous, a loop in nspredef.c
 * depends on this.
 */
#define ACPI_RTYPE_ANY                  0x00
#define ACPI_RTYPE_NONE                 0x01
#define ACPI_RTYPE_INTEGER              0x02
#define ACPI_RTYPE_STRING               0x04
#define ACPI_RTYPE_BUFFER               0x08
#define ACPI_RTYPE_PACKAGE              0x10
#define ACPI_RTYPE_REFERENCE            0x20
#define ACPI_RTYPE_ALL                  0x3F

#define ACPI_NUM_RTYPES                 5	/* Number of actual object types */

/* Info for running the _REG methods */

struct acpi_reg_walk_info {
	u32 function;
	u32 reg_run_count;
	acpi_adr_space_type space_id;
};

/*****************************************************************************
 *
 * Event typedefs and structs
 *
 ****************************************************************************/

/* Dispatch info for each host-installed SCI handler */

struct acpi_sci_handler_info {
	struct acpi_sci_handler_info *next;
	acpi_sci_handler address;	/* Address of handler */
	void *context;		/* Context to be passed to handler */
};

/* Dispatch info for each GPE -- either a method or handler, cannot be both */

struct acpi_gpe_handler_info {
	acpi_gpe_handler address;	/* Address of handler, if any */
	void *context;		/* Context to be passed to handler */
	struct acpi_namespace_node *method_node;	/* Method node for this GPE level (saved) */
	u8 original_flags;	/* Original (pre-handler) GPE info */
	u8 originally_enabled;	/* True if GPE was originally enabled */
};

/* Notify info for implicit notify, multiple device objects */

struct acpi_gpe_notify_info {
	struct acpi_namespace_node *device_node;	/* Device to be notified */
	struct acpi_gpe_notify_info *next;
};

/*
 * GPE dispatch info. At any time, the GPE can have at most one type
 * of dispatch - Method, Handler, or Implicit Notify.
 */
union acpi_gpe_dispatch_info {
	struct acpi_namespace_node *method_node;	/* Method node for this GPE level */
	struct acpi_gpe_handler_info *handler;  /* Installed GPE handler */
	struct acpi_gpe_notify_info *notify_list;	/* List of _PRW devices for implicit notifies */
};

/*
 * Information about a GPE, one per each GPE in an array.
 * NOTE: Important to keep this struct as small as possible.
 */
struct acpi_gpe_event_info {
	union acpi_gpe_dispatch_info dispatch;	/* Either Method, Handler, or notify_list */
	struct acpi_gpe_register_info *register_info;	/* Backpointer to register info */
	u8 flags;		/* Misc info about this GPE */
	u8 gpe_number;		/* This GPE */
	u8 runtime_count;	/* References to a run GPE */
	u8 disable_for_dispatch;	/* Masked during dispatching */
};

/* GPE register address */

struct acpi_gpe_address {
	u8 space_id;	/* Address space where the register exists */
	u64 address;	/* 64-bit address of the register */
};

/* Information about a GPE register pair, one per each status/enable pair in an array */

struct acpi_gpe_register_info {
	struct acpi_gpe_address status_address;	/* Address of status reg */
	struct acpi_gpe_address enable_address;	/* Address of enable reg */
	u16 base_gpe_number;	/* Base GPE number for this register */
	u8 enable_for_wake;	/* GPEs to keep enabled when sleeping */
	u8 enable_for_run;	/* GPEs to keep enabled when running */
	u8 mask_for_run;	/* GPEs to keep masked when running */
	u8 enable_mask;		/* Current mask of enabled GPEs */
};

/*
 * Information about a GPE register block, one per each installed block --
 * GPE0, GPE1, and one per each installed GPE Block Device.
 */
struct acpi_gpe_block_info {
	struct acpi_namespace_node *node;
	struct acpi_gpe_block_info *previous;
	struct acpi_gpe_block_info *next;
	struct acpi_gpe_xrupt_info *xrupt_block;	/* Backpointer to interrupt block */
	struct acpi_gpe_register_info *register_info;	/* One per GPE register pair */
	struct acpi_gpe_event_info *event_info;	/* One for each GPE */
	u64 address;		/* Base address of the block */
	u32 register_count;	/* Number of register pairs in block */
	u16 gpe_count;		/* Number of individual GPEs in block */
	u16 block_base_number;	/* Base GPE number for this block */
	u8 space_id;
	u8 initialized;		/* TRUE if this block is initialized */
};

/* Information about GPE interrupt handlers, one per each interrupt level used for GPEs */

struct acpi_gpe_xrupt_info {
	struct acpi_gpe_xrupt_info *previous;
	struct acpi_gpe_xrupt_info *next;
	struct acpi_gpe_block_info *gpe_block_list_head;	/* List of GPE blocks for this xrupt */
	u32 interrupt_number;	/* System interrupt number */
};

struct acpi_gpe_walk_info {
	struct acpi_namespace_node *gpe_device;
	struct acpi_gpe_block_info *gpe_block;
	u16 count;
	acpi_owner_id owner_id;
	u8 execute_by_owner_id;
};

struct acpi_gpe_device_info {
	u32 index;
	u32 next_block_base_index;
	acpi_status status;
	struct acpi_namespace_node *gpe_device;
};

typedef acpi_status (*acpi_gpe_callback) (struct acpi_gpe_xrupt_info *
					  gpe_xrupt_info,
					  struct acpi_gpe_block_info *
					  gpe_block, void *context);

/* Information about each particular fixed event */

struct acpi_fixed_event_handler {
	acpi_event_handler handler;	/* Address of handler. */
	void *context;		/* Context to be passed to handler */
};

struct acpi_fixed_event_info {
	u8 status_register_id;//状态寄存器的索引
	u8 enable_register_id;//使能寄存器的索引
	u16 status_bit_mask;//状态位掩码
	u16 enable_bit_mask;//使能位掩码
};

/* Information used during field processing */

struct acpi_field_info {
	u8 skip_field;
	u8 field_flag;
	u32 pkg_length;
};

/* Information about the interrupt ID and _EVT of a GED device */

struct acpi_ged_handler_info {
	struct acpi_ged_handler_info *next;
	u32 int_id;		/* The interrupt ID that triggers the execution of the evt_method. */
	struct acpi_namespace_node *evt_method;	/* The _EVT method to be executed when an interrupt with ID = int_ID is received */
};

/*****************************************************************************
 *
 * Generic "state" object for stacks
 *
 ****************************************************************************/

#define ACPI_CONTROL_NORMAL                  0xC0
#define ACPI_CONTROL_CONDITIONAL_EXECUTING   0xC1
#define ACPI_CONTROL_PREDICATE_EXECUTING     0xC2
#define ACPI_CONTROL_PREDICATE_FALSE         0xC3
#define ACPI_CONTROL_PREDICATE_TRUE          0xC4

#define ACPI_STATE_COMMON \
	void                            *next; \
	u8                              descriptor_type; /* To differentiate various internal objs */\
	u8                              flags; \
	u16                             value; \
	u16                             state

	/* There are 2 bytes available here until the next natural alignment boundary */

struct acpi_common_state {
	ACPI_STATE_COMMON;
};

/*
 * Update state - used to traverse complex objects such as packages
 */
struct acpi_update_state {
	ACPI_STATE_COMMON;
	union acpi_operand_object *object;
};

/*
 * Pkg state - used to traverse nested package structures
 */
/*
 * struct acpi_pkg_state - ACPI包遍历状态结构
 *
 * 该结构专门用于跟踪ACPI包对象(Package)的遍历状态，支持嵌套包处理、
 * 包复制操作和状态恢复等功能。是ACPI对象遍历机制的核心数据结构之一。
 */
struct acpi_pkg_state {
	ACPI_STATE_COMMON;//基础状态字段(8字节头部)
	u32 index;//当前处理的元素索引(0-based)
	union acpi_operand_object *source_object;//正在遍历的源包对象
	union acpi_operand_object *dest_object;//目标包对象(用于复制操作)
	struct acpi_walk_state *walk_state;//关联的walk状态(控制方法执行上下文)
	void *this_target_obj;//当前处理元素的目标存储位置
	u32 num_packages;//嵌套包计数器(调试/验证用)
};

/*
 * Control state - one per if/else and while constructs.
 * Allows nesting of these constructs
 */
struct acpi_control_state {
	ACPI_STATE_COMMON;
	u16 opcode;
	union acpi_parse_object *predicate_op;
	u8 *aml_predicate_start;	/* Start of if/while predicate */
	u8 *package_end;	/* End of if/while block */
	u64 loop_timeout;	/* While() loop timeout */
};

/*
 * Scope state - current scope during namespace lookups
 */
struct acpi_scope_state {
	ACPI_STATE_COMMON;
	struct acpi_namespace_node *node;
};

struct acpi_pscope_state {
	ACPI_STATE_COMMON;
	u32 arg_count;		/* Number of fixed arguments */
	union acpi_parse_object *op;	/* Current op being parsed */
	u8 *arg_end;		/* Current argument end */
	u8 *pkg_end;		/* Current package end */
	u32 arg_list;		/* Next argument to parse */
};

/*
 * Thread state - one per thread across multiple walk states. Multiple walk
 * states are created when there are nested control methods executing.
 * ACPI线程状态结构体，用于跟踪线程在执行AML控制方法时的同步状态、执行上下文
 * 和资源持有情况。是ACPI解释器实现多线程安全和资源管理的核心数据结构
 */
struct acpi_thread_state {
	ACPI_STATE_COMMON;//包含通用状态字段（如嵌套级别、异常处理等）
	u8 current_sync_level;//当前线程的同步级别（用于跟踪互斥锁的嵌套获取层级）。作用：记录当前线程持有的同步资源深度，如递归获取互斥锁时的计数
	struct acpi_walk_state *walk_state_list;//该线程所有AML方法执行状态的链表头指针。作用：保存该线程正在执行的多个控制方法（如_ASI、_OSC）的执行上下文
	union acpi_operand_object *acquired_mutex_list;//当前线程持有的所有互斥锁对象列表
	acpi_thread_id thread_id;//线程唯一标识符（由操作系统提供）。作用：标识当前线程的ID，用于多线程环境中的资源归属判断
};

/*
 * Result values - used to accumulate the results of nested
 * AML arguments
 */
struct acpi_result_values {
	ACPI_STATE_COMMON;
	union acpi_operand_object *obj_desc[ACPI_RESULTS_FRAME_OBJ_NUM];
};

typedef
acpi_status (*acpi_parse_downwards) (struct acpi_walk_state * walk_state,
				     union acpi_parse_object ** out_op);

typedef
acpi_status (*acpi_parse_upwards) (struct acpi_walk_state * walk_state);

/* Global handlers for AML Notifies */

struct acpi_global_notify_handler {
	acpi_notify_handler handler;
	void *context;
};

/*
 * Notify info - used to pass info to the deferred notify
 * handler/dispatcher.
 */
struct acpi_notify_info {
	ACPI_STATE_COMMON;
	u8 handler_list_id;
	struct acpi_namespace_node *node;
	union acpi_operand_object *handler_list_head;
	struct acpi_global_notify_handler *global;
};

/* Generic state is union of structs above */
/*
 * acpi_generic_state - ACPI通用状态联合体
 * 
 * 这个联合体定义了ACPI子系统使用的所有可能状态类型，通过共用内存空间实现高效状态管理。
 * 各状态类型共享相同的起始结构(acpi_common_state)，通过descriptor_type字段区分具体类型。
 */
union acpi_generic_state {
	struct acpi_common_state common;//基础状态字段
	struct acpi_control_state control;//控制方法执行状态
	struct acpi_update_state update;//对象更新状态
	struct acpi_scope_state scope;//命名空间作用域状态
	struct acpi_pscope_state parse_scope;//解析器作用域状态
	struct acpi_pkg_state pkg;//包遍历状态(重点)
	struct acpi_thread_state thread;//线程状态
	struct acpi_result_values results;//方法结果状态
	struct acpi_notify_info notify;//通知处理状态
};

/*****************************************************************************
 *
 * Interpreter typedefs and structs
 *
 ****************************************************************************/

typedef
acpi_status (*acpi_execute_op) (struct acpi_walk_state * walk_state);

/* Address Range info block */
/*
 * struct acpi_address_range - ACPI地址范围描述符
 *
 * 该结构用于跟踪系统内存和I/O空间的已分配区域，主要功能包括：
 * 1. 维护硬件资源的全局视图
 * 2. 防止地址空间冲突
 * 3. 支持区域查询和验证
 * 
 * 组成全局链表，按地址空间类型分类存储(通过acpi_gbl_address_range_list数组索引)
 */
struct acpi_address_range {
	struct acpi_address_range *next;//链表指针
	struct acpi_namespace_node *region_node;//关联的ACPI命名空间节点
	acpi_physical_address start_address;//区域起始地址
	acpi_physical_address end_address;//区域结束地址
};

/*****************************************************************************
 *
 * Parser typedefs and structs
 *
 ****************************************************************************/

/*
 * AML opcode, name, and argument layout
 */
/*
 * struct acpi_opcode_info - AML操作码元信息描述结构
 *
 * 核心作用：
 * 1. 提供AML操作码的完整语义描述
 * 2. 指导解析器正确处理操作码参数
 * 3. 控制运行时解释行为
 * 4. 支持反汇编和调试输出
 *
 * 存储位置：
 * - 全局只读数组acpi_gbl_aml_op_info（按操作码索引）
 * - 每个ACPI操作码对应一个静态实例
 */
struct acpi_opcode_info {
#if defined(ACPI_DISASSEMBLER) || defined(ACPI_DEBUG_OUTPUT)
	char *name;		//操作码助记符（如"MethodOp"），仅用于反汇编和调试输出
#endif
	u32 parse_args;		//解析阶段参数处理规则（位图编码）
	u32 runtime_args;	//运行时参数处理规则（位图编码）
	u16 flags;		//标志位
	u8 object_type;		//对应的ACPI内部对象类型（ACPI_TYPE_*）
	u8 class;		//操作码大类
	u8 type;		//操作码子类型
};

/* Value associated with the parse object */
/*
 * union acpi_parse_value - ACPI解析器通用值类型联合体
 * 
 * 这个联合体用于表示ACPI解析树节点可以包含的不同类型的值。
 * 根据上下文和操作码类型，使用不同的成员来存储对应的值。
 */
union acpi_parse_value {
	u64 integer;		/* 整数常量(最大64位) */
	u32 size;		/* 字节列表或字段大小 */
	char *string;		/* NULL结尾的字符串 */
	u8 *buffer;		/* 缓冲区或字符串数据 */
	char *name;		/* NULL结尾的名称字符串 */
	union acpi_parse_object *arg;	/* 参数和包含的操作对象 */
};

#if defined(ACPI_DISASSEMBLER) || defined(ACPI_DEBUG_OUTPUT)
#define ACPI_DISASM_ONLY_MEMBERS(a)     a;
#else
#define ACPI_DISASM_ONLY_MEMBERS(a)
#endif

#if defined(ACPI_ASL_COMPILER)
#define ACPI_CONVERTER_ONLY_MEMBERS(a)  a;
#else
#define ACPI_CONVERTER_ONLY_MEMBERS(a)
#endif

/*
 * ACPI_PARSE_COMMON - ACPI解析对象的通用基础结构定义
 *
 * 这个宏定义了所有ACPI解析对象(Parse Object)共有的基础字段，
 * 用于构建ACPI解析树(Parse Tree)的节点结构。
 */
#define ACPI_PARSE_COMMON \
	union acpi_parse_object         *parent;            /* 指向父节点，构成树形结构 */\
	u8                              descriptor_type;    /* 对象类型标识符：用于区分不同类型的ACPI内部对象 */\
	u8                              flags;              /* 操作符类型标志：包含操作符的特殊属性信息 */\
	u16                             aml_opcode;         /* AML操作码：存储从AML字节码中解析出的操作码 */\
	u8                              *aml;               /* 指向AML字节码中的声明位置：记录该节点对应的原始AML代码地址 */\
	union acpi_parse_object         *next;              /* 指向同级下一个节点，构成链表 */\
	struct acpi_namespace_node      *node;              /* 关联的命名空间节点：解释器使用，连接解析树和命名空间 */\
	union acpi_parse_value          value;              /* 操作码关联的值或参数：联合体存储各种类型的操作数值 */\
	u8                              arg_list_length;    /* 参数列表长度：记录该操作码携带的参数数量 */\
	/* 反汇编工具专用字段 (仅在ACPI_DISASM_ONLY宏定义时包含) */ \
	 ACPI_DISASM_ONLY_MEMBERS (\
	u16                             disasm_flags;       /* 反汇编过程使用的标志位 */\
	u8                              disasm_opcode;      /* 用于反汇编的子类型代码 */\
	char                            *operator_symbol;   /* C风格操作符名称字符串 */\
	char                            aml_op_name[16])    /* 操作码名称(调试用) */\
	/* ASL转换器专用字段 (仅在ACPI_CONVERTER_ONLY宏定义时包含) */ \
	 ACPI_CONVERTER_ONLY_MEMBERS (\
	char                            *inline_comment;    /* 行内注释内容 */\
	char                            *end_node_comment;  /* 节点结束注释 */\
	char                            *name_comment;      /* Name操作符第一个参数的注释 */\
	char                            *close_brace_comment; /* 右大括号后的注释 */\
	struct acpi_comment_node        *comment_list;      /* 节点前的注释列表 */\
	struct acpi_comment_node        *end_blk_comment;   /* 代码块结束前的注释 */\
	char                            *cv_filename;       /* 关联的源文件名(ASL/ASL+转换用) */\
	char                            *cv_parent_filename)	/* 父节点关联的源文件名 */

/* categories of comments */

typedef enum {
	STANDARD_COMMENT = 1,
	INLINE_COMMENT,
	ENDNODE_COMMENT,
	OPENBRACE_COMMENT,
	CLOSE_BRACE_COMMENT,
	STD_DEFBLK_COMMENT,
	END_DEFBLK_COMMENT,
	FILENAME_COMMENT,
	PARENTFILENAME_COMMENT,
	ENDBLK_COMMENT,
	INCLUDE_COMMENT
} asl_comment_types;

/* Internal opcodes for disasm_opcode field above */

#define ACPI_DASM_BUFFER                0x00	/* Buffer is a simple data buffer */
#define ACPI_DASM_RESOURCE              0x01	/* Buffer is a Resource Descriptor */
#define ACPI_DASM_STRING                0x02	/* Buffer is a ASCII string */
#define ACPI_DASM_UNICODE               0x03	/* Buffer is a Unicode string */
#define ACPI_DASM_PLD_METHOD            0x04	/* Buffer is a _PLD method bit-packed buffer */
#define ACPI_DASM_UUID                  0x05	/* Buffer is a UUID/GUID */
#define ACPI_DASM_EISAID                0x06	/* Integer is an EISAID */
#define ACPI_DASM_MATCHOP               0x07	/* Parent opcode is a Match() operator */
#define ACPI_DASM_LNOT_PREFIX           0x08	/* Start of a Lnot_equal (etc.) pair of opcodes */
#define ACPI_DASM_LNOT_SUFFIX           0x09	/* End  of a Lnot_equal (etc.) pair of opcodes */
#define ACPI_DASM_HID_STRING            0x0A	/* String is a _HID or _CID */
#define ACPI_DASM_IGNORE_SINGLE         0x0B	/* Ignore the opcode but not it's children */
#define ACPI_DASM_SWITCH                0x0C	/* While is a Switch */
#define ACPI_DASM_SWITCH_PREDICATE      0x0D	/* Object is a predicate for a Switch or Case block */
#define ACPI_DASM_CASE                  0x0E	/* If/Else is a Case in a Switch/Case block */
#define ACPI_DASM_DEFAULT               0x0F	/* Else is a Default in a Switch/Case block */

/*
 * List struct used in the -ca option
 */
struct acpi_comment_node {
	char *comment;
	struct acpi_comment_node *next;
};

struct acpi_comment_addr_node {
	u8 *addr;
	struct acpi_comment_addr_node *next;
};

/*
 * File node - used for "Include" operator file stack and
 * dependency tree for the -ca option
 */
struct acpi_file_node {
	void *file;
	char *filename;
	char *file_start;	/* Points to AML and indicates when the AML for this particular file starts. */
	char *file_end;		/* Points to AML and indicates when the AML for this particular file ends. */
	struct acpi_file_node *next;
	struct acpi_file_node *parent;
	u8 include_written;
	struct acpi_comment_node *include_comment;
};

/*
 * Generic operation (for example:  If, While, Store)
 */
struct acpi_parse_obj_common {
ACPI_PARSE_COMMON};

/*
 * Extended Op for named ops (Scope, Method, etc.), deferred ops (Methods and op_regions),
 * and bytelists.
 */
struct acpi_parse_obj_named {
	ACPI_PARSE_COMMON char *path;
	u8 *data;		/* AML body or bytelist data */
	u32 length;		/* AML length */
	u32 name;		/* 4-byte name or zero if no name */
};

/* This version is used by the iASL compiler only */

#define ACPI_MAX_PARSEOP_NAME       20

struct acpi_parse_obj_asl {
	ACPI_PARSE_COMMON union acpi_parse_object *child;
	union acpi_parse_object *parent_method;
	char *filename;
	u8 file_changed;
	char *parent_filename;
	char *external_name;
	char *namepath;
	char name_seg[4];
	u32 extra_value;
	u32 column;
	u32 line_number;
	u32 logical_line_number;
	u32 logical_byte_offset;
	u32 end_line;
	u32 end_logical_line;
	u32 acpi_btype;
	u32 aml_length;
	u32 aml_subtree_length;
	u32 final_aml_length;
	u32 final_aml_offset;
	u32 compile_flags;
	u16 parse_opcode;
	u8 aml_opcode_length;
	u8 aml_pkg_len_bytes;
	u8 extra;
	char parse_op_name[ACPI_MAX_PARSEOP_NAME];
};
/*
 * union acpi_parse_object - ACPI解析树节点通用数据结构
 */
union acpi_parse_object {
	struct acpi_parse_obj_common common;//：所有解析对象通用字段
	struct acpi_parse_obj_named named;//命名对象扩展：用于Device/Method等需要名称绑定的对象
	struct acpi_parse_obj_asl asl;//ASL编译器专用：携带源代码级调试信息
};

struct asl_comment_state {
	u8 comment_type;
	u32 spaces_before;
	union acpi_parse_object *latest_parse_op;
	union acpi_parse_object *parsing_paren_brace_node;
	u8 capture_comments;
};

/*
 * Parse state - one state per parser invocation and each control
 * method.
 */
struct acpi_parse_state {
	u8 *aml_start;		/* First AML byte */
	u8 *aml;		/* Next AML byte */
	u8 *aml_end;		/* (last + 1) AML byte */
	u8 *pkg_start;		/* Current package begin */
	u8 *pkg_end;		/* Current package end */
	union acpi_parse_object *start_op;	/* Root of parse tree */
	struct acpi_namespace_node *start_node;
	union acpi_generic_state *scope;	/* Current scope */
	union acpi_parse_object *start_scope;
	u32 aml_size;
};

/* Parse object flags */

#define ACPI_PARSEOP_GENERIC                0x01
#define ACPI_PARSEOP_NAMED_OBJECT           0x02
#define ACPI_PARSEOP_DEFERRED               0x04
#define ACPI_PARSEOP_BYTELIST               0x08
#define ACPI_PARSEOP_IN_STACK               0x10
#define ACPI_PARSEOP_TARGET                 0x20
#define ACPI_PARSEOP_IN_CACHE               0x80

/* Parse object disasm_flags */

#define ACPI_PARSEOP_IGNORE                 0x0001
#define ACPI_PARSEOP_PARAMETER_LIST         0x0002
#define ACPI_PARSEOP_EMPTY_TERMLIST         0x0004
#define ACPI_PARSEOP_PREDEFINED_CHECKED     0x0008
#define ACPI_PARSEOP_CLOSING_PAREN          0x0010
#define ACPI_PARSEOP_COMPOUND_ASSIGNMENT    0x0020
#define ACPI_PARSEOP_ASSIGNMENT             0x0040
#define ACPI_PARSEOP_ELSEIF                 0x0080
#define ACPI_PARSEOP_LEGACY_ASL_ONLY        0x0100

/*****************************************************************************
 *
 * Hardware (ACPI registers) and PNP
 *
 ****************************************************************************/

struct acpi_bit_register_info {
	u8 parent_register;
	u8 bit_position;
	u16 access_bit_mask;
};

/*
 * Some ACPI registers have bits that must be ignored -- meaning that they
 * must be preserved.
 */
#define ACPI_PM1_STATUS_PRESERVED_BITS          0x0800	/* Bit 11 */

/* Write-only bits must be zeroed by software */

#define ACPI_PM1_CONTROL_WRITEONLY_BITS         0x2004	/* Bits 13, 2 */

/* For control registers, both ignored and reserved bits must be preserved */

/*
 * For PM1 control, the SCI enable bit (bit 0, SCI_EN) is defined by the
 * ACPI specification to be a "preserved" bit - "OSPM always preserves this
 * bit position", section 4.7.3.2.1. However, on some machines the OS must
 * write a one to this bit after resume for the machine to work properly.
 * To enable this, we no longer attempt to preserve this bit. No machines
 * are known to fail if the bit is not preserved. (May 2009)
 */
#define ACPI_PM1_CONTROL_IGNORED_BITS           0x0200	/* Bit 9 */
#define ACPI_PM1_CONTROL_RESERVED_BITS          0xC1F8	/* Bits 14-15, 3-8 */
#define ACPI_PM1_CONTROL_PRESERVED_BITS \
	       (ACPI_PM1_CONTROL_IGNORED_BITS | ACPI_PM1_CONTROL_RESERVED_BITS)

#define ACPI_PM2_CONTROL_PRESERVED_BITS         0xFFFFFFFE	/* All except bit 0 */

/*
 * Register IDs
 * These are the full ACPI registers
 */
#define ACPI_REGISTER_PM1_STATUS                0x01
#define ACPI_REGISTER_PM1_ENABLE                0x02
#define ACPI_REGISTER_PM1_CONTROL               0x03
#define ACPI_REGISTER_PM2_CONTROL               0x04
#define ACPI_REGISTER_PM_TIMER                  0x05
#define ACPI_REGISTER_PROCESSOR_BLOCK           0x06
#define ACPI_REGISTER_SMI_COMMAND_BLOCK         0x07

/* Masks used to access the bit_registers */

#define ACPI_BITMASK_TIMER_STATUS               0x0001
#define ACPI_BITMASK_BUS_MASTER_STATUS          0x0010
#define ACPI_BITMASK_GLOBAL_LOCK_STATUS         0x0020
#define ACPI_BITMASK_POWER_BUTTON_STATUS        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_STATUS        0x0200
#define ACPI_BITMASK_RT_CLOCK_STATUS            0x0400
#define ACPI_BITMASK_PCIEXP_WAKE_STATUS         0x4000	/* ACPI 3.0 */
#define ACPI_BITMASK_WAKE_STATUS                0x8000

#define ACPI_BITMASK_ALL_FIXED_STATUS           (\
	ACPI_BITMASK_TIMER_STATUS          | \
	ACPI_BITMASK_BUS_MASTER_STATUS     | \
	ACPI_BITMASK_GLOBAL_LOCK_STATUS    | \
	ACPI_BITMASK_POWER_BUTTON_STATUS   | \
	ACPI_BITMASK_SLEEP_BUTTON_STATUS   | \
	ACPI_BITMASK_RT_CLOCK_STATUS       | \
	ACPI_BITMASK_PCIEXP_WAKE_STATUS    | \
	ACPI_BITMASK_WAKE_STATUS)

#define ACPI_BITMASK_TIMER_ENABLE               0x0001
#define ACPI_BITMASK_GLOBAL_LOCK_ENABLE         0x0020
#define ACPI_BITMASK_POWER_BUTTON_ENABLE        0x0100
#define ACPI_BITMASK_SLEEP_BUTTON_ENABLE        0x0200
#define ACPI_BITMASK_RT_CLOCK_ENABLE            0x0400
#define ACPI_BITMASK_PCIEXP_WAKE_DISABLE        0x4000	/* ACPI 3.0 */

#define ACPI_BITMASK_SCI_ENABLE                 0x0001
#define ACPI_BITMASK_BUS_MASTER_RLD             0x0002
#define ACPI_BITMASK_GLOBAL_LOCK_RELEASE        0x0004
#define ACPI_BITMASK_SLEEP_TYPE                 0x1C00
#define ACPI_BITMASK_SLEEP_ENABLE               0x2000

#define ACPI_BITMASK_ARB_DISABLE                0x0001

/* Raw bit position of each bit_register */

#define ACPI_BITPOSITION_TIMER_STATUS           0x00
#define ACPI_BITPOSITION_BUS_MASTER_STATUS      0x04
#define ACPI_BITPOSITION_GLOBAL_LOCK_STATUS     0x05
#define ACPI_BITPOSITION_POWER_BUTTON_STATUS    0x08
#define ACPI_BITPOSITION_SLEEP_BUTTON_STATUS    0x09
#define ACPI_BITPOSITION_RT_CLOCK_STATUS        0x0A
#define ACPI_BITPOSITION_PCIEXP_WAKE_STATUS     0x0E	/* ACPI 3.0 */
#define ACPI_BITPOSITION_WAKE_STATUS            0x0F

#define ACPI_BITPOSITION_TIMER_ENABLE           0x00
#define ACPI_BITPOSITION_GLOBAL_LOCK_ENABLE     0x05
#define ACPI_BITPOSITION_POWER_BUTTON_ENABLE    0x08
#define ACPI_BITPOSITION_SLEEP_BUTTON_ENABLE    0x09
#define ACPI_BITPOSITION_RT_CLOCK_ENABLE        0x0A
#define ACPI_BITPOSITION_PCIEXP_WAKE_DISABLE    0x0E	/* ACPI 3.0 */

#define ACPI_BITPOSITION_SCI_ENABLE             0x00
#define ACPI_BITPOSITION_BUS_MASTER_RLD         0x01
#define ACPI_BITPOSITION_GLOBAL_LOCK_RELEASE    0x02
#define ACPI_BITPOSITION_SLEEP_TYPE             0x0A
#define ACPI_BITPOSITION_SLEEP_ENABLE           0x0D

#define ACPI_BITPOSITION_ARB_DISABLE            0x00

/* Structs and definitions for _OSI support and I/O port validation */

#define ACPI_ALWAYS_ILLEGAL             0x00

struct acpi_interface_info {
	char *name;
	struct acpi_interface_info *next;
	u8 flags;
	u8 value;
};

#define ACPI_OSI_INVALID                0x01
#define ACPI_OSI_DYNAMIC                0x02
#define ACPI_OSI_FEATURE                0x04
#define ACPI_OSI_DEFAULT_INVALID        0x08
#define ACPI_OSI_OPTIONAL_FEATURE       (ACPI_OSI_FEATURE | ACPI_OSI_DEFAULT_INVALID | ACPI_OSI_INVALID)

struct acpi_port_info {
	char *name;
	u16 start;
	u16 end;
	u8 osi_dependency;
};

/*****************************************************************************
 *
 * Resource descriptors
 *
 ****************************************************************************/

/* resource_type values */

#define ACPI_ADDRESS_TYPE_MEMORY_RANGE          0
#define ACPI_ADDRESS_TYPE_IO_RANGE              1
#define ACPI_ADDRESS_TYPE_BUS_NUMBER_RANGE      2

/* Resource descriptor types and masks */

#define ACPI_RESOURCE_NAME_LARGE                0x80
#define ACPI_RESOURCE_NAME_SMALL                0x00

#define ACPI_RESOURCE_NAME_SMALL_MASK           0x78	/* Bits 6:3 contain the type */
#define ACPI_RESOURCE_NAME_SMALL_LENGTH_MASK    0x07	/* Bits 2:0 contain the length */
#define ACPI_RESOURCE_NAME_LARGE_MASK           0x7F	/* Bits 6:0 contain the type */

/*
 * Small resource descriptor "names" as defined by the ACPI specification.
 * Note: Bits 2:0 are used for the descriptor length
 */
#define ACPI_RESOURCE_NAME_IRQ                  0x20
#define ACPI_RESOURCE_NAME_DMA                  0x28
#define ACPI_RESOURCE_NAME_START_DEPENDENT      0x30
#define ACPI_RESOURCE_NAME_END_DEPENDENT        0x38
#define ACPI_RESOURCE_NAME_IO                   0x40
#define ACPI_RESOURCE_NAME_FIXED_IO             0x48
#define ACPI_RESOURCE_NAME_FIXED_DMA            0x50
#define ACPI_RESOURCE_NAME_RESERVED_S2          0x58
#define ACPI_RESOURCE_NAME_RESERVED_S3          0x60
#define ACPI_RESOURCE_NAME_RESERVED_S4          0x68
#define ACPI_RESOURCE_NAME_VENDOR_SMALL         0x70
#define ACPI_RESOURCE_NAME_END_TAG              0x78

/*
 * Large resource descriptor "names" as defined by the ACPI specification.
 * Note: includes the Large Descriptor bit in bit[7]
 */
#define ACPI_RESOURCE_NAME_MEMORY24             0x81
#define ACPI_RESOURCE_NAME_GENERIC_REGISTER     0x82
#define ACPI_RESOURCE_NAME_RESERVED_L1          0x83
#define ACPI_RESOURCE_NAME_VENDOR_LARGE         0x84
#define ACPI_RESOURCE_NAME_MEMORY32             0x85
#define ACPI_RESOURCE_NAME_FIXED_MEMORY32       0x86
#define ACPI_RESOURCE_NAME_ADDRESS32            0x87
#define ACPI_RESOURCE_NAME_ADDRESS16            0x88
#define ACPI_RESOURCE_NAME_EXTENDED_IRQ         0x89
#define ACPI_RESOURCE_NAME_ADDRESS64            0x8A
#define ACPI_RESOURCE_NAME_EXTENDED_ADDRESS64   0x8B
#define ACPI_RESOURCE_NAME_GPIO                 0x8C
#define ACPI_RESOURCE_NAME_PIN_FUNCTION         0x8D
#define ACPI_RESOURCE_NAME_SERIAL_BUS           0x8E
#define ACPI_RESOURCE_NAME_PIN_CONFIG           0x8F
#define ACPI_RESOURCE_NAME_PIN_GROUP            0x90
#define ACPI_RESOURCE_NAME_PIN_GROUP_FUNCTION   0x91
#define ACPI_RESOURCE_NAME_PIN_GROUP_CONFIG     0x92
#define ACPI_RESOURCE_NAME_CLOCK_INPUT          0x93
#define ACPI_RESOURCE_NAME_LARGE_MAX            0x94

/*****************************************************************************
 *
 * Miscellaneous
 *
 ****************************************************************************/

#define ACPI_ASCII_ZERO                 0x30

/*****************************************************************************
 *
 * Disassembler
 *
 ****************************************************************************/

struct acpi_external_list {
	char *path;
	char *internal_path;
	struct acpi_external_list *next;
	u32 value;
	u16 length;
	u16 flags;
	u8 type;
};

/* Values for Flags field above */

#define ACPI_EXT_RESOLVED_REFERENCE         0x01	/* Object was resolved during cross ref */
#define ACPI_EXT_ORIGIN_FROM_FILE           0x02	/* External came from a file */
#define ACPI_EXT_INTERNAL_PATH_ALLOCATED    0x04	/* Deallocate internal path on completion */
#define ACPI_EXT_EXTERNAL_EMITTED           0x08	/* External() statement has been emitted */
#define ACPI_EXT_ORIGIN_FROM_OPCODE         0x10	/* External came from a External() opcode */
#define ACPI_EXT_CONFLICTING_DECLARATION    0x20	/* External has a conflicting declaration within AML */

struct acpi_external_file {
	char *path;
	struct acpi_external_file *next;
};

struct acpi_parse_object_list {
	union acpi_parse_object *op;
	struct acpi_parse_object_list *next;
};

/*****************************************************************************
 *
 * Debugger
 *
 ****************************************************************************/

struct acpi_db_method_info {
	acpi_handle method;
	acpi_handle main_thread_gate;
	acpi_handle thread_complete_gate;
	acpi_handle info_gate;
	acpi_thread_id *threads;
	u32 num_threads;
	u32 num_created;
	u32 num_completed;

	char *name;
	u32 flags;
	u32 num_loops;
	char pathname[ACPI_DB_LINE_BUFFER_SIZE];
	char **args;
	acpi_object_type *types;

	/*
	 * Arguments to be passed to method for the commands Threads and
	 * Background. Note, ACPI specifies a maximum of 7 arguments (0 - 6).
	 *
	 * For the Threads command, the Number of threads, ID of current
	 * thread and Index of current thread inside all them created.
	 */
	char init_args;
#ifdef ACPI_DEBUGGER
	acpi_object_type arg_types[ACPI_METHOD_NUM_ARGS];
#endif
	char *arguments[ACPI_METHOD_NUM_ARGS];
	char num_threads_str[11];
	char id_of_thread_str[11];
	char index_of_thread_str[11];
};

struct acpi_integrity_info {
	u32 nodes;
	u32 objects;
};

#define ACPI_DB_DISABLE_OUTPUT          0x00
#define ACPI_DB_REDIRECTABLE_OUTPUT     0x01
#define ACPI_DB_CONSOLE_OUTPUT          0x02
#define ACPI_DB_DUPLICATE_OUTPUT        0x03

struct acpi_object_info {
	u32 types[ACPI_TOTAL_TYPES];
};

/*****************************************************************************
 *
 * Debug
 *
 ****************************************************************************/

/* Entry for a memory allocation (debug only) */

#define ACPI_MEM_MALLOC                 0
#define ACPI_MEM_CALLOC                 1
#define ACPI_MAX_MODULE_NAME            16

#define ACPI_COMMON_DEBUG_MEM_HEADER \
	struct acpi_debug_mem_block     *previous; \
	struct acpi_debug_mem_block     *next; \
	u32                             size; \
	u32                             component; \
	u32                             line; \
	char                            module[ACPI_MAX_MODULE_NAME]; \
	u8                              alloc_type;

struct acpi_debug_mem_header {
ACPI_COMMON_DEBUG_MEM_HEADER};

struct acpi_debug_mem_block {
	ACPI_COMMON_DEBUG_MEM_HEADER u64 user_space;
};

#define ACPI_MEM_LIST_GLOBAL            0
#define ACPI_MEM_LIST_NSNODE            1
#define ACPI_MEM_LIST_MAX               1
#define ACPI_NUM_MEM_LISTS              2

/*****************************************************************************
 *
 * Info/help support
 *
 ****************************************************************************/

struct ah_predefined_name {
	char *name;
	char *description;
#ifndef ACPI_ASL_COMPILER
	char *action;
#endif
};

struct ah_device_id {
	char *name;
	char *description;
};

struct ah_uuid {
	char *description;
	char *string;
};

struct ah_table {
	char *signature;
	char *description;
};

#endif				/* __ACLOCAL_H__ */
