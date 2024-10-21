/* SPDX-License-Identifier: GPL-2.0 */

	.align 4				/*将当前地址对齐到4字节边界*/
	.section .payload, "ax", %progbits	/*定义名为.payload的节，设置为可分配和可执行*/
	.globl _start				/*声明_start为全局符号，供其他文件引用*/
_start:						/*定义_start标签，作为程序的入口点*/
	.incbin "arch/riscv/boot/Image"		/*将指定的二进制文件内容嵌入到输出中*/

