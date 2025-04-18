// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*******************************************************************************
 *
 * Module Name: utmisc - common utility procedures
 *
 ******************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utmisc")

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_is_pci_root_bridge
 *
 * PARAMETERS:  id              - The HID/CID in string format
 *
 * RETURN:      TRUE if the Id is a match for a PCI/PCI-Express Root Bridge
 *
 * DESCRIPTION: Determine if the input ID is a PCI Root Bridge ID.
 *
 ******************************************************************************/
u8 acpi_ut_is_pci_root_bridge(char *id)
{

	/*
	 * Check if this is a PCI root bridge.
	 * ACPI 3.0+: check for a PCI Express root also.
	 */
	if (!(strcmp(id,
		     PCI_ROOT_HID_STRING)) ||
	    !(strcmp(id, PCI_EXPRESS_ROOT_HID_STRING))) {
		return (TRUE);
	}

	return (FALSE);
}

#if (defined ACPI_ASL_COMPILER || defined ACPI_EXEC_APP || defined ACPI_NAMES_APP)
/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_is_aml_table
 *
 * PARAMETERS:  table               - An ACPI table
 *
 * RETURN:      TRUE if table contains executable AML; FALSE otherwise
 *
 * DESCRIPTION: Check ACPI Signature for a table that contains AML code.
 *              Currently, these are DSDT,SSDT,PSDT. All other table types are
 *              data tables that do not contain AML code.
 *
 ******************************************************************************/

u8 acpi_ut_is_aml_table(struct acpi_table_header *table)
{

	/* These are the only tables that contain executable AML */

	if (ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_DSDT) ||
	    ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_PSDT) ||
	    ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_SSDT) ||
	    ACPI_COMPARE_NAMESEG(table->signature, ACPI_SIG_OSDT) ||
	    ACPI_IS_OEM_SIG(table->signature)) {
		return (TRUE);
	}

	return (FALSE);
}
#endif

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_dword_byte_swap
 *
 * PARAMETERS:  value           - Value to be converted
 *
 * RETURN:      u32 integer with bytes swapped
 *
 * DESCRIPTION: Convert a 32-bit value to big-endian (swap the bytes)
 *
 ******************************************************************************/

u32 acpi_ut_dword_byte_swap(u32 value)
{
	union {
		u32 value;
		u8 bytes[4];
	} out;
	union {
		u32 value;
		u8 bytes[4];
	} in;

	ACPI_FUNCTION_ENTRY();

	in.value = value;

	out.bytes[0] = in.bytes[3];
	out.bytes[1] = in.bytes[2];
	out.bytes[2] = in.bytes[1];
	out.bytes[3] = in.bytes[0];

	return (out.value);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_set_integer_width
 *
 * PARAMETERS:  Revision            From DSDT header
 *
 * RETURN:      None
 *
 * DESCRIPTION: Set the global integer bit width based upon the revision
 *              of the DSDT. For Revision 1 and 0, Integers are 32 bits.
 *              For Revision 2 and above, Integers are 64 bits. Yes, this
 *              makes a difference.
 *
 ******************************************************************************/

void acpi_ut_set_integer_width(u8 revision)
{

	if (revision < 2) {

		/* 32-bit case */

		acpi_gbl_integer_bit_width = 32;
		acpi_gbl_integer_nybble_width = 8;
		acpi_gbl_integer_byte_width = 4;
	} else {
		/* 64-bit case (ACPI 2.0+) */

		acpi_gbl_integer_bit_width = 64;
		acpi_gbl_integer_nybble_width = 16;
		acpi_gbl_integer_byte_width = 8;
	}
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_create_update_state_and_push
 *
 * PARAMETERS:  object          - Object to be added to the new state
 *              action          - Increment/Decrement
 *              state_list      - List the state will be added to
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Create a new state and push it
 *
 ******************************************************************************/

acpi_status
acpi_ut_create_update_state_and_push(union acpi_operand_object *object,
				     u16 action,
				     union acpi_generic_state **state_list)
{
	union acpi_generic_state *state;

	ACPI_FUNCTION_ENTRY();

	/* Ignore null objects; these are expected */

	if (!object) {
		return (AE_OK);
	}

	state = acpi_ut_create_update_state(object, action);
	if (!state) {
		return (AE_NO_MEMORY);
	}

	acpi_ut_push_generic_state(state_list, state);
	return (AE_OK);
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_walk_package_tree
 *
 * PARAMETERS:  source_object       - The package to walk
 *              target_object       - Target object (if package is being copied)
 *              walk_callback       - Called once for each package element
 *              context             - Passed to the callback function
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Walk through a package, including subpackages
 *
 ******************************************************************************/
/*
 * acpi_ut_walk_package_tree - 递归遍历ACPI包对象树
 * 
 * 该函数使用状态机机制深度优先遍历包及其嵌套子包，对每个元素调用回调函数
 * 
 * @source_object: 要遍历的根包对象
 * @target_object: 目标对象(用于复制操作)
 * @walk_callback: 每个元素调用的回调函数
 * @context: 传递给回调的上下文
 * 
 * 返回值: 
 *   AE_OK - 成功
 *   AE_NO_MEMORY - 内存不足
 *   其他 - 回调函数返回的错误状态
 */
acpi_status
acpi_ut_walk_package_tree(union acpi_operand_object *source_object,
			  void *target_object,
			  acpi_pkg_callback walk_callback, void *context)
{
	acpi_status status = AE_OK;
	union acpi_generic_state *state_list = NULL;//状态堆栈
	union acpi_generic_state *state;//当前状态
	union acpi_operand_object *this_source_obj;//当前元素对象
	u32 this_index;//当前元素索引

	ACPI_FUNCTION_TRACE(ut_walk_package_tree);

	state = acpi_ut_create_pkg_state(source_object, target_object, 0);//创建初始状态(从根包开始)
	if (!state) {
		return_ACPI_STATUS(AE_NO_MEMORY);
	}

	while (state) {//主状态机循环

		/* Get one element of the package */
		/* 获取当前包中的下一个元素 */
		this_index = state->pkg.index;
		this_source_obj =
		    state->pkg.source_object->package.elements[this_index];
		state->pkg.this_target_obj =
		    &state->pkg.source_object->package.elements[this_index];

                /*
                 * 处理三种情况：
                 * 1. 未初始化的包元素(合法情况)
                 * 2. 非操作对象(非ACPI_DESC_TYPE_OPERAND)
                 * 3. 非包类型的普通元素
                 */
		if ((!this_source_obj) ||//空元素
		    (ACPI_GET_DESCRIPTOR_TYPE(this_source_obj) !=
		     ACPI_DESC_TYPE_OPERAND) ||//非操作对象
		    (this_source_obj->common.type != ACPI_TYPE_PACKAGE)) {//非包类型
			status =
			    walk_callback(ACPI_COPY_TYPE_SIMPLE,
					  this_source_obj, state, context);//调用回调函数处理简单类型元素
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}

			state->pkg.index++;//移动到下一个元素
			/*
    			 * 处理当前包遍历完成的情况(索引超过元素数量)
    			 * 需要循环处理因为可能有多个嵌套层级同时完成
    			 */
			while (state->pkg.index >=
			       state->pkg.source_object->package.count) {
        			/*
        			 * 当前层级的包已遍历完成：
        			 * 1. 释放已完成包的状态对象
        			 * 2. 弹出上一级状态继续处理
        			 */
				acpi_ut_delete_generic_state(state);//释放当前状态内存
				state = acpi_ut_pop_generic_state(&state_list);//弹出父包状态

				/* Finished when there are no more states */

				if (!state) {// 检查状态堆栈是否已空
            				/* 
            				 * 所有层级的包都已处理完成：
            				 * - 已回到最外层的根包
            				 * - 整个遍历过程正常结束
            				 */
					return_ACPI_STATUS(AE_OK);//已完成根包遍历,返回
				}

        			/* 
        			 * 返回到父包层级后：
        			 * 需要移动到父包的下一个元素
       				 * (因为刚完成的是父包中的一个子包元素)
        			 */
				state->pkg.index++;//返回上一级后移动到下一个元素
			}
		} else {
			/* This is a subobject of type package */
			/* 当前元素是嵌套包对象(ACPI_TYPE_PACKAGE)
			 * 先调用回调函数处理这个包元素
			 */
			status =
			    walk_callback(ACPI_COPY_TYPE_PACKAGE,
					  this_source_obj, state, context);//处理嵌套包元素
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}

    			/*
    			 * 准备深入遍历嵌套包：
    			 * 1. 将当前状态压入堆栈(保存父包遍历进度)
    			 * 2. 为子包创建新的遍历状态
    			 */
			acpi_ut_push_generic_state(&state_list, state);//当前状态压栈
			state =
			    acpi_ut_create_pkg_state(this_source_obj,
						     state->pkg.this_target_obj,
						     0);//创建子包的遍历状态,从子包的第0个元素开始
			if (!state) {//内存分配失败时的处理

				/* Free any stacked Update State objects */

				while (state_list) {//循环释放堆栈中所有已保存的状态
					state =
					    acpi_ut_pop_generic_state
					    (&state_list);//弹出状态
					acpi_ut_delete_generic_state(state);//释放内存
				}
				return_ACPI_STATUS(AE_NO_MEMORY);
			}
		}
	}

	/* We should never get here */

	ACPI_ERROR((AE_INFO, "State list did not terminate correctly"));

	return_ACPI_STATUS(AE_AML_INTERNAL);
}

#ifdef ACPI_DEBUG_OUTPUT
/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_display_init_pathname
 *
 * PARAMETERS:  type                - Object type of the node
 *              obj_handle          - Handle whose pathname will be displayed
 *              path                - Additional path string to be appended.
 *                                      (NULL if no extra path)
 *
 * RETURN:      acpi_status
 *
 * DESCRIPTION: Display full pathname of an object, DEBUG ONLY
 *
 ******************************************************************************/

void
acpi_ut_display_init_pathname(u8 type,
			      struct acpi_namespace_node *obj_handle,
			      const char *path)
{
	acpi_status status;
	struct acpi_buffer buffer;

	ACPI_FUNCTION_ENTRY();

	/* Only print the path if the appropriate debug level is enabled */

	if (!(acpi_dbg_level & ACPI_LV_INIT_NAMES)) {
		return;
	}

	/* Get the full pathname to the node */

	buffer.length = ACPI_ALLOCATE_LOCAL_BUFFER;
	status = acpi_ns_handle_to_pathname(obj_handle, &buffer, TRUE);
	if (ACPI_FAILURE(status)) {
		return;
	}

	/* Print what we're doing */

	switch (type) {
	case ACPI_TYPE_METHOD:

		acpi_os_printf("Executing  ");
		break;

	default:

		acpi_os_printf("Initializing ");
		break;
	}

	/* Print the object type and pathname */

	acpi_os_printf("%-12s %s",
		       acpi_ut_get_type_name(type), (char *)buffer.pointer);

	/* Extra path is used to append names like _STA, _INI, etc. */

	if (path) {
		acpi_os_printf(".%s", path);
	}
	acpi_os_printf("\n");

	ACPI_FREE(buffer.pointer);
}
#endif
