/* SPDX-License-Identifier: GPL-2.0 */
/* riscv架构链接脚本 */

#include <asm/page.h>
#include <asm/pgtable.h>

OUTPUT_ARCH(riscv)		/*指定输出架构为RISC-V*/
ENTRY(_start)			/*定义程序入口点为_start*/

SECTIONS			/*开始定义段*/
{
	. = KERNEL_LINK_ADDR;	/*设置当前地址为内核链接地址*/

	.payload : {		/*定义名为.payload的段*/
		*(.payload)	/*包含所有名为.payload的输入节*/
		. = ALIGN(8);	/*当前地址对齐到8字节边界*/
	}
}
