/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/******************************************************************************
 *
 * Name: acobject.h - Definition of union acpi_operand_object  (Internal object only)
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#ifndef _ACOBJECT_H
#define _ACOBJECT_H

/* acpisrc:struct_defs -- for acpisrc conversion */

/*
 * The union acpi_operand_object is used to pass AML operands from the dispatcher
 * to the interpreter, and to keep track of the various handlers such as
 * address space handlers and notify handlers. The object is a constant
 * size in order to allow it to be cached and reused.
 *
 * Note: The object is optimized to be aligned and will not work if it is
 * byte-packed.
 */
#if ACPI_MACHINE_WIDTH == 64
#pragma pack(8)
#else
#pragma pack(4)
#endif

/*******************************************************************************
 *
 * Common Descriptors
 *
 ******************************************************************************/

/*
 * Common area for all objects.
 *
 * descriptor_type is used to differentiate between internal descriptors, and
 * must be in the same place across all descriptors
 *
 * Note: The descriptor_type and Type fields must appear in the identical
 * position in both the struct acpi_namespace_node and union acpi_operand_object
 * structures.
 */
#define ACPI_OBJECT_COMMON_HEADER \
	union acpi_operand_object       *next_object;       /* Objects linked to parent NS node */\
	u8                              descriptor_type;    /* To differentiate various internal objs */\
	u8                              type;               /* acpi_object_type */\
	u16                             reference_count;    /* For object deletion management */\
	u8                              flags
	/*
	 * Note: There are 3 bytes available here before the
	 * next natural alignment boundary (for both 32/64 cases)
	 */

/* Values for Flag byte above */

#define AOPOBJ_AML_CONSTANT         0x01	/* Integer is an AML constant */
#define AOPOBJ_STATIC_POINTER       0x02	/* Data is part of an ACPI table, don't delete */
#define AOPOBJ_DATA_VALID           0x04	/* Object is initialized and data is valid */
#define AOPOBJ_OBJECT_INITIALIZED   0x08	/* Region is initialized */
#define AOPOBJ_REG_CONNECTED        0x10	/* _REG was run */
#define AOPOBJ_SETUP_COMPLETE       0x20	/* Region setup is complete */
#define AOPOBJ_INVALID              0x40	/* Host OS won't allow a Region address */

/******************************************************************************
 *
 * Basic data types
 *
 *****************************************************************************/

struct acpi_object_common {
	ACPI_OBJECT_COMMON_HEADER;
};

/* ACPI 整数对象结构体,用于表示AML解释器中的整型数据（64位无符号） */
struct acpi_object_integer {
	ACPI_OBJECT_COMMON_HEADER;//标准ACPI对象头
	u8 fill[3];//填充字节（用于结构体对齐和编译器警告消除）
	u64 value;//存储的实际整数值
};

/*
 * Note: The String and Buffer object must be identical through the
 * pointer and length elements. There is code that depends on this.
 *
 * Fields common to both Strings and Buffers
 */
#define ACPI_COMMON_BUFFER_INFO(_type) \
	_type                           *pointer; \
	u32                             length

/* Null terminated, ASCII characters only */

struct acpi_object_string {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_BUFFER_INFO(char);	/* String in AML stream or allocated string */
};

struct acpi_object_buffer {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_BUFFER_INFO(u8);	/* Buffer in AML stream or allocated buffer */
	u32 aml_length;
	u8 *aml_start;
	struct acpi_namespace_node *node;	/* Link back to parent node */
};

/*
 * struct acpi_object_package - ACPI包对象(Package)的内部表示
 *
 * 该结构定义了ACPI包对象的完整内存表示，用于存储有序的对象集合。
 * 包是ACPI中最重要的复合数据类型之一，用于实现：
 * - 方法参数传递
 * - 可变长度数据集合
 * - 硬件寄存器组表示
 */
struct acpi_object_package {
	ACPI_OBJECT_COMMON_HEADER;//标准对象头(类型/引用计数等)
	struct acpi_namespace_node *node;	//回指父命名空间节点
	union acpi_operand_object **elements;	//对象指针数组
	u8 *aml_start;//原始AML代码起始位置
	u32 aml_length;//包定义的AML代码长度
	u32 count;		//实际元素数量
};

/******************************************************************************
 *
 * Complex data types
 *
 *****************************************************************************/

struct acpi_object_event {
	ACPI_OBJECT_COMMON_HEADER;
	acpi_semaphore os_semaphore;	/* Actual OS synchronization object */
};

struct acpi_object_mutex {
	ACPI_OBJECT_COMMON_HEADER;
	u8 sync_level;		/* 0-15, specified in Mutex() call */
	u16 acquisition_depth;	/* Allow multiple Acquires, same thread */
	acpi_mutex os_mutex;	/* Actual OS synchronization object */
	acpi_thread_id thread_id;	/* Current owner of the mutex */
	struct acpi_thread_state *owner_thread;	/* Current owner of the mutex */
	union acpi_operand_object *prev;	/* Link for list of acquired mutexes */
	union acpi_operand_object *next;	/* Link for list of acquired mutexes */
	struct acpi_namespace_node *node;	/* Containing namespace node */
	u8 original_sync_level;	/* Owner's original sync level (0-15) */
};

struct acpi_object_region {
	ACPI_OBJECT_COMMON_HEADER;
	u8 space_id;
	struct acpi_namespace_node *node;	/* Containing namespace node */
	union acpi_operand_object *handler;	/* Handler for region access */
	union acpi_operand_object *next;
	acpi_physical_address address;
	u32 length;
	void *pointer;		/* Only for data table regions */
};

struct acpi_object_method {
	ACPI_OBJECT_COMMON_HEADER;//基础对象头（类型、引用计数等）
	u8 info_flags;//方法标志位集合
	u8 param_count;//方法需要的参数数量
	u8 sync_level;//同步级别（0-7），表示方法执行所需的最小同步级别
	union acpi_operand_object *mutex;//互斥锁对象
	union acpi_operand_object *node;//命名空间节点（方法在命名空间中的位置）
	u8 *aml_start;//指向方法AML代码的起始地址（方法执行时的指令流入口）
	union {
		acpi_internal_method implementation;//内置方法实现函数指针
		union acpi_operand_object *handler;//外部方法处理程序对象（如用户注册的回调）
	} dispatch;

	u32 aml_length;//AML代码长度（用于确定方法指令流的结束位置）
	acpi_owner_id owner_id;//所有权ID（用于跟踪方法创建的命名空间对象）
	u8 thread_count;//当前执行该方法的线程数（用于多线程同步）
};

/* Flags for info_flags field above */

#define ACPI_METHOD_MODULE_LEVEL        0x01	/* Method is actually module-level code */
#define ACPI_METHOD_INTERNAL_ONLY       0x02	/* Method is implemented internally (_OSI) */
#define ACPI_METHOD_SERIALIZED          0x04	/* Method is serialized */
#define ACPI_METHOD_SERIALIZED_PENDING  0x08	/* Method is to be marked serialized */
#define ACPI_METHOD_IGNORE_SYNC_LEVEL   0x10	/* Method was auto-serialized at table load time */
#define ACPI_METHOD_MODIFIED_NAMESPACE  0x20	/* Method modified the namespace */

/******************************************************************************
 *
 * Objects that can be notified. All share a common notify_info area.
 *
 *****************************************************************************/

/*
 * Common fields for objects that support ASL notifications
 */
#define ACPI_COMMON_NOTIFY_INFO \
	union acpi_operand_object       *notify_list[2];    /* Handlers for system/device notifies */\
	union acpi_operand_object       *handler	/* Handler for Address space */

/* COMMON NOTIFY for POWER, PROCESSOR, DEVICE, and THERMAL */

struct acpi_object_notify_common {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_NOTIFY_INFO;
};

struct acpi_object_device {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_NOTIFY_INFO;
	struct acpi_gpe_block_info *gpe_block;
};

struct acpi_object_power_resource {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_NOTIFY_INFO;
	u32 system_level;
	u32 resource_order;
};

struct acpi_object_processor {
	ACPI_OBJECT_COMMON_HEADER;

	/* The next two fields take advantage of the 3-byte space before NOTIFY_INFO */

	u8 proc_id;
	u8 length;
	ACPI_COMMON_NOTIFY_INFO;
	acpi_io_address address;
};

struct acpi_object_thermal_zone {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_NOTIFY_INFO;
};

/******************************************************************************
 *
 * Fields. All share a common header/info field.
 *
 *****************************************************************************/

/*
 * Common bitfield for the field objects
 * "Field Datum"  -- a datum from the actual field object
 * "Buffer Datum" -- a datum from a user buffer, read from or to be written to the field
 */
#define ACPI_COMMON_FIELD_INFO \
	u8                              field_flags;        /* Access, update, and lock bits */\
	u8                              attribute;          /* From access_as keyword */\
	u8                              access_byte_width;  /* Read/Write size in bytes */\
	struct acpi_namespace_node      *node;              /* Link back to parent node */\
	u32                             bit_length;         /* Length of field in bits */\
	u32                             base_byte_offset;   /* Byte offset within containing object */\
	u32                             value;              /* Value to store into the Bank or Index register */\
	u8                              start_field_bit_offset;/* Bit offset within first field datum (0-63) */\
	u8                              access_length	/* For serial regions/fields */


/* COMMON FIELD (for BUFFER, REGION, BANK, and INDEX fields) */

struct acpi_object_field_common {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_FIELD_INFO;
	union acpi_operand_object *region_obj;	/* Parent Operation Region object (REGION/BANK fields only) */
};

struct acpi_object_region_field {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_FIELD_INFO;
	u16 resource_length;
	union acpi_operand_object *region_obj;	/* Containing op_region object */
	u8 *resource_buffer;	/* resource_template for serial regions/fields */
	u16 pin_number_index;	/* Index relative to previous Connection/Template */
	u8 *internal_pcc_buffer;	/* Internal buffer for fields associated with PCC */
};

struct acpi_object_bank_field {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_FIELD_INFO;
	union acpi_operand_object *region_obj;	/* Containing op_region object */
	union acpi_operand_object *bank_obj;	/* bank_select Register object */
};

struct acpi_object_index_field {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_FIELD_INFO;

	/*
	 * No "RegionObj" pointer needed since the Index and Data registers
	 * are each field definitions unto themselves.
	 */
	union acpi_operand_object *index_obj;	/* Index register */
	union acpi_operand_object *data_obj;	/* Data register */
};

/* The buffer_field is different in that it is part of a Buffer, not an op_region */

struct acpi_object_buffer_field {
	ACPI_OBJECT_COMMON_HEADER;
	ACPI_COMMON_FIELD_INFO;
	u8 is_create_field;	/* Special case for objects created by create_field() */
	union acpi_operand_object *buffer_obj;	/* Containing Buffer object */
};

/******************************************************************************
 *
 * Objects for handlers
 *
 *****************************************************************************/

struct acpi_object_notify_handler {
	ACPI_OBJECT_COMMON_HEADER;
	struct acpi_namespace_node *node;	/* Parent device */
	u32 handler_type;	/* Type: Device/System/Both */
	acpi_notify_handler handler;	/* Handler address */
	void *context;
	union acpi_operand_object *next[2];	/* Device and System handler lists */
};

struct acpi_object_addr_handler {
	ACPI_OBJECT_COMMON_HEADER;//ACPI对象通用头（类型、引用计数等）
	u8 space_id;//地址空间类型标识符（如内存、IO、PCI配置空间等）
	u8 handler_flags;//处理程序标志位（控制行为，如只读、可写等）
	acpi_adr_space_handler handler;//地址空间访问回调函数（读写操作处理）
	struct acpi_namespace_node *node;//父设备节点（命名空间中的所属设备）
	void *context;//用户自定义上下文数据（设备特定参数）
	acpi_mutex context_mutex;//上下文数据的互斥锁（多线程访问保护）
	acpi_adr_space_setup setup;//初始化回调（安装处理程序时调用）
	union acpi_operand_object *region_list;	//使用该处理程序的区域对象链表头
	union acpi_operand_object *next;//链表指针（用于将多个处理程序对象链接）
};

/* Flags for address handler (handler_flags) */

#define ACPI_ADDR_HANDLER_DEFAULT_INSTALLED  0x01

/******************************************************************************
 *
 * Special internal objects
 *
 *****************************************************************************/

/*
 * The Reference object is used for these opcodes:
 * Arg[0-6], Local[0-7], index_op, name_op, ref_of_op, load_op, load_table_op, debug_op
 * The Reference.Class differentiates these types.
 */
struct acpi_object_reference {
	ACPI_OBJECT_COMMON_HEADER;
	u8 class;		/* Reference Class */
	u8 target_type;		/* Used for Index Op */
	u8 resolved;		/* Reference has been resolved to a value */
	void *object;		/* name_op=>HANDLE to obj, index_op=>union acpi_operand_object */
	struct acpi_namespace_node *node;	/* ref_of or Namepath */
	union acpi_operand_object **where;	/* Target of Index */
	u8 *index_pointer;	/* Used for Buffers and Strings */
	u8 *aml;		/* Used for deferred resolution of the ref */
	u32 value;		/* Used for Local/Arg/Index/ddb_handle */
};

/* Values for Reference.Class above */

typedef enum {
	ACPI_REFCLASS_LOCAL = 0,	/* Method local */
	ACPI_REFCLASS_ARG = 1,	/* Method argument */
	ACPI_REFCLASS_REFOF = 2,	/* Result of ref_of() TBD: Split to Ref/Node and Ref/operand_obj? */
	ACPI_REFCLASS_INDEX = 3,	/* Result of Index() */
	ACPI_REFCLASS_TABLE = 4,	/* ddb_handle - Load(), load_table() */
	ACPI_REFCLASS_NAME = 5,	/* Reference to a named object */
	ACPI_REFCLASS_DEBUG = 6,	/* Debug object */

	ACPI_REFCLASS_MAX = 6
} ACPI_REFERENCE_CLASSES;

/*
 * Extra object is used as additional storage for types that
 * have AML code in their declarations (term_args) that must be
 * evaluated at run time.
 *
 * Currently: Region and field_unit types
 */
struct acpi_object_extra {
	ACPI_OBJECT_COMMON_HEADER;//标准ACPI对象头（包含type/flags等）

	/* _REG 方法相关 */
	struct acpi_namespace_node *method_REG;//指向该区域关联的_REG方法的命名空间节点,当操作区域的处理程序连接/断开时执行

	/* 命名空间节点 */
	struct acpi_namespace_node *scope_node;//该区域所属的命名空间范围节点,用于确定对象的解析上下文
	void *region_context;//区域处理程序的私有数据指针,内容由具体的区域处理程序定义和使用
	u8 *aml_start;//AML 代码相关,指向该区域定义在AML中的起始地址,用于动态区域的重解析
	u32 aml_length;//该区域定义在AML中的代码长度,与aml_start配合使用
};

/* Additional data that can be attached to namespace nodes */

struct acpi_object_data {
	ACPI_OBJECT_COMMON_HEADER;
	acpi_object_handler handler;
	void *pointer;
};

/* Structure used when objects are cached for reuse */

struct acpi_object_cache_list {
	ACPI_OBJECT_COMMON_HEADER;
	union acpi_operand_object *next;	/* Link for object cache and internal lists */
};

/******************************************************************************
 *
 * union acpi_operand_object descriptor - a giant union of all of the above
 *
 *****************************************************************************/
/**
 * union acpi_operand_object - ACPI操作对象联合体
 * 
 * 表示ACPI解释器可以处理的所有对象类型，通过共用体实现类型多态。
 * 包含24种不同类型的ACPI对象，共享相同的内存空间。
 */
union acpi_operand_object {
	struct acpi_object_common common;//基础对象头(所有对象类型共有)
	struct acpi_object_integer integer;//整数对象(64位有符号)
	struct acpi_object_string string;//字符串对象(可变长度)
	struct acpi_object_buffer buffer;//缓冲区对象(二进制数据)
	struct acpi_object_package package;//包对象(对象数组)
	struct acpi_object_event event;//事件同步对象 
	struct acpi_object_method method;//控制方法对象(AML字节码)
	struct acpi_object_mutex mutex;//互斥体对象
	struct acpi_object_region region;//操作地址空间区域对象(寄存器空间映射)
	struct acpi_object_notify_common common_notify;//地址空间处理程序对象
	struct acpi_object_device device;//设备对象
	struct acpi_object_power_resource power_resource;//电源资源
	struct acpi_object_processor processor;//处理器对象
	struct acpi_object_thermal_zone thermal_zone;//热区对象
	/* 字段单元对象(多种变体) */
	struct acpi_object_field_common common_field;//字段公共部分
	struct acpi_object_region_field field;//区域字段 
	struct acpi_object_buffer_field buffer_field;//缓冲区字段 
	struct acpi_object_bank_field bank_field;//bank字段
	struct acpi_object_index_field index_field;//索引字段
	/* 通知/引用对象 */
	struct acpi_object_notify_handler notify;//通知处理程序
	struct acpi_object_addr_handler address_space;//处理程序对象
	struct acpi_object_reference reference;//对象引用
	/* 特殊用途对象 */
	struct acpi_object_extra extra;//扩展信息对象
	struct acpi_object_data data;//数据对象
	struct acpi_object_cache_list cache;//对象缓存列表 

	/*
	 * Add namespace node to union in order to simplify code that accepts both
	 * ACPI_OPERAND_OBJECTs and ACPI_NAMESPACE_NODEs. The structures share
	 * a common descriptor_type field in order to differentiate them.
	 */
	struct acpi_namespace_node node;//命名空间节点联合(特殊设计)，允许代码同时处理操作对象和命名空间节点，通过descriptor_type字段区分实际类型
};

/******************************************************************************
 *
 * union acpi_descriptor - objects that share a common descriptor identifier
 *
 *****************************************************************************/

/* Object descriptor types */

#define ACPI_DESC_TYPE_CACHED           0x01	/* Used only when object is cached */
#define ACPI_DESC_TYPE_STATE            0x02
#define ACPI_DESC_TYPE_STATE_UPDATE     0x03
#define ACPI_DESC_TYPE_STATE_PACKAGE    0x04
#define ACPI_DESC_TYPE_STATE_CONTROL    0x05
#define ACPI_DESC_TYPE_STATE_RPSCOPE    0x06
#define ACPI_DESC_TYPE_STATE_PSCOPE     0x07
#define ACPI_DESC_TYPE_STATE_WSCOPE     0x08
#define ACPI_DESC_TYPE_STATE_RESULT     0x09
#define ACPI_DESC_TYPE_STATE_NOTIFY     0x0A
#define ACPI_DESC_TYPE_STATE_THREAD     0x0B
#define ACPI_DESC_TYPE_WALK             0x0C
#define ACPI_DESC_TYPE_PARSER           0x0D
#define ACPI_DESC_TYPE_OPERAND          0x0E
#define ACPI_DESC_TYPE_NAMED            0x0F
#define ACPI_DESC_TYPE_MAX              0x0F

struct acpi_common_descriptor {
	void *common_pointer;
	u8 descriptor_type;	/* To differentiate various internal objs */
};

union acpi_descriptor {
	struct acpi_common_descriptor common;
	union acpi_operand_object object;
	struct acpi_namespace_node node;
	union acpi_parse_object op;
};

#pragma pack()

#endif				/* _ACOBJECT_H */
