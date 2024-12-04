/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PID_TYPES_H
#define _LINUX_PID_TYPES_H

enum pid_type {
	PIDTYPE_PID,//表示进程的 PID 类型，用于唯一标识一个进程
	PIDTYPE_TGID,//表示线程组 ID，用于标识属于同一线程组的进程
	PIDTYPE_PGID,//表示进程组 ID，用于标识属于同一进程组的进程
	PIDTYPE_SID,//表示会话 ID，用于标识属于同一会话的进程
	PIDTYPE_MAX,//用于定义 PID 类型的最大值，用于边界检查
};

struct pid_namespace;
extern struct pid_namespace init_pid_ns;

#endif /* _LINUX_PID_TYPES_H */
