/* SPDX-License-Identifier: GPL-2.0 */
/*
 * linux/cgroup-defs.h - basic definitions for cgroup
 *
 * This file provides basic type and interface.  Include this file directly
 * only if necessary to avoid cyclic dependencies.
 */
#ifndef _LINUX_CGROUP_DEFS_H
#define _LINUX_CGROUP_DEFS_H

#include <linux/limits.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/percpu-refcount.h>
#include <linux/percpu-rwsem.h>
#include <linux/u64_stats_sync.h>
#include <linux/workqueue.h>
#include <linux/bpf-cgroup-defs.h>
#include <linux/psi_types.h>

#ifdef CONFIG_CGROUPS

struct cgroup;
struct cgroup_root;
struct cgroup_subsys;
struct cgroup_taskset;
struct kernfs_node;
struct kernfs_ops;
struct kernfs_open_file;
struct seq_file;
struct poll_table_struct;

#define MAX_CGROUP_TYPE_NAMELEN 32
#define MAX_CGROUP_ROOT_NAMELEN 64
#define MAX_CFTYPE_NAME		64

/* define the enumeration of all cgroup subsystems */
#define SUBSYS(_x) _x ## _cgrp_id,
enum cgroup_subsys_id {
#include <linux/cgroup_subsys.h>
	CGROUP_SUBSYS_COUNT,
};
#undef SUBSYS

/* bits in struct cgroup_subsys_state flags field */
enum {
	CSS_NO_REF	= (1 << 0), /* no reference counting for this css */
	CSS_ONLINE	= (1 << 1), /* between ->css_online() and ->css_offline() */
	CSS_RELEASED	= (1 << 2), /* refcnt reached zero, released */
	CSS_VISIBLE	= (1 << 3), /* css is visible to userland */
	CSS_DYING	= (1 << 4), /* css is dying */
};

/* bits in struct cgroup flags field */
enum {
	/* Control Group requires release notifications to userspace */
	CGRP_NOTIFY_ON_RELEASE,
	/*
	 * Clone the parent's configuration when creating a new child
	 * cpuset cgroup.  For historical reasons, this option can be
	 * specified at mount time and thus is implemented here.
	 */
	CGRP_CPUSET_CLONE_CHILDREN,

	/* Control group has to be frozen. */
	CGRP_FREEZE,

	/* Cgroup is frozen. */
	CGRP_FROZEN,

	/* Control group has to be killed. */
	CGRP_KILL,
};

/* cgroup_root->flags */
enum {
	CGRP_ROOT_NOPREFIX	= (1 << 1), /* mounted subsystems have no named prefix */
	CGRP_ROOT_XATTR		= (1 << 2), /* supports extended attributes */

	/*
	 * Consider namespaces as delegation boundaries.  If this flag is
	 * set, controller specific interface files in a namespace root
	 * aren't writeable from inside the namespace.
	 */
	CGRP_ROOT_NS_DELEGATE	= (1 << 3),

	/*
	 * Reduce latencies on dynamic cgroup modifications such as task
	 * migrations and controller on/offs by disabling percpu operation on
	 * cgroup_threadgroup_rwsem. This makes hot path operations such as
	 * forks and exits into the slow path and more expensive.
	 *
	 * The static usage pattern of creating a cgroup, enabling controllers,
	 * and then seeding it with CLONE_INTO_CGROUP doesn't require write
	 * locking cgroup_threadgroup_rwsem and thus doesn't benefit from
	 * favordynmod.
	 */
	CGRP_ROOT_FAVOR_DYNMODS = (1 << 4),

	/*
	 * Enable cpuset controller in v1 cgroup to use v2 behavior.
	 */
	CGRP_ROOT_CPUSET_V2_MODE = (1 << 16),

	/*
	 * Enable legacy local memory.events.
	 */
	CGRP_ROOT_MEMORY_LOCAL_EVENTS = (1 << 17),

	/*
	 * Enable recursive subtree protection
	 */
	CGRP_ROOT_MEMORY_RECURSIVE_PROT = (1 << 18),

	/*
	 * Enable hugetlb accounting for the memory controller.
	 */
	 CGRP_ROOT_MEMORY_HUGETLB_ACCOUNTING = (1 << 19),
};

/* cftype->flags */
enum {
	CFTYPE_ONLY_ON_ROOT	= (1 << 0),	/* only create on root cgrp */
	CFTYPE_NOT_ON_ROOT	= (1 << 1),	/* don't create on root cgrp */
	CFTYPE_NS_DELEGATABLE	= (1 << 2),	/* writeable beyond delegation boundaries */

	CFTYPE_NO_PREFIX	= (1 << 3),	/* (DON'T USE FOR NEW FILES) no subsys prefix */
	CFTYPE_WORLD_WRITABLE	= (1 << 4),	/* (DON'T USE FOR NEW FILES) S_IWUGO */
	CFTYPE_DEBUG		= (1 << 5),	/* create when cgroup_debug */

	/* internal flags, do not use outside cgroup core proper */
	__CFTYPE_ONLY_ON_DFL	= (1 << 16),	/* only on default hierarchy */
	__CFTYPE_NOT_ON_DFL	= (1 << 17),	/* not on default hierarchy */
	__CFTYPE_ADDED		= (1 << 18),
};

/*
 * cgroup_file is the handle for a file instance created in a cgroup which
 * is used, for example, to generate file changed notifications.  This can
 * be obtained by setting cftype->file_offset.
 */
struct cgroup_file {
	/* do not access any fields from outside cgroup core */
	struct kernfs_node *kn;
	unsigned long notified_at;
	struct timer_list notify_timer;
};

/*
 * Per-subsystem/per-cgroup state maintained by the system.  This is the
 * fundamental structural building block that controllers deal with.
 *
 * Fields marked with "PI:" are public and immutable and may be accessed
 * directly without synchronization.
 */
struct cgroup_subsys_state {//用于表示 cgroup 中每个子系统（如 CPU、内存等）的状态，
	/* PI: the cgroup that this css is attached to */
	struct cgroup *cgroup;//指向该子系统状态所属的 cgroup

	/* PI: the cgroup subsystem that this css is attached to */
	struct cgroup_subsys *ss;//指向该子系统状态所属的 cgroup 子系统

	/* reference count - access via css_[try]get() and css_put() */
	struct percpu_ref refcnt;//引用计数，管理该子系统状态的引用，以实现生命周期控制

	/* siblings list anchored at the parent's ->children */
	struct list_head sibling;//与同一父级 cgroup 的其他子节点组成的兄弟链表
	struct list_head children;// 该子系统状态的子节点链表

	/* flush target list anchored at cgrp->rstat_css_list */
	struct list_head rstat_css_node;//用于资源统计刷新，连接到 cgroup 的 rstat_css_list 列表中

	/*
	 * PI: Subsys-unique ID.  0 is unused and root is always 1.  The
	 * matching css can be looked up using css_from_id().
	 */
	int id;//子系统状态的唯一标识符，0 未使用，根节点为 1

	unsigned int flags;//标志位，表示该子系统状态的特性

	/*
	 * Monotonically increasing unique serial number which defines a
	 * uniform order among all csses.  It's guaranteed that all
	 * ->children lists are in the ascending order of ->serial_nr and
	 * used to allow interrupting and resuming iterations.
	 */
	u64 serial_nr;//唯一的序列号，定义子系统状态的统一顺序，保证所有子节点按序排列

	/*
	 * Incremented by online self and children.  Used to guarantee that
	 * parents are not offlined before their children.
	 */
	atomic_t online_cnt;//在线计数器，确保父节点不会在其子节点之前被下线

	/* percpu_ref killing and RCU release */
	struct work_struct destroy_work;//销毁任务结构，处理引用计数的清理
	struct rcu_work destroy_rwork;//RCU 销毁任务，延迟释放资源

	/*
	 * PI: the parent css.	Placed here for cache proximity to following
	 * fields of the containing structure.
	 */
	struct cgroup_subsys_state *parent;//指向父级子系统状态，以便于遍历和管理层次结构
};

/*
 * A css_set is a structure holding pointers to a set of
 * cgroup_subsys_state objects. This saves space in the task struct
 * object and speeds up fork()/exit(), since a single inc/dec and a
 * list_add()/del() can bump the reference count on the entire cgroup
 * set for a task.
 */
struct css_set {//cgroup 子系统状态集合，表示一个任务在多个子系统上的状态集合，通常与一个任务组关联
	/*
	 * Set of subsystem states, one for each subsystem. This array is
	 * immutable after creation apart from the init_css_set during
	 * subsystem registration (at boot time).
	 */
	struct cgroup_subsys_state *subsys[CGROUP_SUBSYS_COUNT];//子系统状态数组，包含该集合中所有子系统的状态

	/* reference count */
	refcount_t refcount;//引用计数，管理该集合的引用次数，用于生命周期控制

	/*
	 * For a domain cgroup, the following points to self.  If threaded,
	 * to the matching cset of the nearest domain ancestor.  The
	 * dom_cset provides access to the domain cgroup and its csses to
	 * which domain level resource consumptions should be charged.
	 * 对于 domain 类型的 cgroup，以下指针指向自身；如果是 threaded 类型，则指向最近的 domain 祖先的匹配 cset。
	 * dom_cset 用于访问 domain cgroup 及其子系统状态，以便计算域级资源消耗。
	 */
	struct css_set *dom_cset;//指向 domain 类型的 cgroup 子系统集合或最近的 domain 祖先

	/* the default cgroup associated with this css_set */
	struct cgroup *dfl_cgrp;//该集合关联的默认 cgroup

	/* internal task count, protected by css_set_lock */
	int nr_tasks;// 内部任务的数量，用于跟踪属于该集合的任务数

	/*
	 * Lists running through all tasks using this cgroup group.
	 * mg_tasks lists tasks which belong to this cset but are in the
	 * process of being migrated out or in.  Protected by
	 * css_set_lock, but, during migration, once tasks are moved to
	 * mg_tasks, it can be read safely while holding cgroup_mutex.
	 * 列表遍历所有使用此 cgroup 组的任务。mg_tasks 列出属于此集合但正在迁移中的任务。由 css_set_lock 保护。
	 * 但在迁移过程中，一旦任务移动到 mg_tasks，则可以在持有 cgroup_mutex 时安全读取。
	 */
	struct list_head tasks;//所有使用此集合的任务链表
	struct list_head mg_tasks;//正在迁移中的任务链表
	struct list_head dying_tasks;//正在退出的任务链表

	/* all css_task_iters currently walking this cset */
	struct list_head task_iters;//遍历该集合的任务迭代器链表

	/*
	 * On the default hierarchy, ->subsys[ssid] may point to a css
	 * attached to an ancestor instead of the cgroup this css_set is
	 * associated with.  The following node is anchored at
	 * ->subsys[ssid]->cgroup->e_csets[ssid] and provides a way to
	 * iterate through all css's attached to a given cgroup.
	 * 在默认层级中，->subsys[ssid] 可能指向附加到祖先的 css，而不是此
	 * css_set 所关联的 cgroup。以下节点锚定在->subsys[ssid]->cgroup->e_csets[ssid] 上,
	 * 提供了一种遍历所有附加到给定 cgroup 的 css 的方式。
	 */
	struct list_head e_cset_node[CGROUP_SUBSYS_COUNT];//链接到 cgroup 的所有附加子系统状态的节点

	/* all threaded csets whose ->dom_cset points to this cset */
	struct list_head threaded_csets;//所有将此集合作为 domain cset 的 threaded cset 链表
	struct list_head threaded_csets_node;//threaded 集合中的节点

	/*
	 * List running through all cgroup groups in the same hash
	 * slot. Protected by css_set_lock
	 * 列表遍历所有位于同一哈希槽中的 cgroup 组。由 css_set_lock 保护。
	 */
	struct hlist_node hlist;//哈希表节点，用于将 css_set 组织在同一哈希槽中

	/*
	 * List of cgrp_cset_links pointing at cgroups referenced from this
	 * css_set.  Protected by css_set_lock.
	 */
	struct list_head cgrp_links;//关联的 cgroup 链接列表

	/*
	 * List of csets participating in the on-going migration either as
	 * source or destination.  Protected by cgroup_mutex.
	 */
	struct list_head mg_src_preload_node;//作为迁移源的节点
	struct list_head mg_dst_preload_node;//作为迁移目标的节点
	struct list_head mg_node;//正在迁移的节点

	/*
	 * If this cset is acting as the source of migration the following
	 * two fields are set.  mg_src_cgrp and mg_dst_cgrp are
	 * respectively the source and destination cgroups of the on-going
	 * migration.  mg_dst_cset is the destination cset the target tasks
	 * on this cset should be migrated to.  Protected by cgroup_mutex.
	 * 如果此 cset 作为迁移的源，则以下两个字段会被设置。mg_src_cgrp 和 
	 * mg_dst_cgrp 分别是正在进行迁移的源和目标 cgroup。g_dst_cset 是目
	 * 标 cset，目标任务应从此集合迁移到。
	 */
	struct cgroup *mg_src_cgrp;//当前迁移的源 cgroup
	struct cgroup *mg_dst_cgrp;//当前迁移的目标 cgroup
	struct css_set *mg_dst_cset;//当前迁移的目标 css_set

	/* dead and being drained, ignore for migration */
	bool dead;//表示该集合是否已终止

	/* For RCU-protected deletion */
	struct rcu_head rcu_head;//RCU 头部结构，用于延迟删除
};

struct cgroup_base_stat {
	struct task_cputime cputime;

#ifdef CONFIG_SCHED_CORE
	u64 forceidle_sum;
#endif
};

/*
 * rstat - cgroup scalable recursive statistics.  Accounting is done
 * per-cpu in cgroup_rstat_cpu which is then lazily propagated up the
 * hierarchy on reads.
 *
 * When a stat gets updated, the cgroup_rstat_cpu and its ancestors are
 * linked into the updated tree.  On the following read, propagation only
 * considers and consumes the updated tree.  This makes reading O(the
 * number of descendants which have been active since last read) instead of
 * O(the total number of descendants).
 *
 * This is important because there can be a lot of (draining) cgroups which
 * aren't active and stat may be read frequently.  The combination can
 * become very expensive.  By propagating selectively, increasing reading
 * frequency decreases the cost of each read.
 *
 * This struct hosts both the fields which implement the above -
 * updated_children and updated_next - and the fields which track basic
 * resource statistics on top of it - bsync, bstat and last_bstat.
 */
struct cgroup_rstat_cpu {
	/*
	 * ->bsync protects ->bstat.  These are the only fields which get
	 * updated in the hot path.
	 */
	struct u64_stats_sync bsync;
	struct cgroup_base_stat bstat;

	/*
	 * Snapshots at the last reading.  These are used to calculate the
	 * deltas to propagate to the global counters.
	 */
	struct cgroup_base_stat last_bstat;

	/*
	 * This field is used to record the cumulative per-cpu time of
	 * the cgroup and its descendants. Currently it can be read via
	 * eBPF/drgn etc, and we are still trying to determine how to
	 * expose it in the cgroupfs interface.
	 */
	struct cgroup_base_stat subtree_bstat;

	/*
	 * Snapshots at the last reading. These are used to calculate the
	 * deltas to propagate to the per-cpu subtree_bstat.
	 */
	struct cgroup_base_stat last_subtree_bstat;

	/*
	 * Child cgroups with stat updates on this cpu since the last read
	 * are linked on the parent's ->updated_children through
	 * ->updated_next.
	 *
	 * In addition to being more compact, singly-linked list pointing
	 * to the cgroup makes it unnecessary for each per-cpu struct to
	 * point back to the associated cgroup.
	 *
	 * Protected by per-cpu cgroup_rstat_cpu_lock.
	 */
	struct cgroup *updated_children;	/* terminated by self cgroup */
	struct cgroup *updated_next;		/* NULL iff not on the list */
};

struct cgroup_freezer_state {
	/* Should the cgroup and its descendants be frozen. */
	bool freeze;

	/* Should the cgroup actually be frozen? */
	int e_freeze;

	/* Fields below are protected by css_set_lock */

	/* Number of frozen descendant cgroups */
	int nr_frozen_descendants;

	/*
	 * Number of tasks, which are counted as frozen:
	 * frozen, SIGSTOPped, and PTRACEd.
	 */
	int nr_frozen_tasks;
};

struct cgroup {//用于描述控制组（cgroup）及其属性的数据结构
	/* 自己的子系统状态（css），其 ->ss 为空，指向自身的 cgroup */
	struct cgroup_subsys_state self;//该 cgroup 自身的子系统状态结构，用于管理该 cgroup 的生命周期和状态

	unsigned long flags;//cgroup 的标志位，使用 unsigned long 以支持位操作

	/*
	 * 该 cgroup 的深度，根节点的深度为 0，每向下走一级，深度递增 1。 
	 * 结合 ancestors[] 可以确定一个 cgroup 是否是另一个的后代，而无需遍历整个层级。
	 */
	int level;//该 cgroup 在 cgroup 层级中的深度

	/* Maximum allowed descent tree depth */
	int max_depth;//cgroup 子树的最大允许深度

	/*
	 * Keep track of total numbers of visible and dying descent cgroups.
	 * Dying cgroups are cgroups which were deleted by a user,
	 * but are still existing because someone else is holding a reference.
	 * max_descendants is a maximum allowed number of descent cgroups.
	 *
	 * nr_descendants and nr_dying_descendants are protected
	 * by cgroup_mutex and css_set_lock. It's fine to read them holding
	 * any of cgroup_mutex and css_set_lock; for writing both locks
	 * should be held.
	 */
	int nr_descendants;//可见的子 cgroup 数量
	int nr_dying_descendants;//"dying" 状态的子 cgroup数量.(dying状态指被用户删除的的cgroup)
	int max_descendants;// 允许的最大子 cgroup 数量

	/*
	 * Each non-empty css_set associated with this cgroup contributes
	 * one to nr_populated_csets.  The counter is zero iff this cgroup
	 * doesn't have any tasks.
	 *
	 * All children which have non-zero nr_populated_csets and/or
	 * nr_populated_children of their own contribute one to either
	 * nr_populated_domain_children or nr_populated_threaded_children
	 * depending on their type.  Each counter is zero iff all cgroups
	 * of the type in the subtree proper don't have any tasks.
	 */
	int nr_populated_csets;//与该 cgroup 关联的非空 css_set 数量
	int nr_populated_domain_children;//子 cgroup 中有任务的 domain 类型子 cgroup 数量
	int nr_populated_threaded_children;//子 cgroup 中有任务的 threaded 类型子 cgroup 数量

	int nr_threaded_children;//活跃的 threaded 类型子 cgroup 的数量

	struct kernfs_node *kn;	//该 cgroup 在 kernfs 中的节点，表示在文件系统中的条目
	struct cgroup_file procs_file;//"cgroup.procs" 文件的句柄，用于管理 cgroup 内任务
	struct cgroup_file events_file;// "cgroup.events" 文件的句柄，用于记录和管理事件

	/* handles for "{cpu,memory,io,irq}.pressure" */
	struct cgroup_file psi_files[NR_PSI_RESOURCES];//用于管理 CPU、内存、IO 和 IRQ 压力的文件句柄

	/*
	 * The bitmask of subsystems enabled on the child cgroups.
	 * ->subtree_control is the one configured through
	 * "cgroup.subtree_control" while ->subtree_ss_mask is the effective
	 * one which may have more subsystems enabled.  Controller knobs
	 * are made available iff it's enabled in ->subtree_control.
	 */
	u16 subtree_control;//子 cgroup 允许的子系统控制
	u16 subtree_ss_mask;//子 cgroup 实际启用的子系统掩码
	u16 old_subtree_control;// 旧的子系统控制
	u16 old_subtree_ss_mask;//旧的子系统掩码

	/* Private pointers for each registered subsystem */
	struct cgroup_subsys_state __rcu *subsys[CGROUP_SUBSYS_COUNT];//指向各个子系统的状态指针数组

	struct cgroup_root *root;//指向该 cgroup 的根结构

	/*
	 * 指向 css_sets 的 cgrp_cset_links 列表，css_sets 中包含在该 cgroup 中的任务。
	 * 受 css_set_lock 保护。
	 */
	struct list_head cset_links;//指向 css_set 的链接列表，用于关联该 cgroup 的 css_set

	/*
	 * 在默认层级中，一个具有部分子系统禁用的 cgroup 的css_set会指向启用
	 * 了该子系统的最近祖先的 css。
	 */
	struct list_head e_csets[CGROUP_SUBSYS_COUNT];//指向与该子系统关联的 css_set 列表

	/*
	 * If !threaded, self.  If threaded, it points to the nearest
	 * domain ancestor.  Inside a threaded subtree, cgroups are exempt
	 * from process granularity and no-internal-task constraint.
	 * Domain level resource consumptions which aren't tied to a
	 * specific task are charged to the dom_cgrp.
	 */
	struct cgroup *dom_cgrp;//指向最近的 domain 类型祖先的指针
	struct cgroup *old_dom_cgrp;//在启用 threaded 时使用，指向旧的 domain cgroup

	/* 每个 CPU 的递归资源统计信息 */
	struct cgroup_rstat_cpu __percpu *rstat_cpu;//每个 CPU 的资源统计
	struct list_head rstat_css_list;//资源统计的 css 列表

	/*
	 * 为 rstat_flush 添加填充，以将 rstat_cpu 和 rstat_css_list 与频繁更新的字段隔开。
	 */
	CACHELINE_PADDING(_pad_);

	/*
	 * A singly-linked list of cgroup structures to be rstat flushed.
	 * This is a scratch field to be used exclusively by
	 * cgroup_rstat_flush_locked() and protected by cgroup_rstat_lock.
	 * 用于 rstat 刷新的单链表，仅由 cgroup_rstat_flush_locked() 使用，受
	 * cgroup_rstat_lock 保护。
	 */
	struct cgroup	*rstat_flush_next;//指向下一个需要进行资源统计刷新的 cgroup

	/* cgroup basic resource statistics */
	struct cgroup_base_stat last_bstat;//上一次的基本资源统计
	struct cgroup_base_stat bstat;//当前的基本资源统计
	struct prev_cputime prev_cputime;//用于打印 cgroup CPU 时间的统计

	/*
	 * list of pidlists, up to two for each namespace (one for procs, one
	 * for tasks); created on demand.
	 * pidlists 列表，每个命名空间最多有两个（一个用于 procs，一个用于 tasks），
	 * 根据需求创建。
	 */
	struct list_head pidlists;//存储与该 cgroup 相关的 pid 列表
	struct mutex pidlist_mutex;//保护 pidlists 的互斥锁

	/* used to wait for offlining of csses */
	wait_queue_head_t offline_waitq;//等待队列，用于等待子系统状态的下线

	/* used to schedule release agent */
	struct work_struct release_agent_work;//用于调度释放代理的工作结构

	/* used to track pressure stalls */
	struct psi_group *psi;//用于跟踪资源压力的 psi 组指针

	/* used to store eBPF programs */
	struct cgroup_bpf bpf;//关联的 eBPF 程序，扩展 cgroup 的功能

	/* If there is block congestion on this cgroup. */
	atomic_t congestion_count;//跟踪该 cgroup 是否存在块设备的拥塞情况

	/* Used to store internal freezer state */
	struct cgroup_freezer_state freezer;//该 cgroup 的冻结状态

#ifdef CONFIG_BPF_SYSCALL
	struct bpf_local_storage __rcu  *bpf_cgrp_storage;//用于存储 eBPF 的本地存储指针
#endif

	/* All ancestors including self */
	struct cgroup *ancestors[];//包含自身的所有祖先 cgroup，用于快速遍历祖先链
};

/*
 * A cgroup_root represents the root of a cgroup hierarchy, and may be
 * associated with a kernfs_root to form an active hierarchy.  This is
 * internal to cgroup core.  Don't access directly from controllers.
 */
struct cgroup_root {//描述 cgroup 层级的结构体，每个 cgroup 层级（hierarchy）都有一个唯一的 cgroup_root 来管理该层级的根节点及其相关信息
	struct kernfs_root *kf_root;//指向与该 cgroup 根节点相关联的 kernfs 根节点

	/* The bitmask of subsystems attached to this hierarchy */
	unsigned int subsys_mask;//子系统的位掩码，表示附加到该层级的所有子系统

	/* Unique id for this hierarchy. */
	int hierarchy_id;//层级的唯一标识符，用于区分不同的层级

	/* A list running through the active hierarchies */
	struct list_head root_list;//连接所有活动层级的链表，用于遍历和管理
	struct rcu_head rcu;//RCU 机制的头部结构，支持并发访问和延迟释放

	/*
	 * 根 cgroup。在其释放时会销毁包含的 cgroup_root。cgrp->ancestors[0] 
	 * 将被用于溢出到以下字段。cgrp_ancestor_storage 必须紧随其后。
	 */
	struct cgroup cgrp;//根 cgroup，表示 cgroup 层级中的顶层 cgroup

	/* must follow cgrp for cgrp->ancestors[0], see above */
	struct cgroup *cgrp_ancestor_storage;//存储根cgroup的祖先节点信息

	/* Number of cgroups in the hierarchy, used only for /proc/cgroups */
	atomic_t nr_cgrps;//该层级中的 cgroup 数量，以原子方式进行管理，确保线程安全

	/* Hierarchy-specific flags */
	unsigned int flags;// 用于表示该层级的特定标志信息，决定层级的行为

	/* The path to use for release notifications. */
	char release_agent_path[PATH_MAX];//当cgroup释放时触发通知的路径

	/* The name for this hierarchy - may be empty */
	char name[MAX_CGROUP_ROOT_NAMELEN];//该层级的名称，允许用户为层级命名，可能为空
};

/*
 * struct cftype: handler definitions for cgroup control files
 *
 * When reading/writing to a file:
 *	- the cgroup to use is file->f_path.dentry->d_parent->d_fsdata
 *	- the 'cftype' of the file is file->f_path.dentry->d_fsdata
 */
struct cftype {//表示控制组（cgroup）中的一个控制文件，用于管理与 cgroup 子系统相关的控制文件操作和文件的内部属性
	/*
	 * By convention, the name should begin with the name of the
	 * subsystem, followed by a period.  Zero length string indicates
	 * end of cftype array.
	 */
	char name[MAX_CFTYPE_NAME];//表示该控制文件类型的名称，通常命名为子系统名称后加一个点（如 cpu.shares）
	unsigned long private;//用于存储与该文件类型相关的私有数据。具体的用途通常由 cgroup 子系统定义。

	/*
	 * The maximum length of string, excluding trailing nul, that can
	 * be passed to write.  If < PAGE_SIZE-1, PAGE_SIZE-1 is assumed.
	 */
	size_t max_write_len;//表示可以写入该文件的最大字符数（不包括字符串结束符 \0）。如果小于 PAGE_SIZE - 1，则默认为 PAGE_SIZE - 1。

	/* CFTYPE_* flags */
	unsigned int flags;//用于标识该文件类型的标志，定义了文件的行为特性，如是否可以写、是否是可读的等。

	/*
	 * If non-zero, should contain the offset from the start of css to
	 * a struct cgroup_file field.  cgroup will record the handle of
	 * the created file into it.  The recorded handle can be used as
	 * long as the containing css remains accessible.
	 */
	unsigned int file_offset;//如果该值非零，表示从 cgroup_subsys_state 结构体开始的偏移量，指向该文件类型的某个字段

	/*
	 * Fields used for internal bookkeeping.  Initialized automatically
	 * during registration.
	 */
	struct cgroup_subsys *ss;//指向对应的 cgroup_subsys 结构体。如果是 cgroup 核心文件（即不属于任何子系统的文件），该字段为 NULL。
	struct list_head node;//链表节点，用于将该 cftype 连接到其对应的 cgroup_subsys 结构体的 cfts 链表中。
	struct kernfs_ops *kf_ops;//指向 kernfs_ops 的指针，定义了与文件系统相关的操作合集。

	int (*open)(struct kernfs_open_file *of);//用于打开该控制文件的回调函数
	void (*release)(struct kernfs_open_file *of);//释放该控制文件时调用的回调函数。

	/*
	 * read_u64() is a shortcut for the common case of returning a
	 * single integer. Use it in place of read()
	 */
	u64 (*read_u64)(struct cgroup_subsys_state *css, struct cftype *cft);//读取 u64 类型的数据，通常用于返回单一整数值的场景。
	/*
	 * read_s64() is a signed version of read_u64()
	 */
	s64 (*read_s64)(struct cgroup_subsys_state *css, struct cftype *cft);//读取带符号的 s64 类型的数据。

	/* generic seq_file read interface */
	int (*seq_show)(struct seq_file *sf, void *v);//读取文件的通用回调函数，使用 seq_file API 显示内容。

	/* 用于序列化读取操作的回调函数，这些函数用于处理 seq_file 的遍历。可选操作，全部执行或不执行 */
	void *(*seq_start)(struct seq_file *sf, loff_t *ppos);//指定读取文件时的起始回调函数,初始化需要的资源或状态
	void *(*seq_next)(struct seq_file *sf, void *v, loff_t *ppos);//指定文件中下一项进程的获取回调函数,遍历控制文件的进程列表并在读取文件内容时，返回下一个进程。
	void (*seq_stop)(struct seq_file *sf, void *v);//指定显示文件内容的回调函数

	/*
	 * write_u64() is a shortcut for the common case of accepting
	 * a single integer (as parsed by simple_strtoull) from
	 * userspace. Use in place of write(); return 0 or error.
	 */
	int (*write_u64)(struct cgroup_subsys_state *css, struct cftype *cft,
			 u64 val);//写入一个 u64 类型的数据，通常用于接收单一整数值。
	/*
	 * write_s64() is a signed version of write_u64()
	 */
	int (*write_s64)(struct cgroup_subsys_state *css, struct cftype *cft,
			 s64 val);//写入带符号的 s64 类型的数据。

	/*
	 * write() is the generic write callback which maps directly to
	 * kernfs write operation and overrides all other operations.
	 * Maximum write size is determined by ->max_write_len.  Use
	 * of_css/cft() to access the associated css and cft.
	 */
	ssize_t (*write)(struct kernfs_open_file *of,
			 char *buf, size_t nbytes, loff_t off);//通用的写操作函数，直接映射到 kernfs 的写操作，最大写入大小由 max_write_len 确定。

	__poll_t (*poll)(struct kernfs_open_file *of,
			 struct poll_table_struct *pt);//文件的轮询操作，用于处理文件的事件监听。

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lock_class_key	lockdep_key;//用于调试目的，帮助追踪锁的依赖关系，在 CONFIG_DEBUG_LOCK_ALLOC 配置启用时才会使用。
#endif
};

/*
 * Control Group subsystem type.
 * See Documentation/admin-guide/cgroup-v1/cgroups.rst for details
 */
struct cgroup_subsys {//表示一个 cgroup 子系统的各种操作和属性
	struct cgroup_subsys_state *(*css_alloc)(struct cgroup_subsys_state *parent_css);//为 cgroup 子系统分配一个 cgroup_subsys_state 结构体，该结构体表示与该子系统相关的状态。
	int (*css_online)(struct cgroup_subsys_state *css);//将 cgroup 子系统的状态设为在线，通常在 cgroup 子系统初始化时调用。
	void (*css_offline)(struct cgroup_subsys_state *css);//将 cgroup 子系统的状态设为离线，通常在子系统销毁时调用。
	void (*css_released)(struct cgroup_subsys_state *css);//在 cgroup 子系统被释放时调用，通常用于清理与该子系统相关的资源。
	void (*css_free)(struct cgroup_subsys_state *css);// 释放与该子系统相关的资源。
	void (*css_reset)(struct cgroup_subsys_state *css);//重置 cgroup 子系统的状态。
	void (*css_rstat_flush)(struct cgroup_subsys_state *css, int cpu);// 用于刷新 cgroup 子系统的资源统计信息。
	int (*css_extra_stat_show)(struct seq_file *seq,
				   struct cgroup_subsys_state *css);//显示额外的统计信息，通常用于 seq_file 输出。
	int (*css_local_stat_show)(struct seq_file *seq,
				   struct cgroup_subsys_state *css);//显示与该 cgroup 子系统相关的本地统计信息。
	/*与任务集（taskset）和进程管理相关的函数指针*/
	int (*can_attach)(struct cgroup_taskset *tset);// 判断是否可以将任务集（taskset）附加到该 cgroup 子系统中。
	void (*cancel_attach)(struct cgroup_taskset *tset);//取消任务集的附加。
	void (*attach)(struct cgroup_taskset *tset);//将任务集附加到该 cgroup 子系统。
	void (*post_attach)(void);// 任务附加完成后的操作。
	int (*can_fork)(struct task_struct *task,
			struct css_set *cset);//判断一个任务是否可以被添加到该 cgroup 子系统。
	void (*cancel_fork)(struct task_struct *task, struct css_set *cset);//取消一个任务的 fork 操作。
	void (*fork)(struct task_struct *task);//处理任务的 fork 操作。
	void (*exit)(struct task_struct *task);//任务退出时的操作。
	void (*release)(struct task_struct *task);//释放任务在 cgroup 中占用的资源。
	void (*bind)(struct cgroup_subsys_state *root_css);//将 cgroup 子系统的根子系统（root_css）与该子系统绑定。

	bool early_init:1;//如果为 true，表示该控制器是早期初始化的。

	/*
	 * If %true, the controller, on the default hierarchy, doesn't show
	 * up in "cgroup.controllers" or "cgroup.subtree_control", is
	 * implicitly enabled on all cgroups on the default hierarchy, and
	 * bypasses the "no internal process" constraint.  This is for
	 * utility type controllers which is transparent to userland.
	 *
	 * An implicit controller can be stolen from the default hierarchy
	 * anytime and thus must be okay with offline csses from previous
	 * hierarchies coexisting with csses for the current one.
	 */
	bool implicit_on_dfl:1;//如果为 true，表示在默认层次结构中该控制器不会出现在 cgroup.controllers 或 cgroup.subtree_control 文件中，它在默认层次结构上是隐式启用的。

	/*
	 * If %true, the controller, supports threaded mode on the default
	 * hierarchy.  In a threaded subtree, both process granularity and
	 * no-internal-process constraint are ignored and a threaded
	 * controllers should be able to handle that.
	 *
	 * Note that as an implicit controller is automatically enabled on
	 * all cgroups on the default hierarchy, it should also be
	 * threaded.  implicit && !threaded is not supported.
	 */
	bool threaded:1;//如果为 true，表示该控制器支持线程模式。在线程模式下，进程粒度和无内核进程的约束会被忽略。

	/* the following two fields are initialized automatically during boot */
	int id;//cgroup 子系统的标识符。
	const char *name;//cgroup 子系统的名称。

	/* optional, initialized automatically during boot if not set */
	const char *legacy_name;//如果未设置，会自动初始化为 name，表示该子系统在 cgroup v1 中的名称。

	/* link to parent, protected by cgroup_lock() */
	struct cgroup_root *root;//指向 cgroup_root 的指针，表示该子系统的根节点，通常由 cgroup_lock() 来保护。

	/* idr for css->id */
	struct idr css_idr;//idr（ID 分配器）用于管理该子系统状态的 ID。

	/*
	 * List of cftypes.  Each entry is the first entry of an array
	 * terminated by zero length name.
	 */
	struct list_head cfts;//控制类型（cftypes）的链表，用于表示该子系统支持的所有控制文件类型。

	/*
	 * Base cftypes which are automatically registered.  The two can
	 * point to the same array.
	 * dfl_cftypes 和 legacy_cftypes: 默认和旧的 cgroup 控制文件类型数组，分别用于默认层次结构和遗留层次结构。
	 */
	struct cftype *dfl_cftypes;	/* for the default hierarchy */
	struct cftype *legacy_cftypes;	/* for the legacy hierarchies */

	/*
	 * A subsystem may depend on other subsystems.  When such subsystem
	 * is enabled on a cgroup, the depended-upon subsystems are enabled
	 * together if available.  Subsystems enabled due to dependency are
	 * not visible to userland until explicitly enabled.  The following
	 * specifies the mask of subsystems that this one depends on.
	 */
	unsigned int depends_on;//该子系统依赖的其他子系统的掩码，表示该子系统启动时需要哪些其他子系统一起启用。
};

extern struct percpu_rw_semaphore cgroup_threadgroup_rwsem;

/**
 * cgroup_threadgroup_change_begin - threadgroup exclusion for cgroups
 * @tsk: target task
 *
 * Allows cgroup operations to synchronize against threadgroup changes
 * using a percpu_rw_semaphore.
 */
static inline void cgroup_threadgroup_change_begin(struct task_struct *tsk)
{
	percpu_down_read(&cgroup_threadgroup_rwsem);
}

/**
 * cgroup_threadgroup_change_end - threadgroup exclusion for cgroups
 * @tsk: target task
 *
 * Counterpart of cgroup_threadcgroup_change_begin().
 */
static inline void cgroup_threadgroup_change_end(struct task_struct *tsk)
{
	percpu_up_read(&cgroup_threadgroup_rwsem);
}

#else	/* CONFIG_CGROUPS */

#define CGROUP_SUBSYS_COUNT 0

static inline void cgroup_threadgroup_change_begin(struct task_struct *tsk)
{
	might_sleep();
}

static inline void cgroup_threadgroup_change_end(struct task_struct *tsk) {}

#endif	/* CONFIG_CGROUPS */

#ifdef CONFIG_SOCK_CGROUP_DATA

/*
 * sock_cgroup_data is embedded at sock->sk_cgrp_data and contains
 * per-socket cgroup information except for memcg association.
 *
 * On legacy hierarchies, net_prio and net_cls controllers directly
 * set attributes on each sock which can then be tested by the network
 * layer. On the default hierarchy, each sock is associated with the
 * cgroup it was created in and the networking layer can match the
 * cgroup directly.
 */
struct sock_cgroup_data {
	struct cgroup	*cgroup; /* v2 */
#ifdef CONFIG_CGROUP_NET_CLASSID
	u32		classid; /* v1 */
#endif
#ifdef CONFIG_CGROUP_NET_PRIO
	u16		prioidx; /* v1 */
#endif
};

static inline u16 sock_cgroup_prioidx(const struct sock_cgroup_data *skcd)
{
#ifdef CONFIG_CGROUP_NET_PRIO
	return READ_ONCE(skcd->prioidx);
#else
	return 1;
#endif
}

static inline u32 sock_cgroup_classid(const struct sock_cgroup_data *skcd)
{
#ifdef CONFIG_CGROUP_NET_CLASSID
	return READ_ONCE(skcd->classid);
#else
	return 0;
#endif
}

static inline void sock_cgroup_set_prioidx(struct sock_cgroup_data *skcd,
					   u16 prioidx)
{
#ifdef CONFIG_CGROUP_NET_PRIO
	WRITE_ONCE(skcd->prioidx, prioidx);
#endif
}

static inline void sock_cgroup_set_classid(struct sock_cgroup_data *skcd,
					   u32 classid)
{
#ifdef CONFIG_CGROUP_NET_CLASSID
	WRITE_ONCE(skcd->classid, classid);
#endif
}

#else	/* CONFIG_SOCK_CGROUP_DATA */

struct sock_cgroup_data {
};

#endif	/* CONFIG_SOCK_CGROUP_DATA */

#endif	/* _LINUX_CGROUP_DEFS_H */
