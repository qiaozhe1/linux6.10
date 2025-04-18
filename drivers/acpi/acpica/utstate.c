// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: utstate - state object support procedures
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utstate")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_push_generic_state
 *
 * PARAMETERS:  list_head           - Head of the state stack
 *              state               - State object to push
 *
 * RETURN:      None
 *
 * DESCRIPTION: Push a state object onto a state stack
 *
 ******************************************************************************/
void
acpi_ut_push_generic_state(union acpi_generic_state **list_head,
			   union acpi_generic_state *state)
{
	ACPI_FUNCTION_ENTRY();

	/* Push the state object onto the front of the list (stack) */

	state->common.next = *list_head;
	*list_head = state;
	return;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_pop_generic_state
 *
 * PARAMETERS:  list_head           - Head of the state stack
 *
 * RETURN:      The popped state object
 *
 * DESCRIPTION: Pop a state object from a state stack
 *
 ******************************************************************************/

union acpi_generic_state *acpi_ut_pop_generic_state(union acpi_generic_state
						    **list_head)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_ENTRY();

	/* Remove the state object at the head of the list (stack) */

	state = *list_head;
	if (state) {

		/* Update the list head */

		*list_head = state->common.next;
	}

	return (state);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_generic_state
 *
 * PARAMETERS:  None
 *
 * RETURN:      The new state object. NULL on failure.
 *
 * DESCRIPTION: Create a generic state object. Attempt to obtain one from
 *              the global state cache;  If none available, create a new one.
 *
 ******************************************************************************/

union acpi_generic_state *acpi_ut_create_generic_state(void)//创建并初始化ACPI通用状态对象。
{
	union acpi_generic_state *state;// 通用状态对象指针

	ACPI_FUNCTION_ENTRY();

	state = acpi_os_acquire_object(acpi_gbl_state_cache);// 从缓存中获取或分配新对象
	if (state) {//成功获取到对象

		/* Initialize */
		state->common.descriptor_type = ACPI_DESC_TYPE_STATE;//设置为通用状态类型
	}

	return (state);//返回对象指针（成功时为非NULL，失败时为NULL）
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_thread_state
 *
 * PARAMETERS:  None
 *
 * RETURN:      New Thread State. NULL on failure
 *
 * DESCRIPTION: Create a "Thread State" - a flavor of the generic state used
 *              to track per-thread info during method execution
 *
 ******************************************************************************/

struct acpi_thread_state *acpi_ut_create_thread_state(void)//创建ACPI线程状态对象，用于跟踪线程在执行AML方法时的同步状态和资源持有情况。
{
	union acpi_generic_state *state;//通用状态对象指针（用于存储线程状态）

	ACPI_FUNCTION_ENTRY();

	/* Create the generic state object */
	/* 创建通用状态对象 */
	state = acpi_ut_create_generic_state();//分配并初始化通用状态对象内存
	if (!state) {//内存分配失败,返回NULL表示创建失败
		return (NULL);
	}

	/* Init fields specific to the update struct */
	/* 初始化线程状态专用字段 */
	state->common.descriptor_type = ACPI_DESC_TYPE_STATE_THREAD;//设置对象类型为线程状态
	state->thread.thread_id = acpi_os_get_thread_id();// 获取当前线程的唯一ID（由操作系统提供）

	/* Check for invalid thread ID - zero is very bad, it will break things */

	if (!state->thread.thread_id) {//如果线程ID为0（无效）
		ACPI_ERROR((AE_INFO, "Invalid zero ID from AcpiOsGetThreadId"));
		state->thread.thread_id = (acpi_thread_id) 1;// 强制设置为有效值1（防止后续操作失败）
	}

	return ((struct acpi_thread_state *)state);// 返回转换后的线程状态对象指针
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_update_state
 *
 * PARAMETERS:  object          - Initial Object to be installed in the state
 *              action          - Update action to be performed
 *
 * RETURN:      New state object, null on failure
 *
 * DESCRIPTION: Create an "Update State" - a flavor of the generic state used
 *              to update reference counts and delete complex objects such
 *              as packages.
 *
 ******************************************************************************/

union acpi_generic_state *acpi_ut_create_update_state(union acpi_operand_object
						      *object, u16 action)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_ENTRY();

	/* Create the generic state object */

	state = acpi_ut_create_generic_state();
	if (!state) {
		return (NULL);
	}

	/* Init fields specific to the update struct */

	state->common.descriptor_type = ACPI_DESC_TYPE_STATE_UPDATE;
	state->update.object = object;
	state->update.value = action;
	return (state);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_pkg_state
 *
 * PARAMETERS:  object          - Initial Object to be installed in the state
 *              action          - Update action to be performed
 *
 * RETURN:      New state object, null on failure
 *
 * DESCRIPTION: Create a "Package State"
 *
 ******************************************************************************/
/*
 * acpi_ut_create_pkg_state - 创建包遍历状态对象
 * 
 * 该函数为包遍历操作创建并初始化一个状态控制块，用于跟踪包遍历过程中的上下文信息
 *
 * @internal_object: 要遍历的ACPI内部包对象(源对象)
 * @external_object: 关联的外部对象(目标对象)
 * @index: 当前包中的起始元素索引
 *
 * 返回值:
 *   成功 - 指向新创建的状态对象的指针
 *   失败 - NULL(内存不足)
 */
union acpi_generic_state *acpi_ut_create_pkg_state(void *internal_object,
						   void *external_object,
						   u32 index)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_ENTRY();

	/* Create the generic state object */

	state = acpi_ut_create_generic_state();//创建基础状态对象,从ACPI状态缓存分配
	if (!state) {
		return (NULL);
	}

	/* Init fields specific to the update struct */

	state->common.descriptor_type = ACPI_DESC_TYPE_STATE_PACKAGE;//标记为包状态
	state->pkg.source_object = (union acpi_operand_object *)internal_object;//设置源包
	state->pkg.dest_object = external_object;//设置目标位置
	state->pkg.index = index;// 初始化当前索引
	state->pkg.num_packages = 1;// 初始化包计数器

	return (state);// 返回初始化完成的状态对象
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_control_state
 *
 * PARAMETERS:  None
 *
 * RETURN:      New state object, null on failure
 *
 * DESCRIPTION: Create a "Control State" - a flavor of the generic state used
 *              to support nested IF/WHILE constructs in the AML.
 *
 ******************************************************************************/

union acpi_generic_state *acpi_ut_create_control_state(void)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_ENTRY();

	/* Create the generic state object */

	state = acpi_ut_create_generic_state();
	if (!state) {
		return (NULL);
	}

	/* Init fields specific to the control struct */

	state->common.descriptor_type = ACPI_DESC_TYPE_STATE_CONTROL;
	state->common.state = ACPI_CONTROL_CONDITIONAL_EXECUTING;

	return (state);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_delete_generic_state
 *
 * PARAMETERS:  state               - The state object to be deleted
 *
 * RETURN:      None
 *
 * DESCRIPTION: Release a state object to the state cache. NULL state objects
 *              are ignored.
 *
 ******************************************************************************/

void acpi_ut_delete_generic_state(union acpi_generic_state *state)
{
	ACPI_FUNCTION_ENTRY();

	/* Ignore null state */

	if (state) {
		(void)acpi_os_release_object(acpi_gbl_state_cache, state);
	}

	return;
}
