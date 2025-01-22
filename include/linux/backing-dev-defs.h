/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_BACKING_DEV_DEFS_H
#define __LINUX_BACKING_DEV_DEFS_H

#include <linux/list.h>
#include <linux/radix-tree.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/percpu_counter.h>
#include <linux/percpu-refcount.h>
#include <linux/flex_proportions.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/kref.h>
#include <linux/refcount.h>

struct page;
struct device;
struct dentry;

/*
 * Bits in bdi_writeback.state
 */
enum wb_state {
	WB_registered,		/* bdi_register() was done */
	WB_writeback_running,	/* Writeback is in progress */
	WB_has_dirty_io,	/* Dirty inodes on ->b_{dirty|io|more_io} */
	WB_start_all,		/* nr_pages == 0 (all) work pending */
};

enum wb_stat_item {
	WB_RECLAIMABLE,
	WB_WRITEBACK,
	WB_DIRTIED,
	WB_WRITTEN,
	NR_WB_STAT_ITEMS
};

#define WB_STAT_BATCH (8*(1+ilog2(nr_cpu_ids)))

/*
 * why some writeback work was initiated
 */
enum wb_reason {
	WB_REASON_BACKGROUND,
	WB_REASON_VMSCAN,
	WB_REASON_SYNC,
	WB_REASON_PERIODIC,
	WB_REASON_LAPTOP_TIMER,
	WB_REASON_FS_FREE_SPACE,
	/*
	 * There is no bdi forker thread any more and works are done
	 * by emergency worker, however, this is TPs userland visible
	 * and we'll be exposing exactly the same information,
	 * so it has a mismatch name.
	 */
	WB_REASON_FORKER_THREAD,
	WB_REASON_FOREIGN_FLUSH,

	WB_REASON_MAX,
};

struct wb_completion {
	atomic_t		cnt;
	wait_queue_head_t	*waitq;
};

#define __WB_COMPLETION_INIT(_waitq)	\
	(struct wb_completion){ .cnt = ATOMIC_INIT(1), .waitq = (_waitq) }

/*
 * If one wants to wait for one or more wb_writeback_works, each work's
 * ->done should be set to a wb_completion defined using the following
 * macro.  Once all work items are issued with wb_queue_work(), the caller
 * can wait for the completion of all using wb_wait_for_completion().  Work
 * items which are waited upon aren't freed automatically on completion.
 */
#define WB_COMPLETION_INIT(bdi)		__WB_COMPLETION_INIT(&(bdi)->wb_waitq)

#define DEFINE_WB_COMPLETION(cmpl, bdi)	\
	struct wb_completion cmpl = WB_COMPLETION_INIT(bdi)

/*
 * Each wb (bdi_writeback) can perform writeback operations, is measured
 * and throttled, independently.  Without cgroup writeback, each bdi
 * (bdi_writeback) is served by its embedded bdi->wb.
 *
 * On the default hierarchy, blkcg implicitly enables memcg.  This allows
 * using memcg's page ownership for attributing writeback IOs, and every
 * memcg - blkcg combination can be served by its own wb by assigning a
 * dedicated wb to each memcg, which enables isolation across different
 * cgroups and propagation of IO back pressure down from the IO layer upto
 * the tasks which are generating the dirty pages to be written back.
 *
 * A cgroup wb is indexed on its bdi by the ID of the associated memcg,
 * refcounted with the number of inodes attached to it, and pins the memcg
 * and the corresponding blkcg.  As the corresponding blkcg for a memcg may
 * change as blkcg is disabled and enabled higher up in the hierarchy, a wb
 * is tested for blkcg after lookup and removed from index on mismatch so
 * that a new wb for the combination can be created.
 *
 * Each bdi_writeback that is not embedded into the backing_dev_info must hold
 * a reference to the parent backing_dev_info.  See cgwb_create() for details.
 */
struct bdi_writeback {
	struct backing_dev_info *bdi;	/* our parent bdi */

	unsigned long state;		/* Always use atomic bitops on this */
	unsigned long last_old_flush;	/* last old data flush */

	struct list_head b_dirty;	/* dirty inodes */
	struct list_head b_io;		/* parked for writeback */
	struct list_head b_more_io;	/* parked for more writeback */
	struct list_head b_dirty_time;	/* time stamps are dirty */
	spinlock_t list_lock;		/* protects the b_* lists */

	atomic_t writeback_inodes;	/* number of inodes under writeback */
	struct percpu_counter stat[NR_WB_STAT_ITEMS];

	unsigned long bw_time_stamp;	/* last time write bw is updated */
	unsigned long dirtied_stamp;
	unsigned long written_stamp;	/* pages written at bw_time_stamp */
	unsigned long write_bandwidth;	/* the estimated write bandwidth */
	unsigned long avg_write_bandwidth; /* further smoothed write bw, > 0 */

	/*
	 * The base dirty throttle rate, re-calculated on every 200ms.
	 * All the bdi tasks' dirty rate will be curbed under it.
	 * @dirty_ratelimit tracks the estimated @balanced_dirty_ratelimit
	 * in small steps and is much more smooth/stable than the latter.
	 */
	unsigned long dirty_ratelimit;
	unsigned long balanced_dirty_ratelimit;

	struct fprop_local_percpu completions;
	int dirty_exceeded;
	enum wb_reason start_all_reason;

	spinlock_t work_lock;		/* protects work_list & dwork scheduling */
	struct list_head work_list;
	struct delayed_work dwork;	/* work item used for writeback */
	struct delayed_work bw_dwork;	/* work item used for bandwidth estimate */

	struct list_head bdi_node;	/* anchored at bdi->wb_list */

#ifdef CONFIG_CGROUP_WRITEBACK
	struct percpu_ref refcnt;	/* used only for !root wb's */
	struct fprop_local_percpu memcg_completions;
	struct cgroup_subsys_state *memcg_css; /* the associated memcg */
	struct cgroup_subsys_state *blkcg_css; /* and blkcg */
	struct list_head memcg_node;	/* anchored at memcg->cgwb_list */
	struct list_head blkcg_node;	/* anchored at blkcg->cgwb_list */
	struct list_head b_attached;	/* attached inodes, protected by list_lock */
	struct list_head offline_node;	/* anchored at offline_cgwbs */

	union {
		struct work_struct release_work;
		struct rcu_head rcu;
	};
#endif
};

struct backing_dev_info {//用于管理后台设备信息的核心结构体,提供了内核对后台设备（如块设备）写回和管理的支持
	u64 id;// 唯一标识后台设备信息的 ID
	struct rb_node rb_node; /* 红黑树节点，用于通过ID组织和索引 */
	struct list_head bdi_list;	//链表头，用于链接所有后台设备信息实例
	unsigned long ra_pages;	/* 最大预读大小，以 PAGE_SIZE 为单位 */
	unsigned long io_pages;	/* 最大允许的 I/O 大小 */

	struct kref refcnt;	/* 结构体的引用计数器，用于内存管理 */
	unsigned int capabilities; /* 设备的功能标志 */
	unsigned int min_ratio;//最小使用比例
	unsigned int max_ratio, max_prop_frac;//最大使用比例和最大属性分数

	/*
	 * Sum of avg_write_bw of wbs with dirty inodes.  > 0 if there are
	 * any dirty wbs, which is depended upon by bdi_has_dirty().
	 * 包含带有脏 inode 的写回结构的平均写入带宽总和。如果有脏数据，
	 * bdi_has_dirty() 将依赖此值返回 > 0。
	 */
	atomic_long_t tot_write_bandwidth;
	/*
	 * Jiffies when last process was dirty throttled on this bdi. Used by
	 * blk-wbt.
	 * 记录最后一个进程在该 bdi 上因脏数据限制而被阻塞的时间（以 jiffies 为单位）。
	 * 由 blk-wbt 使用。
	 */
	unsigned long last_bdp_sleep;

	struct bdi_writeback wb;  /* 根写回信息，用于该 bdi 的写回操作 */
	struct list_head wb_list; /* 所有写回结构的链表 */
#ifdef CONFIG_CGROUP_WRITEBACK
	struct radix_tree_root cgwb_tree; /* 活跃 cgroup 写回结构的基数树 */
	struct mutex cgwb_release_mutex;  /* 保护写回结构关闭时的互斥锁 */
	struct rw_semaphore wb_switch_rwsem; /* 在同步期间禁止 cgroup 写回切换 */
#endif
	wait_queue_head_t wb_waitq;//等待队列头，用于写回任务等待

	struct device *dev;//指向相关设备的指针
	char dev_name[64];//设备名称字符串
	struct device *owner;//设备的拥有者，可能是父设备

	struct timer_list laptop_mode_wb_timer;//定时器，用于笔记本模式的写回调度

#ifdef CONFIG_DEBUG_FS
	struct dentry *debug_dir;//用于 debugfs 的目录节点
#endif
};

struct wb_lock_cookie {
	bool locked;
	unsigned long flags;
};

#ifdef CONFIG_CGROUP_WRITEBACK

/**
 * wb_tryget - try to increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline bool wb_tryget(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		return percpu_ref_tryget(&wb->refcnt);
	return true;
}

/**
 * wb_get - increment a wb's refcount
 * @wb: bdi_writeback to get
 */
static inline void wb_get(struct bdi_writeback *wb)
{
	if (wb != &wb->bdi->wb)
		percpu_ref_get(&wb->refcnt);
}

/**
 * wb_put - decrement a wb's refcount
 * @wb: bdi_writeback to put
 * @nr: number of references to put
 */
static inline void wb_put_many(struct bdi_writeback *wb, unsigned long nr)
{
	if (WARN_ON_ONCE(!wb->bdi)) {
		/*
		 * A driver bug might cause a file to be removed before bdi was
		 * initialized.
		 */
		return;
	}

	if (wb != &wb->bdi->wb)
		percpu_ref_put_many(&wb->refcnt, nr);
}

/**
 * wb_put - decrement a wb's refcount
 * @wb: bdi_writeback to put
 */
static inline void wb_put(struct bdi_writeback *wb)
{
	wb_put_many(wb, 1);
}

/**
 * wb_dying - is a wb dying?
 * @wb: bdi_writeback of interest
 *
 * Returns whether @wb is unlinked and being drained.
 */
static inline bool wb_dying(struct bdi_writeback *wb)
{
	return percpu_ref_is_dying(&wb->refcnt);
}

#else	/* CONFIG_CGROUP_WRITEBACK */

static inline bool wb_tryget(struct bdi_writeback *wb)
{
	return true;
}

static inline void wb_get(struct bdi_writeback *wb)
{
}

static inline void wb_put(struct bdi_writeback *wb)
{
}

static inline void wb_put_many(struct bdi_writeback *wb, unsigned long nr)
{
}

static inline bool wb_dying(struct bdi_writeback *wb)
{
	return false;
}

#endif	/* CONFIG_CGROUP_WRITEBACK */

#endif	/* __LINUX_BACKING_DEV_DEFS_H */
