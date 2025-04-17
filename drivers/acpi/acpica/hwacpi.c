// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: hwacpi - ACPI Hardware Initialization/Mode Interface
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"

#define _COMPONENT          ACPI_HARDWARE
ACPI_MODULE_NAME("hwacpi")

#if (!ACPI_REDUCED_HARDWARE)	/* Entire module */
/******************************************************************************
 *
 * FUNCTION:    acpi_hw_set_mode
 *
 * PARAMETERS:  mode            - SYS_MODE_ACPI or SYS_MODE_LEGACY
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Transitions the system into the requested mode.
 *
 ******************************************************************************/
/*
 * acpi_hw_set_mode - 设置系统ACPI/传统模式
 * @mode: 目标模式 (ACPI_SYS_MODE_ACPI 或 ACPI_SYS_MODE_LEGACY)
 *
 * 返回值：
 * AE_OK - 模式切换成功或不需要切换
 * AE_NO_HARDWARE_RESPONSE - 硬件不支持模式切换
 * AE_BAD_PARAMETER - 无效模式参数
 * 其他 - 硬件I/O错误
 *
 * 实现说明：
 * 1. 简化硬件模式直接返回成功
 * 2. 严格检查硬件支持情况
 * 3. 通过SMI命令端口执行实际切换
 */
acpi_status acpi_hw_set_mode(u32 mode)
{

	acpi_status status;

	ACPI_FUNCTION_TRACE(hw_set_mode);

	/* If the Hardware Reduced flag is set, machine is always in acpi mode */
	/* 检查1：处理简化硬件模式 */
	if (acpi_gbl_reduced_hardware) {//检查简化硬件标志
		return_ACPI_STATUS(AE_OK);//简化硬件无需模式切换
	}

	/*
	 * ACPI 2.0 clarified that if SMI_CMD in FADT is zero,
	 * system does not support mode transition.
	 */
	/* 检查2：验证SMI命令端口支持 */
	if (!acpi_gbl_FADT.smi_command) {//检查FADT中的SMI_CMD
		ACPI_ERROR((AE_INFO,
			    "No SMI_CMD in FADT, mode transition failed"));
		return_ACPI_STATUS(AE_NO_HARDWARE_RESPONSE);//硬件不支持
	}

	/*
	 * ACPI 2.0 clarified the meaning of ACPI_ENABLE and ACPI_DISABLE
	 * in FADT: If it is zero, enabling or disabling is not supported.
	 * As old systems may have used zero for mode transition,
	 * we make sure both the numbers are zero to determine these
	 * transitions are not supported.
	 */
	/* 检查3：验证模式切换命令支持 */
	if (!acpi_gbl_FADT.acpi_enable && !acpi_gbl_FADT.acpi_disable) {//检查使能/禁用值
		ACPI_ERROR((AE_INFO,
			    "No ACPI mode transition supported in this system "
			    "(enable/disable both zero)"));
		return_ACPI_STATUS(AE_OK);//静默返回成功(历史兼容)
	}

	switch (mode) {//模式切换操作
	case ACPI_SYS_MODE_ACPI://ACPI模式切换

		/* BIOS should have disabled ALL fixed and GP events */

		status = acpi_hw_write_port(acpi_gbl_FADT.smi_command,
					    (u32) acpi_gbl_FADT.acpi_enable, 8);//向SMI端口写入ACPI使能命令
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "Attempting to enable ACPI mode\n"));
		break;

	case ACPI_SYS_MODE_LEGACY://传统模式切换
		/*
		 * BIOS should clear all fixed status bits and restore fixed event
		 * enable bits to default
		 */
		status = acpi_hw_write_port(acpi_gbl_FADT.smi_command,
					    (u32)acpi_gbl_FADT.acpi_disable, 8);//向SMI端口写入ACPI禁用命令
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
				  "Attempting to enable Legacy (non-ACPI) mode\n"));
		break;

	default://无效模式处理

		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	if (ACPI_FAILURE(status)) {//检查I/O操作结果
		ACPI_EXCEPTION((AE_INFO, status,
				"Could not write ACPI mode change"));
		return_ACPI_STATUS(status);
	}

	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_hw_get_mode
 *
 * PARAMETERS:  none
 *
 * RETURN:      SYS_MODE_ACPI or SYS_MODE_LEGACY
 *
 * DESCRIPTION: Return current operating state of system. Determined by
 *              querying the SCI_EN bit.
 *
 ******************************************************************************/
/*
 * acpi_hw_get_mode - 获取当前系统ACPI模式状态
 * 
 * 返回值：
 * ACPI_SYS_MODE_ACPI   - 系统处于ACPI模式
 * ACPI_SYS_MODE_LEGACY - 系统处于传统(非ACPI)模式
 *
 * 实现逻辑：
 * 1. 简化硬件模式直接返回ACPI模式
 * 2. 无SMI命令端口时默认返回ACPI模式
 * 3. 通过读取SCI_ENABLE位寄存器确定实际模式
 */
u32 acpi_hw_get_mode(void)
{
	acpi_status status;
	u32 value;

	ACPI_FUNCTION_TRACE(hw_get_mode);

	/* If the Hardware Reduced flag is set, machine is always in acpi mode */

	if (acpi_gbl_reduced_hardware) {//检查全局简化硬件标志
		return_UINT32(ACPI_SYS_MODE_ACPI);//简化硬件总是ACPI模式,直接返回
	}

	/*
	 * 验证硬件支持模式切换
	 * ACPI 2.0规定：FADT中SMI_CMD为零表示不支持模式切换
	 */
	if (!acpi_gbl_FADT.smi_command) {//检查SMI命令端口
		return_UINT32(ACPI_SYS_MODE_ACPI);//无SMI端口默认ACPI模式
	}

	status = acpi_read_bit_register(ACPI_BITREG_SCI_ENABLE, &value);//读取SCI使能位寄存器
	if (ACPI_FAILURE(status)) {
		return_UINT32(ACPI_SYS_MODE_LEGACY);//读取失败处理,默认传统模式
	}

	if (value) {//检查寄存器值
		return_UINT32(ACPI_SYS_MODE_ACPI);//位为1表示ACPI模式
	} else {
		return_UINT32(ACPI_SYS_MODE_LEGACY);//位为0表示传统模式
	}
}

#endif				/* !ACPI_REDUCED_HARDWARE */
