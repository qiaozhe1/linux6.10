/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/poll.h>
#include <linux/ns_common.h>
#include <linux/fs_pin.h>

struct mnt_namespace {
	struct ns_common	ns;
	struct mount *	root;
	struct rb_root		mounts; /* Protected by namespace_sem */
	struct user_namespace	*user_ns;
	struct ucounts		*ucounts;
	u64			seq;	/* Sequence number to prevent loops */
	wait_queue_head_t poll;
	u64 event;
	unsigned int		nr_mounts; /* # of mounts in the namespace */
	unsigned int		pending_mounts;
} __randomize_layout;

struct mnt_pcp {
	int mnt_count;
	int mnt_writers;
};

struct mountpoint {
	struct hlist_node m_hash;
	struct dentry *m_dentry;
	struct hlist_head m_list;
	int m_count;
};

struct mount {//定义挂载结构体，表示文件系统的挂载信息
	struct hlist_node mnt_hash;		//哈希链表节点，用于挂载哈希表
	struct mount *mnt_parent;		//指向父挂载的指针
	struct dentry *mnt_mountpoint;		//挂载点的目录项指针
	struct vfsmount mnt;			//关联的虚拟文件系统挂载结构
	union {					//联合体，用于不同上下文的内存管理
		struct rcu_head mnt_rcu;	//RCU（Read-Copy-Update）头部，用于延迟释放
		struct llist_node mnt_llist;	//线性链表节点，用于特殊挂载场景
	};
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;	//每CPU私有数据，用于多处理器环境
#else
	int mnt_count;				//挂载计数，用于引用计数
	int mnt_writers;			//当前写入者计数
#endif
	struct list_head mnt_mounts;		//子挂载的列表锚点
	struct list_head mnt_child;		//子挂载通过此链表进行连接
	struct list_head mnt_instance;		//在超级块的挂载列表中的挂载实例
	const char *mnt_devname;		//设备名称，例如 /dev/dsk/hda1
	union {					//联合体，包含用于不同数据结构的节点
		struct rb_node mnt_node;	//红黑树节点，用于命名空间的挂载列表
		struct list_head mnt_list;	//链表节点，用于其他挂载场景
	};
	struct list_head mnt_expire;		//在文件系统特定的过期列表中的链接
	struct list_head mnt_share;		//共享挂载的循环链表
	struct list_head mnt_slave_list;	//从挂载列表
	struct list_head mnt_slave;		//从挂载列表的条目
	struct mount *mnt_master;		//指向主挂载的指针
	struct mnt_namespace *mnt_ns;		//包含此挂载的命名空间
	struct mountpoint *mnt_mp;		//指向挂载点的结构
	union {					//联合体，用于挂载点的管理
		struct hlist_node mnt_mp_list;	//拥有相同挂载点的挂载列表
		struct hlist_node mnt_umount;	//卸载的哈希链表节点
	};
	struct list_head mnt_umounting; 	//卸载传播的列表条目
#ifdef CONFIG_FSNOTIFY
	struct fsnotify_mark_connector __rcu *mnt_fsnotify_marks;	// FSNotify标记连接器
	__u32 mnt_fsnotify_mask;		//FSNotify掩码
#endif
	int mnt_id;				// 挂载标识符，重用
	u64 mnt_id_unique;			//唯一的挂载ID，直到重启
	int mnt_group_id;			//同类组的标识符
	int mnt_expiry_mark;			//如果标记为过期则为真
	struct hlist_head mnt_pins;		//挂载锁定列表
	struct hlist_head mnt_stuck_children;	//挂载被阻塞的子挂载列表
} __randomize_layout;

#define MNT_NS_INTERNAL ERR_PTR(-EINVAL) /* distinct from any mnt_namespace */

static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static inline int mnt_has_parent(struct mount *mnt)
{
	return mnt != mnt->mnt_parent;
}

static inline int is_mounted(struct vfsmount *mnt)
{
	/* neither detached nor internal? */
	return !IS_ERR_OR_NULL(real_mount(mnt)->mnt_ns);
}

extern struct mount *__lookup_mnt(struct vfsmount *, struct dentry *);

extern int __legitimize_mnt(struct vfsmount *, unsigned);

static inline bool __path_is_mountpoint(const struct path *path)
{
	struct mount *m = __lookup_mnt(path->mnt, path->dentry);
	return m && likely(!(m->mnt.mnt_flags & MNT_SYNC_UMOUNT));
}

extern void __detach_mounts(struct dentry *dentry);

static inline void detach_mounts(struct dentry *dentry)
{
	if (!d_mountpoint(dentry))
		return;
	__detach_mounts(dentry);
}

static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	refcount_inc(&ns->ns.count);
}

extern seqlock_t mount_lock;

struct proc_mounts {
	struct mnt_namespace *ns;
	struct path root;
	int (*show)(struct seq_file *, struct vfsmount *);
};

extern const struct seq_operations mounts_op;

extern bool __is_local_mountpoint(struct dentry *dentry);
static inline bool is_local_mountpoint(struct dentry *dentry)
{
	if (!d_mountpoint(dentry))
		return false;

	return __is_local_mountpoint(dentry);
}

static inline bool is_anon_ns(struct mnt_namespace *ns)
{
	return ns->seq == 0;
}

static inline void move_from_ns(struct mount *mnt, struct list_head *dt_list)
{
	WARN_ON(!(mnt->mnt.mnt_flags & MNT_ONRB));
	mnt->mnt.mnt_flags &= ~MNT_ONRB;
	rb_erase(&mnt->mnt_node, &mnt->mnt_ns->mounts);
	list_add_tail(&mnt->mnt_list, dt_list);
}

extern void mnt_cursor_del(struct mnt_namespace *ns, struct mount *cursor);
