/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/******************************************************************************
 *
 * Name: acnames.h - Global names and strings
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#ifndef __ACNAMES_H__
#define __ACNAMES_H__

/* Method names - 这些方法可以出现在名称空间中的任何位置 */
/* ACPI 预定义方法名称常量定义 */
/* 注：所有ACPI方法名均为4字符（包含结尾的下划线填充）*/
#define METHOD_NAME__ADR        "_ADR"	//设备地址 - 用于获取设备的物理地址（Address）
#define METHOD_NAME__AEI        "_AEI"	//ACPI事件中断 - 声明设备使用的事件中断引脚（ACPI Event Interrupt）
#define METHOD_NAME__BBN        "_BBN"	//总线编号 - 指定PCI总线的编号（Bus Number）
#define METHOD_NAME__CBA        "_CBA"	// 配置基地址 - PCI配置空间的基地址（Configuration Base Address）
#define METHOD_NAME__CID        "_CID"	//兼容ID - 提供设备的兼容硬件ID列表（Compatible ID）
#define METHOD_NAME__CLS        "_CLS"	//设备类别 - 定义设备类别码（Class Code）
#define METHOD_NAME__CRS        "_CRS"	//当前资源设置 - 描述设备当前使用的硬件资源（Current Resource Settings）
#define METHOD_NAME__DDN        "_DDN"	//DOS设备名 - 旧式DOS设备名称（DOS Device Name）
#define METHOD_NAME__DIS        "_DIS"	//禁用设备 - 用于禁用设备（Disable）
#define METHOD_NAME__DMA        "_DMA"	//DMA资源 - 描述设备的DMA资源需求（DMA）
#define METHOD_NAME__EVT        "_EVT"	//事件 - 通用事件通知方法（Event）
#define METHOD_NAME__HID        "_HID"	//硬件ID - 设备的标准ACPI硬件标识符（Hardware ID）
#define METHOD_NAME__INI        "_INI"	//初始化 - 设备/对象的初始化方法（Initialize）
#define METHOD_NAME__PLD        "_PLD"	//物理设备位置 - 描述设备的物理位置信息（Physical Location of Device）
#define METHOD_NAME__DSD        "_DSD"	//设备特定数据 - 补充设备描述信息（Device Specific Data）
#define METHOD_NAME__PRS        "_PRS"	//可能资源设置 - 设备可能使用的资源（Possible Resource Settings）
#define METHOD_NAME__PRT        "_PRT"	//PCI路由表 - PCI中断路由信息（PCI Routing Table）
#define METHOD_NAME__PRW        "_PRW"	//电源资源唤醒 - 描述唤醒系统所需的电源资源（Power Resources for Wake）
#define METHOD_NAME__PS0        "_PS0"	//电源状态0 - 设备电源状态相关（Power State 0）
#define METHOD_NAME__PS1        "_PS1"	//电源状态1 - 设备电源状态相关（Power State 1）
#define METHOD_NAME__PS2        "_PS2"	//电源状态2 - 设备电源状态相关（Power State 2）
#define METHOD_NAME__PS3        "_PS3"	//电源状态3 - 设备电源状态相关（Power State 3）
#define METHOD_NAME__REG        "_REG"	//区域设置 - 操作区域处理程序连接/断开通知（Region Setup）
#define METHOD_NAME__SB_        "_SB_"	//系统总线 - 表示ACPI命名空间中的系统总线（System Bus）
#define METHOD_NAME__SEG        "_SEG"	//PCI段组 - PCI域/段组编号（Segment Group）
#define METHOD_NAME__SRS        "_SRS"	//设置资源设置 - 配置设备硬件资源（Set Resource Settings）
#define METHOD_NAME__STA        "_STA"	//状态 - 返回设备/对象的当前状态（Status）
#define METHOD_NAME__SUB        "_SUB"	//子设备ID - 设备的子ID信息（Subsystem ID）
#define METHOD_NAME__UID        "_UID"	//唯一ID - 设备的唯一标识符（Unique ID）

/* Method names - these methods must appear at the namespace root */

#define METHOD_PATHNAME__PTS    "\\_PTS"
#define METHOD_PATHNAME__SST    "\\_SI._SST"
#define METHOD_PATHNAME__WAK    "\\_WAK"

/* Definitions of the predefined namespace names  */

#define ACPI_UNKNOWN_NAME       (u32) 0x3F3F3F3F	/* Unknown name is "????" */
#define ACPI_PREFIX_MIXED       (u32) 0x69706341	/* "Acpi" */
#define ACPI_PREFIX_LOWER       (u32) 0x69706361	/* "acpi" */

/* Root name stuff */

#define ACPI_ROOT_NAME          (u32) 0x5F5F5F5C	/* Root name is    "\___" */
#define ACPI_ROOT_PATHNAME      "\\___"
#define ACPI_NAMESPACE_ROOT     "Namespace Root"
#define ACPI_NS_ROOT_PATH       "\\"

#endif				/* __ACNAMES_H__  */
