// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/types.h>
#include <linux/extable.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/binfmts.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/acpi.h>
#include <linux/bootconfig.h>
#include <linux/console.h>
#include <linux/nmi.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/kprobes.h>
#include <linux/kmsan.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/kfence.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/buildid.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/sched/isolation.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/utsname.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/padata.h>
#include <linux/pid_namespace.h>
#include <linux/device/driver.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/init.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/pti.h>
#include <linux/blkdev.h>
#include <linux/sched/clock.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/moduleloader.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/cache.h>
#include <linux/rodata_test.h>
#include <linux/jump_label.h>
#include <linux/kcsan.h>
#include <linux/init_syscalls.h>
#include <linux/stackdepot.h>
#include <linux/randomize_kstack.h>
#include <linux/pidfs.h>
#include <linux/ptdump.h>
#include <net/net_namespace.h>

#include <asm/io.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/initcall.h>

#include <kunit/test.h>

static int kernel_init(void *);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line __ro_after_init;
unsigned int saved_command_line_len __ro_after_init;
/* Command line for parameter parsing */
static char *static_command_line;
/* Untouched extra command line */
static char *extra_command_line;
/* Extra init arguments */
static char *extra_init_args;

#ifdef CONFIG_BOOT_CONFIG
/* Is bootconfig on command line? */
static bool bootconfig_found;
static size_t initargs_offs;
#else
# define bootconfig_found false
# define initargs_offs 0
#endif

static char *execute_command;
static char *ramdisk_execute_command = "/init";

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

#ifdef CONFIG_BLK_DEV_INITRD
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	u32 size, csum;//size 保存bootconfig数据的大小，`csum`保存校验和
	char *data;//指向可能包含 bootconfig 的数据区域
	u32 *hdr;//指向 bootconfig 的头部，包含大小和校验和
	int i;

	if (!initrd_end)//如果initrd_end未定义，说明没有initrd，直接返回 NULL
		return NULL;

	data = (char *)initrd_end - BOOTCONFIG_MAGIC_LEN;//data指向initrd的尾部，尝试找到BOOTCONFIG_MAGIC 的位置
	/*
	 * Since Grub may align the size of initrd to 4, we must
	 * check the preceding 3 bytes as well.
	 * 由于 Grub 可能将 initrd 的大小对齐到 4 字节，我们需要检查前面的 3 个字节
	 * 以确保不遗漏 bootconfig 的魔术标记。
	 */
	for (i = 0; i < 4; i++) {
		if (!memcmp(data, BOOTCONFIG_MAGIC, BOOTCONFIG_MAGIC_LEN))//比较当前 data 位置的内容是否是 BOOTCONFIG_MAGIC
			goto found;//找到 bootconfig 的魔术标记，跳转到 `found` 标签
		data--;//如果没有找到，指针前移一个字节，继续检查.
	}
	return NULL;//未找到bootconfig魔术标记，返回NULL

found:
	//找到bootconfig头部，数据的前8字节包含大小和校验和
	hdr = (u32 *)(data - 8);
	size = le32_to_cpu(hdr[0]);//读取并转换bootconfig数据的大小
	csum = le32_to_cpu(hdr[1]);//读取并转换bootconfig数据的校验和

	data = ((void *)hdr) - size;//将 data 指向 bootconfig 数据的开始位置
	if ((unsigned long)data < initrd_start) {//检查 bootconfig 数据是否在 initrd 的有效范围内
		pr_err("bootconfig size %d is greater than initrd size %ld\n",
			size, initrd_end - initrd_start);
		return NULL;
	}

	if (xbc_calc_checksum(data, size) != csum) {//验证 bootconfig 数据的校验和是否匹配
		pr_err("bootconfig checksum failed\n");
		return NULL;
	}

	/* Remove bootconfig from initramfs/initrd */
	initrd_end = (unsigned long)data;//从 initramfs/initrd 中移除 bootconfig 数据
	if (_size)// 如果传入了 _size 参数，则将 bootconfig 的大小返回给调用者
		*_size = size;

	return data;//返回指向 bootconfig 数据的指针
}
#else
static void * __init get_boot_config_from_initrd(size_t *_size)
{
	return NULL;
}
#endif

#ifdef CONFIG_BOOT_CONFIG

static char xbc_namebuf[XBC_KEYLEN_MAX] __initdata;

#define rest(dst, end) ((end) > (dst) ? (end) - (dst) : 0)

static int __init xbc_snprint_cmdline(char *buf, size_t size,
				      struct xbc_node *root)
{
	struct xbc_node *knode, *vnode;
	char *end = buf + size;
	const char *val, *q;
	int ret;

	xbc_node_for_each_key_value(root, knode, val) {
		ret = xbc_node_compose_key_after(root, knode,
					xbc_namebuf, XBC_KEYLEN_MAX);
		if (ret < 0)
			return ret;

		vnode = xbc_node_get_child(knode);
		if (!vnode) {
			ret = snprintf(buf, rest(buf, end), "%s ", xbc_namebuf);
			if (ret < 0)
				return ret;
			buf += ret;
			continue;
		}
		xbc_array_for_each_value(vnode, val) {
			/*
			 * For prettier and more readable /proc/cmdline, only
			 * quote the value when necessary, i.e. when it contains
			 * whitespace.
			 */
			q = strpbrk(val, " \t\r\n") ? "\"" : "";
			ret = snprintf(buf, rest(buf, end), "%s=%s%s%s ",
				       xbc_namebuf, q, val, q);
			if (ret < 0)
				return ret;
			buf += ret;
		}
	}

	return buf - (end - size);
}
#undef rest

/* Make an extra command line under given key word */
static char * __init xbc_make_cmdline(const char *key)
{
	struct xbc_node *root;
	char *new_cmdline;
	int ret, len = 0;

	root = xbc_find_node(key);
	if (!root)
		return NULL;

	/* Count required buffer size */
	len = xbc_snprint_cmdline(NULL, 0, root);
	if (len <= 0)
		return NULL;

	new_cmdline = memblock_alloc(len + 1, SMP_CACHE_BYTES);
	if (!new_cmdline) {
		pr_err("Failed to allocate memory for extra kernel cmdline.\n");
		return NULL;
	}

	ret = xbc_snprint_cmdline(new_cmdline, len + 1, root);
	if (ret < 0 || ret > len) {
		pr_err("Failed to print extra kernel cmdline.\n");
		memblock_free(new_cmdline, len + 1);
		return NULL;
	}

	return new_cmdline;
}

static int __init bootconfig_params(char *param, char *val,
				    const char *unused, void *arg)
{
	if (strcmp(param, "bootconfig") == 0) {
		bootconfig_found = true;
	}
	return 0;
}

static int __init warn_bootconfig(char *str)
{
	/* The 'bootconfig' has been handled by bootconfig_params(). */
	return 0;
}

static void __init setup_boot_config(void)
{
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;
	const char *msg, *data;
	int pos, ret;
	size_t size;
	char *err;

	/* Cut out the bootconfig data even if we have no bootconfig option */
	data = get_boot_config_from_initrd(&size);
	/* If there is no bootconfig in initrd, try embedded one. */
	if (!data)
		data = xbc_get_embedded_bootconfig(&size);

	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	err = parse_args("bootconfig", tmp_cmdline, NULL, 0, 0, 0, NULL,
			 bootconfig_params);

	if (IS_ERR(err) || !(bootconfig_found || IS_ENABLED(CONFIG_BOOT_CONFIG_FORCE)))
		return;

	/* parse_args() stops at the next param of '--' and returns an address */
	if (err)
		initargs_offs = err - tmp_cmdline;

	if (!data) {
		/* If user intended to use bootconfig, show an error level message */
		if (bootconfig_found)
			pr_err("'bootconfig' found on command line, but no bootconfig found\n");
		else
			pr_info("No bootconfig data provided, so skipping bootconfig");
		return;
	}

	if (size >= XBC_DATA_MAX) {
		pr_err("bootconfig size %ld greater than max size %d\n",
			(long)size, XBC_DATA_MAX);
		return;
	}

	ret = xbc_init(data, size, &msg, &pos);
	if (ret < 0) {
		if (pos < 0)
			pr_err("Failed to init bootconfig: %s.\n", msg);
		else
			pr_err("Failed to parse bootconfig: %s at %d.\n",
				msg, pos);
	} else {
		xbc_get_info(&ret, NULL);
		pr_info("Load bootconfig: %ld bytes %d nodes\n", (long)size, ret);
		/* keys starting with "kernel." are passed via cmdline */
		extra_command_line = xbc_make_cmdline("kernel");
		/* Also, "init." keys are init arguments */
		extra_init_args = xbc_make_cmdline("init");
	}
	return;
}

static void __init exit_boot_config(void)
{
	xbc_exit();
}

#else	/* !CONFIG_BOOT_CONFIG */
/*
 * 函数的唯一作用是调用 get_boot_config_from_initrd 来从 initrd 中移除 bootconfig 数据。
 * 为了确保 initrd 的数据被正确调整和处理，并防止 bootconfig 数据在后续过程中被错误使用或保留。
 * */
static void __init setup_boot_config(void)
{
	/* Remove bootconfig data from initrd */
	get_boot_config_from_initrd(NULL);
}

static int __init warn_bootconfig(char *str)
{
	pr_warn("WARNING: 'bootconfig' found on the kernel command line but CONFIG_BOOT_CONFIG is not set.\n");
	return 0;
}

#define exit_boot_config()	do {} while (0)

#endif	/* CONFIG_BOOT_CONFIG */

early_param("bootconfig", warn_bootconfig);

bool __init cmdline_has_extra_options(void)
{
	return extra_command_line || extra_init_args;
}

/* Change NUL term back to "=", to make "param" the whole string. */
static void __init repair_env_string(char *param, char *val)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
		} else
			BUG();
	}
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	size_t len = strlen(param);

	/* Handle params aliased to sysctls */
	if (sysctl_is_alias(param))
		return 0;

	repair_env_string(param, val);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strnchr(param, len, '.'))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], len+1))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	size_t len, xlen = 0, ilen = 0;

	if (extra_command_line)
		xlen = strlen(extra_command_line);
	if (extra_init_args) {
		extra_init_args = strim(extra_init_args); /* remove trailing space */
		ilen = strlen(extra_init_args) + 4; /* for " -- " */
	}

	len = xlen + strlen(boot_command_line) + ilen + 1;

	saved_command_line = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!saved_command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	len = xlen + strlen(command_line) + 1;

	static_command_line = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!static_command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	if (xlen) {
		/*
		 * We have to put extra_command_line before boot command
		 * lines because there could be dashes (separator of init
		 * command line) in the command lines.
		 */
		strcpy(saved_command_line, extra_command_line);
		strcpy(static_command_line, extra_command_line);
	}
	strcpy(saved_command_line + xlen, boot_command_line);
	strcpy(static_command_line + xlen, command_line);

	if (ilen) {
		/*
		 * Append supplemental init boot args to saved_command_line
		 * so that user can check what command line options passed
		 * to init.
		 * The order should always be
		 * " -- "[bootconfig init-param][cmdline init-param]
		 */
		if (initargs_offs) {
			len = xlen + initargs_offs;
			strcpy(saved_command_line + len, extra_init_args);
			len += ilen - 4;	/* strlen(extra_init_args) */
			strcpy(saved_command_line + len,
				boot_command_line + initargs_offs - 1);
		} else {
			len = strlen(saved_command_line);
			strcpy(saved_command_line + len, " -- ");
			len += 4;
			strcpy(saved_command_line + len, extra_init_args);
		}
	}

	saved_command_line_len = strlen(saved_command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);
/*在内核启动过程中初始化和启动第一个用户模式进程（init）和内核线程管理进程（kthreadd），并确保系统进入正常的调度状态。*/
static noinline void __ref __noreturn rest_init(void)
{
	struct task_struct *tsk;//定义指向任务结构体的指针tsk
	int pid;

	rcu_scheduler_starting();// 通知 RCU（读取-复制更新）系统调度器即将启动
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 * 首先需要创建 init 进程以便它获得 pid 1，然而 init 进程会想要创建内核线程，
	 * 如果在我们创建 kthreadd 之前调度它，会导致内核 OOPS（错误）
	 */
	pid = user_mode_thread(kernel_init, NULL, CLONE_FS);//创建用户模式线程来运行 kernel_init 函数，并为其分配 pid 1
	/*
	 * Pin init on the boot CPU. Task migration is not properly working
	 * until sched_init_smp() has been run. It will set the allowed
	 * CPUs for init to the non isolated CPUs.
	 * 将 init 进程固定在启动 CPU 上。在 sched_init_smp() 运行之前，任务迁移并不能正常工作。
	 * 该函数将设置 init 进程的允许 CPU 为非隔离的 CPU。
	 */
	rcu_read_lock();//获取 RCU 读取锁，防止在读取任务时发生并发更改
	tsk = find_task_by_pid_ns(pid, &init_pid_ns);//根据进程 ID 在初始命名空间中查找 init 任务
	tsk->flags |= PF_NO_SETAFFINITY;//设置任务标志，禁止设置 CPU 亲和性
	set_cpus_allowed_ptr(tsk, cpumask_of(smp_processor_id()));//将 init 进程限制在启动 CPU 上运行
	rcu_read_unlock();//释放 RCU 读取锁

	numa_default_policy();//设置默认的 NUMA 策略
	pid = kernel_thread(kthreadd, NULL, NULL, CLONE_FS | CLONE_FILES);//创建内核线程 kthreadd，负责管理内核线程
	rcu_read_lock();//获取 RCU 读取锁，防止在读取任务时发生并发更改
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);//根据进程 ID 在初始命名空间中查找 kthreadd 任务
	rcu_read_unlock();//释放 RCU 读取锁

	/*
	 * Enable might_sleep() and smp_processor_id() checks.
	 * They cannot be enabled earlier because with CONFIG_PREEMPTION=y
	 * kernel_thread() would trigger might_sleep() splats. With
	 * CONFIG_PREEMPT_VOLUNTARY=y the init task might have scheduled
	 * already, but it's stuck on the kthreadd_done completion.
	 * 启用 might_sleep() 和 smp_processor_id() 检查。在此之前无法启用这些检查，因为如果配置了 CONFIG_PREEMPTION=y，
	 * kernel_thread() 会触发 might_sleep() 警告。对于 CONFIG_PREEMPT_VOLUNTARY=y，init 任务可能已经调度，但它被卡在
	 * kthreadd_done 完成等待上。
	 */
	system_state = SYSTEM_SCHEDULING;//更新系统状态为调度阶段

	complete(&kthreadd_done);//标记 kthreadd 的初始化已完成

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 * 动空闲线程必须至少调用一次 schedule()，以使系统开始运转：
	 */
	schedule_preempt_disabled();//调用调度函数，以便空闲线程能够进行调度
	/* 调用 cpu_idle，进入 CPU 空闲状态，禁用抢占 */
	cpu_startup_entry(CPUHP_ONLINE);//启动 CPU 并进入空闲状态，表示 CPU 已上线
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strscpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak arch_post_acpi_subsys_init(void) { }

void __init __weak smp_setup_processor_id(void)
{
}

void __init __weak smp_prepare_boot_cpu(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

void __init __weak poking_init(void) { }

void __init __weak pgtable_cache_init(void) { }

void __init __weak trap_init(void) { }

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void);
#else
static inline void initcall_debug_enable(void)
{
}
#endif

#ifdef CONFIG_RANDOMIZE_KSTACK_OFFSET
DEFINE_STATIC_KEY_MAYBE_RO(CONFIG_RANDOMIZE_KSTACK_OFFSET_DEFAULT,
			   randomize_kstack_offset);
DEFINE_PER_CPU(u32, kstack_offset);

static int __init early_randomize_kstack_offset(char *buf)
{
	int ret;
	bool bool_result;

	ret = kstrtobool(buf, &bool_result);
	if (ret)
		return ret;

	if (bool_result)
		static_branch_enable(&randomize_kstack_offset);
	else
		static_branch_disable(&randomize_kstack_offset);
	return 0;
}
early_param("randomize_kstack_offset", early_randomize_kstack_offset);
#endif

static void __init print_unknown_bootoptions(void)
{
	char *unknown_options;
	char *end;
	const char *const *p;
	size_t len;

	if (panic_later || (!argv_init[1] && !envp_init[2]))
		return;

	/*
	 * Determine how many options we have to print out, plus a space
	 * before each
	 */
	len = 1; /* null terminator */
	for (p = &argv_init[1]; *p; p++) {
		len++;
		len += strlen(*p);
	}
	for (p = &envp_init[2]; *p; p++) {
		len++;
		len += strlen(*p);
	}

	unknown_options = memblock_alloc(len, SMP_CACHE_BYTES);
	if (!unknown_options) {
		pr_err("%s: Failed to allocate %zu bytes\n",
			__func__, len);
		return;
	}
	end = unknown_options;

	for (p = &argv_init[1]; *p; p++)
		end += sprintf(end, " %s", *p);
	for (p = &envp_init[2]; *p; p++)
		end += sprintf(end, " %s", *p);

	/* Start at unknown_options[1] to skip the initial space */
	pr_notice("Unknown kernel command line parameters \"%s\", will be passed to user space.\n",
		&unknown_options[1]);
	memblock_free(unknown_options, len);
}
/*用于在系统初始化早期设置每个 CPU 的 NUMA 节点 ID，从而优化 NUMA 环境下的内存访问。*/
static void __init early_numa_node_init(void)
{
#ifdef CONFIG_USE_PERCPU_NUMA_NODE_ID
#ifndef cpu_to_node
	int cpu;

	/* The early_cpu_to_node() should be ready here. */
	for_each_possible_cpu(cpu)//遍历系统中所有可能的 CPU
		set_cpu_numa_node(cpu, early_cpu_to_node(cpu));//将每个 CPU 的 NUMA 节点 ID 设置为 early_cpu_to_node(cpu) 返回的值。early_cpu_to_node(cpu) 通常会返回在系统初始化早期检测到的 CPU 对应的 NUMA 节点 ID，确保在内存分配和访问时尽量使用本地节点的内存，从而优化内存访问性能。
#endif
#endif
}

asmlinkage __visible __init __no_sanitize_address __noreturn __no_stack_protector
void start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	set_task_stack_end_magic(&init_task);//设置初始任务的堆栈结束标记，用于检测堆栈溢出
	smp_setup_processor_id();//设置当前处理器的 ID（用于多处理器系统）
	debug_objects_early_init();//初始化调试对象，用于早期检测内核对象的使用情况
	init_vmlinux_build_id();//初始化 vmlinux 构建 ID，用于标识内核的构建版本

	cgroup_init_early();//早期初始化控制组（cgroup），用于资源限制和监控

	local_irq_disable();//禁用本地中断，防止在初始化过程中被打断
	early_boot_irqs_disabled = true;

	/*
	 * Interrupts are still disabled. Do necessary setups, then
	 * enable them.
	 * 中断仍然被禁用。执行必要的设置，然后启用它们。
	 */
	boot_cpu_init();//初始化引导 CPU，包括设置 CPU 特定的状态和功能
	page_address_init();//初始化页面地址系统，设置页面地址的管理结构
	pr_notice("%s", linux_banner);//打印 Linux 内核启动横幅，显示内核版本等信息
	early_security_init();//早期安全性初始化，设置内核的安全策略
	setup_arch(&command_line);//配置体系结构相关的设置并解析命令行参数
	setup_boot_config();//设置引导配置，读取并处理引导参数
	setup_command_line(command_line);//设置命令行参数，解析并保存命令行参数
	setup_nr_cpu_ids();//设置系统中的 CPU 数量
	setup_per_cpu_areas();//设置per_CPU 的内存区域，用于 CPU 特定的数据存储
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks 准备引导 CPU，包括设置特定于架构的钩子函数 */
	early_numa_node_init();//早期初始化 NUMA 节点，用于非统一内存访问系统
	boot_cpu_hotplug_init();//引导 CPU 热插拔初始化，允许引导 CPU 进行热插拔操作

	pr_notice("Kernel command line: %s\n", saved_command_line);//打印内核命令行，显示启动时的命令行参数
	/* parameters may set static keys */
	jump_label_init();//初始化跳转标签，优化内核代码路径
	parse_early_param();//解析早期参数，设置内核启动时需要的参数
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);//解析引导参数，处理命令行中的参数
	print_unknown_bootoptions();//打印未知的引导选项，显示未被识别的命令行参数
	if (!IS_ERR_OR_NULL(after_dashes))//如果存在额外的引导参数，进行解析
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);
	if (extra_init_args)//如果存在额外的初始化参数，进行解析
		parse_args("Setting extra init args", extra_init_args,
			   NULL, 0, -1, -1, NULL, set_init_arg);

	/* Architectural and non-timekeeping rng init, before allocator init */
	random_init_early(command_line);//早期初始化随机数生成器，用于内核随机数生成

	/*
	 * These use large bootmem allocations and must precede
	 * initalization of page allocator
	 * 这些使用大块的引导内存分配，必须在页面分配器初始化之前执行
	 */
	setup_log_buf(0);//初始化日志缓冲区，用于内核日志记录
	vfs_caches_init_early();//早期初始化虚拟文件系统缓存
	sort_main_extable();//排序主异常表，设置异常处理
	trap_init();//初始化陷阱处理，设置陷阱和异常处理函数
	mm_core_init();//初始化内存管理核心。设置内存管理结构
	poking_init();//初始化内核代码修改机制
	ftrace_init();//初始化内核函数跟踪

	/* trace_printk can be enabled here */
	early_trace_init();//trace_printk 可以在这里启用，早期跟踪初始化

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 * 在开始任何中断（例如定时器中断）之前设置调度程序。
	 * 完整的拓扑设置在 smp_init() 时进行，但与此同时我们仍然有一个
	 * 功能正常的调度程序。
	 */
	sched_init();//初始化调度程序，设置任务调度的相关数据结构
	/*警告如果中断在非常早期就已启用，则禁用它们*/
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	radix_tree_init();//初始化基数树，用于快速查找和存储
	maple_tree_init();//初始化枫树，用于快速查找和存储

	/*
	 * Set up housekeeping before setting up workqueues to allow the unbound
	 * workqueue to take non-housekeeping into account.
	 *
	 */
	housekeeping_init();//初始化后台管理，用于处理系统后台任务

	/*
	 * Allow workqueue creation and work item queueing/cancelling
	 * early.  Work item execution depends on kthreads and starts after
	 * workqueue_init().
	 * 早期允许工作队列创建和工作项排队/取消。
	 * 工作项执行依赖于 kthreads 并在 workqueue_init() 之后开始。
	 */
	workqueue_init_early();//早期初始化工作队列，设置工作队列的管理结构

	rcu_init();//初始化 RCU（Read-Copy Update），用于并发控制

	/* Trace events are available after this */
	trace_init();//跟踪事件在此之后可用，初始化跟踪系统

	if (initcall_debug)//如果启用了 initcall 调试，则启用它
		initcall_debug_enable();

	context_tracking_init();//初始化上下文跟踪，用于任务上下文切换
	/* 在 init_ISA_irqs() 之前初始化一些链接 */
	early_irq_init();
	init_IRQ();//初始化中断处理，设置中断向量和处理函数
	tick_init();//初始化与系统时钟相关的功能
	rcu_init_nohz();//初始化无驻留 RCU，用于低延迟的 RCU 操作
	init_timers();//初始化定时器，设置内核定时器
	srcu_init();//初始化 SRCU（Sleepable RCU），用于睡眠状态下的 RCU 操作
	hrtimers_init();//初始化高精度定时器，设置高精度定时器中断处理
	softirq_init();//初始化软中断，设置软中断处理函数
	timekeeping_init();//初始化时间保持，设置系统时间的管理结构
	time_init();//初始化系统时间，设置时间相关的管理结构

	/* 这必须在时间保持初始化之后，初始化随机数生成器 */
	random_init();

	/* These make use of the fully initialized rng */
	kfence_init();
	boot_init_stack_canary();

	perf_event_init();//初始化性能事件，设置性能监控相关的管理结构
	profile_init();//初始化性能分析，设置性能分析相关的数据结构
	call_function_init();//初始化跨 CPU 调用，设置跨 CPU 函数调用的管理结构
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");//警告如果中断在早期已启用，则打印警告信息

	early_boot_irqs_disabled = false;//设置早期引导中断禁用状态为false
	local_irq_enable();//启用本地中断，允许中断处理

	kmem_cache_init_late();//延迟初始化内存缓存，设置内存缓存的管理结构

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 * 提前启用控制台。我们在进行 PCI 设置等之前启用控制台，console_init() 
	 * 必须意识到这一点。但我们确实希望提前输出，以防出现问题
	 */
	console_init();// 初始化控制台，用于输出内核日志
	if (panic_later)//如果存在 panic 变量，立即触发内核 panic
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	lockdep_init();//初始化锁依赖性检查，设置锁相关的依赖性检查结构

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 * 需要在启用中断时运行此功能，因为它还希望自测 
	 * [硬/软]-中断开/关锁反转错误：
	 */
	locking_selftest();//进行锁自测，检测锁相关的错误

#ifdef CONFIG_BLK_DEV_INITRD
	/*如果初始 RAM 磁盘被覆盖，则禁用它*/
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	setup_per_cpu_pageset();//初始化每个CPU的页面集，设置每个cpu的页面管理结构
	numa_policy_init();//初始化 NUMA 策略，设置 NUMA 相关的管理结构
	acpi_early_init();//早期初始化 ACPI，设置 ACPI 管理结构
	if (late_time_init)//如果存在延迟时间初始化函数，则调用它
		late_time_init();
	sched_clock_init();//初始化调度时钟，设置调度相关的时间管理结构
	calibrate_delay();//校准延迟，计算 CPU 的延迟时间

	arch_cpu_finalize_init();//完成架构 CPU 的初始化，设置 CPU 相关的管理结构

	pid_idr_init();//初始化 PID IDR，设置 PID 管理结构
	anon_vma_init();//初始化匿名 VMA，设置匿名内存区域管理结构
#ifdef CONFIG_X86
	/*如果启用了 EFI 运行时服务，则进入虚拟模式*/
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
	thread_stack_cache_init();//初始化线程栈缓存，设置线程相关的栈管理结构
	cred_init();//初始化权限管理，设置权限相关的管理结构
	fork_init();//初始化 fork 子系统，设置 fork 相关的管理结构
	proc_caches_init();//初始化进程缓存，设置进程相关的缓存管理结构
	uts_ns_init();//初始化 UTS 命名空间，设置 UTS 相关的命名空间管理结构
	key_init();//初始化密钥管理，设置密钥相关的管理结构
	security_init();//初始化安全子系统，设置安全相关的管理结构
	dbg_late_init();//进行调试晚期初始化，设置调试相关的管理结构
	net_ns_init();//初始化网络命名空间，设置网络相关的命名空间管理结构
	vfs_caches_init();//初始化虚拟文件系统缓存，设置文件系统相关的缓存管理结构
	pagecache_init();//初始化页面缓存，设置页面缓存管理结构(页面回写操作相关)
	signals_init();//初始化信号子系统，设置信号相关的管理结构
	seq_file_init();//初始化内核中的 seq_file 缓存池
	proc_root_init();//初始化 proc 根文件系统，设置 proc 文件系统相关的管理结构
	nsfs_init();//挂载命名空间文件系统
	pidfs_init();//挂载 PID 文件系统
	cpuset_init();//初始化 CPU 集合，设置 CPU 集合管理结构
	cgroup_init();//初始化控制组，设置控制组相关的管理结构
	taskstats_init_early();//早期初始化任务统计，设置任务统计管理结构
	delayacct_init();//初始化延迟会计，设置延迟相关的会计管理结构

	acpi_subsystem_init();//初始化 ACPI 子系统，设置 ACPI 相关的管理结构
	arch_post_acpi_subsys_init();//处理架构特定的 ACPI 子系统初始化
	kcsan_init();//初始化内核竞争检测工具，设置内核竞争检测管理结构

	/* Do the rest non-__init'ed, we're now alive */
	rest_init();//执行剩余的非 __init 标记的初始化，内核现在进入正常运行状态

	/*
	 * Avoid stack canaries in callers of boot_init_stack_canary for gcc-10
	 * and older.
	 */
#if !__has_attribute(__no_stack_protector__)
	prevent_tail_call_optimization();//防止尾调用优化
#endif
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
/*
 * For UML, the constructors have already been called by the
 * normal setup code as it's just a normal ELF binary, so we
 * cannot do it again - but we do need CONFIG_CONSTRUCTORS
 * even on UML for modules.
 */
#if defined(CONFIG_CONSTRUCTORS) && !defined(CONFIG_UML)
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = memblock_alloc(sizeof(*entry),
					       SMP_CACHE_BYTES);
			if (!entry)
				panic("%s: Failed to allocate %zu bytes\n",
				      __func__, sizeof(*entry));
			entry->buf = memblock_alloc(strlen(str_entry) + 1,
						    SMP_CACHE_BYTES);
			if (!entry->buf)
				panic("%s: Failed to allocate %zu bytes\n",
				      __func__, strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static __init_or_module void
trace_initcall_start_cb(void *data, initcall_t fn)
{
	ktime_t *calltime = data;

	printk(KERN_DEBUG "calling  %pS @ %i\n", fn, task_pid_nr(current));
	*calltime = ktime_get();
}

static __init_or_module void
trace_initcall_finish_cb(void *data, initcall_t fn, int ret)
{
	ktime_t rettime, *calltime = data;

	rettime = ktime_get();
	printk(KERN_DEBUG "initcall %pS returned %d after %lld usecs\n",
		 fn, ret, (unsigned long long)ktime_us_delta(rettime, *calltime));
}

static ktime_t initcall_calltime;

#ifdef TRACEPOINTS_ENABLED
static void __init initcall_debug_enable(void)
{
	int ret;

	ret = register_trace_initcall_start(trace_initcall_start_cb,
					    &initcall_calltime);
	ret |= register_trace_initcall_finish(trace_initcall_finish_cb,
					      &initcall_calltime);
	WARN(ret, "Failed to register initcall tracepoints\n");
}
# define do_trace_initcall_start	trace_initcall_start
# define do_trace_initcall_finish	trace_initcall_finish
#else
static inline void do_trace_initcall_start(initcall_t fn)
{
	if (!initcall_debug)
		return;
	trace_initcall_start_cb(&initcall_calltime, fn);
}
static inline void do_trace_initcall_finish(initcall_t fn, int ret)
{
	if (!initcall_debug)
		return;
	trace_initcall_finish_cb(&initcall_calltime, fn, ret);
}
#endif /* !TRACEPOINTS_ENABLED */

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	char msgbuf[64];
	int ret;

	if (initcall_blacklisted(fn))
		return -EPERM;

	do_trace_initcall_start(fn);
	ret = fn();
	do_trace_initcall_finish(fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pS returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


static initcall_entry_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static const char *initcall_level_names[] __initdata = {
	"pure",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static int __init ignore_unknown_bootoption(char *param, char *val,
			       const char *unused, void *arg)
{
	return 0;
}

static void __init do_initcall_level(int level, char *command_line)
{
	initcall_entry_t *fn;

	parse_args(initcall_level_names[level],
		   command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, ignore_unknown_bootoption);

	trace_initcall_level(initcall_level_names[level]);
	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static void __init do_initcalls(void)
{
	int level;
	size_t len = saved_command_line_len + 1;
	char *command_line;

	command_line = kzalloc(len, GFP_KERNEL);
	if (!command_line)
		panic("%s: Failed to allocate %zu bytes\n", __func__, len);

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++) {
		/* Parser modifies command_line, restore it each time */
		strcpy(command_line, saved_command_line);
		do_initcall_level(level, command_line);
	}

	kfree(command_line);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	cpuset_init_smp();
	driver_init();
	init_irq_proc();
	do_ctors();
	do_initcalls();
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_entry_t *fn;

	trace_initcall_level("early");
	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(initcall_from_entry(fn));
}

static int run_init_process(const char *init_filename)
{
	const char *const *p;

	argv_init[0] = init_filename;
	pr_info("Run %s as init process\n", init_filename);
	pr_debug("  with arguments:\n");
	for (p = argv_init; *p; p++)
		pr_debug("    %s\n", *p);
	pr_debug("  with environment:\n");
	for (p = envp_init; *p; p++)
		pr_debug("    %s\n", *p);
	return kernel_execve(init_filename, argv_init, envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_STRICT_KERNEL_RWX) || defined(CONFIG_STRICT_MODULE_RWX)
bool rodata_enabled __ro_after_init = true;

#ifndef arch_parse_debug_rodata
static inline bool arch_parse_debug_rodata(char *str) { return false; }
#endif

static int __init set_debug_rodata(char *str)
{
	if (arch_parse_debug_rodata(str))
		return 0;

	if (str && !strcmp(str, "on"))
		rodata_enabled = true;
	else if (str && !strcmp(str, "off"))
		rodata_enabled = false;
	else
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 0;
}
early_param("rodata", set_debug_rodata);
#endif

static void mark_readonly(void)
{
	if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX) && rodata_enabled) {
		/*
		 * load_module() results in W+X mappings, which are cleaned
		 * up with init_free_wq. Let's make sure that queued work is
		 * flushed so that we don't hit false positives looking for
		 * insecure pages which are W+X.
		 */
		flush_module_init_free_work();
		jump_label_init_ro();
		mark_rodata_ro();
		debug_checkwx();
		rodata_test();
	} else if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX)) {
		pr_info("Kernel memory protection disabled.\n");
	} else if (IS_ENABLED(CONFIG_ARCH_HAS_STRICT_KERNEL_RWX)) {
		pr_warn("Kernel memory protection not selected by kernel config.\n");
	} else {
		pr_warn("This architecture does not have kernel memory protection.\n");
	}
}

void __weak free_initmem(void)
{
	free_initmem_default(POISON_FREE_INITMEM);
}
/*由 `kthreadd` 调用的初始化函数,负责初始化内核以及启动用户态的 init 进程
 *内核线程的主要目标是在内核启动并完成基本初始化后，将控制权交给用户态的初始化程序。
 * */
static int __ref kernel_init(void *unused)
{
	int ret;

	/*
	 * Wait until kthreadd is all set-up.
	 */
	wait_for_completion(&kthreadd_done);// 等待 `kthreadd` 完成初始化，确保该线程已经完全准备就绪

	kernel_init_freeable();//进行内核剩余的可释放内存部分的初始化
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();//用于确保所有异步的初始化代码执行完毕，以防止在内存释放时仍有代码在运行。

	system_state = SYSTEM_FREEING_INITMEM;//设置系统状态为 `SYSTEM_FREEING_INITMEM`，表示系统正在释放 `init` 内存
	kprobe_free_init_mem();// 释放 `kprobe` 相关的初始化内存
	ftrace_free_init_mem();//释放 `ftrace` 相关的初始化内存
	kgdb_free_init_mem();//释放 `kgdb`（内核调试器）相关的初始化内存
	exit_boot_config();// 退出引导配置，释放相关的资源
	free_initmem();//释放所有初始化代码段的内存，以便节省内存资源
	mark_readonly();//将只读内存区域进行标记，以防止其被意外修改

	/*
	 * Kernel mappings are now finalized - update the userspace page-table
	 * to finalize PTI.
	 * 内核映射现在已经完成 - 更新用户空间页表以最终确定 PTI。
	 */
	pti_finalize();//最终确定页表隔离（PTI），确保用户空间和内核空间的安全隔离

	system_state = SYSTEM_RUNNING;//将系统状态设置为 `SYSTEM_RUNNING`，表示系统已经进入运行状态
	numa_default_policy();//设置 NUMA（非统一内存访问）默认策略

	rcu_end_inkernel_boot();//结束 RCU 的内核启动阶段，标记 RCU 进入正常运行状态

	do_sysctl_args();//执行系统控制命令行参数的处理

	if (ramdisk_execute_command) {//如果有 `ramdisk_execute_command`（ramdisk 的启动命令），则尝试运行它
		ret = run_init_process(ramdisk_execute_command);//调用 `run_init_process()` 运行 ramdisk 中的初始化进程
		if (!ret)//如果成功，则返回 0，表示内核启动成功
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);//如果失败，打印错误信息
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {//如果有 `execute_command`（启动命令），则尝试运行它
		ret = run_init_process(execute_command);//调用 `run_init_process()` 运行指定的初始化进程
		if (!ret)
			return 0;//如果成功，则返回 0
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);//如果失败，触发 panic，打印错误信息并停止系统
	}

	if (CONFIG_DEFAULT_INIT[0] != '\0') {//如果有默认的初始化命令，则尝试运行它
		ret = run_init_process(CONFIG_DEFAULT_INIT);//调用 `run_init_process()` 运行默认的初始化进程
		if (ret)//如果失败，打印错误信息
			pr_err("Default init %s failed (error %d)\n",
			       CONFIG_DEFAULT_INIT, ret);
		else
			return 0;//如果成功，返回 0
	}

	if (!try_to_run_init_process("/sbin/init") ||//尝试运行 `/sbin/init` 作为初始化进程
	    !try_to_run_init_process("/etc/init") ||//或者 `/etc/init`
	    !try_to_run_init_process("/bin/init") ||//或者 `/bin/init`
	    !try_to_run_init_process("/bin/sh"))//或者 `/bin/sh`
		return 0;//如果任何一个成功，返回 0

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/admin-guide/init.rst for guidance.");//如果没有找到可用的初始化进程，则触发 panic 并打印指导信息
}

/* Open /dev/console, for stdin/stdout/stderr, this should never fail */
void __init console_on_rootfs(void)
{
	struct file *file = filp_open("/dev/console", O_RDWR, 0);

	if (IS_ERR(file)) {
		pr_err("Warning: unable to open an initial console.\n");
		return;
	}
	init_dup(file);
	init_dup(file);
	init_dup(file);
	fput(file);
}

static noinline void __init kernel_init_freeable(void)
{
	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;//设置内存分配标志位，允许在所有节点上进行内存分配

	/*
	 * init can allocate pages on any node
	 */
	set_mems_allowed(node_states[N_MEMORY]);//允许初始化任务在系统内所有节点上分配内存

	cad_pid = get_pid(task_pid(current));//获取当前任务的 PID 并保存到全局变量 `cad_pid`，用于系统管理目的

	smp_prepare_cpus(setup_max_cpus);//准备多处理器系统，将所有可用 CPU 初始化并准备好

	workqueue_init();//初始化工作队列，用于处理内核中的异步任务

	init_mm_internals();//初始化内存管理内部结构，确保内存管理模块能正常工作

	rcu_init_tasks_generic();//初始化 RCU（Read-Copy Update）相关任务，用于保证并发访问的安全性
	do_pre_smp_initcalls();//执行 SMP（对称多处理）初始化之前需要完成的函数调用
	lockup_detector_init();// 初始化锁定检测器，用于检测系统的死锁情况

	smp_init();//初始化 SMP（对称多处理）环境，使多个 CPU 协同工作
	sched_init_smp();//初始化 SMP 调度器，使内核调度器能够适应多处理器的环境

	workqueue_init_topology();//初始化工作队列拓扑，用于支持多处理器环境中的工作队列处理
	async_init();//初始化异步子系统，支持内核中异步操作
	padata_init();//初始化 padata 子系统，用于并行处理和数据分发
	page_alloc_init_late();//初始化页面分配器，延迟执行，以适应内存管理的变化

	do_basic_setup();//完成基本的系统设置，初始化大多数核心子系统

	kunit_run_all_tests();//运行所有 KUnit 单元测试，确保内核功能的正确性

	wait_for_initramfs();//等待 initramfs（初始内存文件系统）的加载完成，确保系统能够加载根文件系统
	console_on_rootfs();//将控制台设置为根文件系统，确保后续输出可以正确显示

	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */
	if (init_eaccess(ramdisk_execute_command) != 0) {//如果存在有效的初始用户空间命令，执行它
		ramdisk_execute_command = NULL;//否则将初始命令设置为空
		prepare_namespace();//准备命名空间，挂载根文件系统
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */

	integrity_load_keys();//加载完整性验证公钥，用于系统的安全性验证
}
