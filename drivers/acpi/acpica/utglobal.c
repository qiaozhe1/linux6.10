// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: utglobal - Global variables for the ACPI subsystem
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#define EXPORT_ACPI_INTERFACES
#define DEFINE_ACPI_GLOBALS

#include <acpi/acpi.h>
#include "accommon.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utglobal")

/*******************************************************************************
 *
 * Static global variable initialization.
 *
 ******************************************************************************/
/* Various state name strings */
const char *acpi_gbl_sleep_state_names[ACPI_S_STATE_COUNT] = {
	"\\_S0_",
	"\\_S1_",
	"\\_S2_",
	"\\_S3_",
	"\\_S4_",
	"\\_S5_"
};

const char *acpi_gbl_lowest_dstate_names[ACPI_NUM_sx_w_METHODS] = {
	"_S0W",
	"_S1W",
	"_S2W",
	"_S3W",
	"_S4W"
};

const char *acpi_gbl_highest_dstate_names[ACPI_NUM_sx_d_METHODS] = {
	"_S1D",
	"_S2D",
	"_S3D",
	"_S4D"
};

/* Hex-to-ascii */

const char acpi_gbl_lower_hex_digits[] = "0123456789abcdef";
const char acpi_gbl_upper_hex_digits[] = "0123456789ABCDEF";

/*******************************************************************************
 *
 * Namespace globals
 *
 ******************************************************************************/
/*
 * Predefined ACPI Names (Built-in to the Interpreter)
 *
 * NOTES:
 * 1) _SB_ is defined to be a device to allow \_SB_._INI to be run
 *    during the initialization sequence.
 * 2) _TZ_ is defined to be a thermal zone in order to allow ASL code to
 *    perform a Notify() operation on it. 09/2010: Changed to type Device.
 *    This still allows notifies, but does not confuse host code that
 *    searches for valid thermal_zone objects.
 */
/*
 * 定义ACPI预定义名称数组:
 * 每个条目包含名称、类型和初始值指针
 * 用于注册ACPI命名空间中的预定义对象（如系统基板、电源事件等）
 * */
const struct acpi_predefined_names acpi_gbl_pre_defined_names[] = {
	/*
	 * 预定义名称"_GPE"：
	 * 类型：ACPI_TYPE_LOCAL_SCOPE（局部作用域对象）
	 * 用途：定义通用电源事件（General Purpose Events）的作用域
	 * NULL表示无初始值（需通过ACPI表或方法初始化）
	 * */
	{"_GPE", ACPI_TYPE_LOCAL_SCOPE, NULL},
	/*
	 * 预定义名称"_PR_"：
	 * 类型：ACPI_TYPE_LOCAL_SCOPE
	 * 用途：电源资源作用域（Power Resources Scope）
	 * */
	{"_PR_", ACPI_TYPE_LOCAL_SCOPE, NULL},
	/*
	 * 预定义名称"_SB_"：
	 * 类型：ACPI_TYPE_DEVICE（设备对象）
	 * 用途：系统基（System Board）设备节点 → 命名空间根下的第一个设备
	 * 所有硬件设备（如CPU、PCI设备）均挂载在此节点下
	 * */
	{"_SB_", ACPI_TYPE_DEVICE, NULL},
	/*
	 * 预定义名称"_SI_"：
	 * 类型：ACPI_TYPE_LOCAL_SCOPE
	 * 用途：系统指示器作用域（System Indicators Scope）
	 * */
	{"_SI_", ACPI_TYPE_LOCAL_SCOPE, NULL},
	/*
	 * 预定义名称"_TZ_"：
	 * 类型：ACPI_TYPE_DEVICE
	 * 用途：热区（Thermal Zone）设备 → 用于温度管理
	 * */
	{"_TZ_", ACPI_TYPE_DEVICE, NULL},
	/*
	 * March, 2015:
	 * _REV对象正在被弃用，因为其他ACPI实现永久返回2。因此，该对象的值
	 * 对兼容性帮助不大。为保持与其他ACPI实现一致，返回2。
	 *
	 * 预定义名称"_REV"：
	 * 类型：ACPI_TYPE_INTEGER（整数对象）
	 * 初始值：强制设为2 → 表示ACPI规范版本（尽管已弃用）
	 * ACPI_CAST_PTR(char, 2)：将整数2转换为char指针类型（符合数组字段要求）
	 */
	{"_REV", ACPI_TYPE_INTEGER, ACPI_CAST_PTR(char, 2)},
	/*
	 * 预定义名称"_OS_"：
	 * 类型：ACPI_TYPE_STRING（字符串对象）
	 * 初始值：指向内核定义的操作系统名称（如"Microsoft Windows 2000"）
	 * 用途：用于ACPI设备检测操作系统类型（如_OSI方法）
	 * */
	{"_OS_", ACPI_TYPE_STRING, ACPI_OS_NAME},
	/*
	 * 预定义名称"_GL_"：
	 * 类型：ACPI_TYPE_MUTEX（互斥锁对象）
	 * 初始值：1 → 表示互斥锁的最大共享层次（1表示独占模式）
	 * 用途：用于同步对共享资源的访问（如热键事件）
	 * */
	{"_GL_", ACPI_TYPE_MUTEX, ACPI_CAST_PTR(char, 1)},
	/*
	 * 预定义名称"_OSI"：
	 * 类型：ACPI_TYPE_METHOD（方法对象）
	 * 初始值：1 → 表示方法参数计数（_OSI方法需一个字符串参数，如操作系统名称）
	 * 用途：操作系统接口方法 → 用于检测操作系统支持的ACPI特性（如Linux/Windows支持）
	 * */
	{"_OSI", ACPI_TYPE_METHOD, ACPI_CAST_PTR(char, 1)},

	/* Table terminator */

	{NULL, ACPI_TYPE_ANY, NULL}//数组终止符
};

#if (!ACPI_REDUCED_HARDWARE)
/******************************************************************************
 *
 * Event and Hardware globals
 *
 ******************************************************************************/

struct acpi_bit_register_info acpi_gbl_bit_register_info[ACPI_NUM_BITREG] = {
	/* Name                                     Parent Register             Register Bit Position                   Register Bit Mask       */

	/* ACPI_BITREG_TIMER_STATUS         */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_TIMER_STATUS,
						ACPI_BITMASK_TIMER_STATUS},
	/* ACPI_BITREG_BUS_MASTER_STATUS    */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_BUS_MASTER_STATUS,
						ACPI_BITMASK_BUS_MASTER_STATUS},
	/* ACPI_BITREG_GLOBAL_LOCK_STATUS   */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_GLOBAL_LOCK_STATUS,
						ACPI_BITMASK_GLOBAL_LOCK_STATUS},
	/* ACPI_BITREG_POWER_BUTTON_STATUS  */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_POWER_BUTTON_STATUS,
						ACPI_BITMASK_POWER_BUTTON_STATUS},
	/* ACPI_BITREG_SLEEP_BUTTON_STATUS  */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_SLEEP_BUTTON_STATUS,
						ACPI_BITMASK_SLEEP_BUTTON_STATUS},
	/* ACPI_BITREG_RT_CLOCK_STATUS      */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_RT_CLOCK_STATUS,
						ACPI_BITMASK_RT_CLOCK_STATUS},
	/* ACPI_BITREG_WAKE_STATUS          */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_WAKE_STATUS,
						ACPI_BITMASK_WAKE_STATUS},
	/* ACPI_BITREG_PCIEXP_WAKE_STATUS   */ {ACPI_REGISTER_PM1_STATUS,
						ACPI_BITPOSITION_PCIEXP_WAKE_STATUS,
						ACPI_BITMASK_PCIEXP_WAKE_STATUS},

	/* ACPI_BITREG_TIMER_ENABLE         */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_TIMER_ENABLE,
						ACPI_BITMASK_TIMER_ENABLE},
	/* ACPI_BITREG_GLOBAL_LOCK_ENABLE   */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_GLOBAL_LOCK_ENABLE,
						ACPI_BITMASK_GLOBAL_LOCK_ENABLE},
	/* ACPI_BITREG_POWER_BUTTON_ENABLE  */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_POWER_BUTTON_ENABLE,
						ACPI_BITMASK_POWER_BUTTON_ENABLE},
	/* ACPI_BITREG_SLEEP_BUTTON_ENABLE  */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_SLEEP_BUTTON_ENABLE,
						ACPI_BITMASK_SLEEP_BUTTON_ENABLE},
	/* ACPI_BITREG_RT_CLOCK_ENABLE      */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_RT_CLOCK_ENABLE,
						ACPI_BITMASK_RT_CLOCK_ENABLE},
	/* ACPI_BITREG_PCIEXP_WAKE_DISABLE  */ {ACPI_REGISTER_PM1_ENABLE,
						ACPI_BITPOSITION_PCIEXP_WAKE_DISABLE,
						ACPI_BITMASK_PCIEXP_WAKE_DISABLE},

	/* ACPI_BITREG_SCI_ENABLE           */ {ACPI_REGISTER_PM1_CONTROL,
						ACPI_BITPOSITION_SCI_ENABLE,
						ACPI_BITMASK_SCI_ENABLE},
	/* ACPI_BITREG_BUS_MASTER_RLD       */ {ACPI_REGISTER_PM1_CONTROL,
						ACPI_BITPOSITION_BUS_MASTER_RLD,
						ACPI_BITMASK_BUS_MASTER_RLD},
	/* ACPI_BITREG_GLOBAL_LOCK_RELEASE  */ {ACPI_REGISTER_PM1_CONTROL,
						ACPI_BITPOSITION_GLOBAL_LOCK_RELEASE,
						ACPI_BITMASK_GLOBAL_LOCK_RELEASE},
	/* ACPI_BITREG_SLEEP_TYPE           */ {ACPI_REGISTER_PM1_CONTROL,
						ACPI_BITPOSITION_SLEEP_TYPE,
						ACPI_BITMASK_SLEEP_TYPE},
	/* ACPI_BITREG_SLEEP_ENABLE         */ {ACPI_REGISTER_PM1_CONTROL,
						ACPI_BITPOSITION_SLEEP_ENABLE,
						ACPI_BITMASK_SLEEP_ENABLE},

	/* ACPI_BITREG_ARB_DIS              */ {ACPI_REGISTER_PM2_CONTROL,
						ACPI_BITPOSITION_ARB_DISABLE,
						ACPI_BITMASK_ARB_DISABLE}
};

struct acpi_fixed_event_info acpi_gbl_fixed_event_info[ACPI_NUM_FIXED_EVENTS] = {
	/* ACPI_EVENT_PMTIMER       */ {ACPI_BITREG_TIMER_STATUS,
					ACPI_BITREG_TIMER_ENABLE,
					ACPI_BITMASK_TIMER_STATUS,
					ACPI_BITMASK_TIMER_ENABLE},	/* ACPI PM Timer 相关事件 */
	/* ACPI_EVENT_GLOBAL        */ {ACPI_BITREG_GLOBAL_LOCK_STATUS,
					ACPI_BITREG_GLOBAL_LOCK_ENABLE,
					ACPI_BITMASK_GLOBAL_LOCK_STATUS,
					ACPI_BITMASK_GLOBAL_LOCK_ENABLE},/* Global Lock 事件，用于协调 OS 与 BIOS/firmware 对某些共享资源（如嵌入式控制器）的访问 */
	/* ACPI_EVENT_POWER_BUTTON  */ {ACPI_BITREG_POWER_BUTTON_STATUS,
					ACPI_BITREG_POWER_BUTTON_ENABLE,
					ACPI_BITMASK_POWER_BUTTON_STATUS,
					ACPI_BITMASK_POWER_BUTTON_ENABLE},/* Power Button（电源键）事件,用户按下电源按钮，会设置 PM1_STS 中的电源键状态位，然后通过 SCI 通知 OS。 */
	/* ACPI_EVENT_SLEEP_BUTTON  */ {ACPI_BITREG_SLEEP_BUTTON_STATUS,
					ACPI_BITREG_SLEEP_BUTTON_ENABLE,
					ACPI_BITMASK_SLEEP_BUTTON_STATUS,
					ACPI_BITMASK_SLEEP_BUTTON_ENABLE},/* Sleep Button（睡眠键）事件. 睡眠键按下事件，机制与电源键类似 */
	/* ACPI_EVENT_RTC           */ {ACPI_BITREG_RT_CLOCK_STATUS,
					ACPI_BITREG_RT_CLOCK_ENABLE,
					ACPI_BITMASK_RT_CLOCK_STATUS,
					ACPI_BITMASK_RT_CLOCK_ENABLE},/* RTC（实时时钟）事件,实时时钟闹钟事件，通常用于定时唤醒 */
};
#endif				/* !ACPI_REDUCED_HARDWARE */

#if defined (ACPI_DISASSEMBLER) || defined (ACPI_ASL_COMPILER)

/* to_pld macro: compile/disassemble strings */

const char *acpi_gbl_pld_panel_list[] = {
	"TOP",
	"BOTTOM",
	"LEFT",
	"RIGHT",
	"FRONT",
	"BACK",
	"UNKNOWN",
	NULL
};

const char *acpi_gbl_pld_vertical_position_list[] = {
	"UPPER",
	"CENTER",
	"LOWER",
	NULL
};

const char *acpi_gbl_pld_horizontal_position_list[] = {
	"LEFT",
	"CENTER",
	"RIGHT",
	NULL
};

const char *acpi_gbl_pld_shape_list[] = {
	"ROUND",
	"OVAL",
	"SQUARE",
	"VERTICALRECTANGLE",
	"HORIZONTALRECTANGLE",
	"VERTICALTRAPEZOID",
	"HORIZONTALTRAPEZOID",
	"UNKNOWN",
	"CHAMFERED",
	NULL
};
#endif

/* Public globals */

ACPI_EXPORT_SYMBOL(acpi_gbl_FADT)
ACPI_EXPORT_SYMBOL(acpi_dbg_level)
ACPI_EXPORT_SYMBOL(acpi_dbg_layer)
ACPI_EXPORT_SYMBOL(acpi_gpe_count)
ACPI_EXPORT_SYMBOL(acpi_current_gpe_count)
