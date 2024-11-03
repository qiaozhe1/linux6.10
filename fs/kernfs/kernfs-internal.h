/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * fs/kernfs/kernfs-internal.h - kernfs internal header file
 *
 * Copyright (c) 2001-3 Patrick Mochel
 * Copyright (c) 2007 SUSE Linux Products GmbH
 * Copyright (c) 2007, 2013 Tejun Heo <teheo@suse.de>
 */

#ifndef __KERNFS_INTERNAL_H
#define __KERNFS_INTERNAL_H

#include <linux/lockdep.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/xattr.h>

#include <linux/kernfs.h>
#include <linux/fs_context.h>

struct kernfs_iattrs {//用于存储与 kernfs 节点相关的 inode 属性
	kuid_t			ia_uid;//该节点的用户 ID，表明节点归属的用户
	kgid_t			ia_gid;//该节点的组 ID，表示节点所属的用户组
	struct timespec64	ia_atime;//最后访问时间，记录用户上次访问该节点的时间。
	struct timespec64	ia_mtime;//最后修改时间，记录节点内容被修改的时间。
	struct timespec64	ia_ctime;//最后状态改变时间，指节点元数据最后更改的时间（如权限变化）。

	struct simple_xattrs	xattrs;//用于存储简单扩展属性的结构，允许附加元数据（如 ACL、SELinux 标签等）到节点上。
	atomic_t		nr_user_xattrs;//跟踪附加的用户扩展属性数量，确保有效管理和使用这些属性
	atomic_t		user_xattr_size;//记录所有用户扩展属性的总大小，帮助内存分配和限制控制。
};

struct kernfs_root {//定义 kernfs_root 结构体，用于表示 kernfs 文件系统的根节点
	/* published fields 公开字段*/
	struct kernfs_node	*kn;// 指向该根节点对应的 kernfs 节点
	unsigned int		flags;//根节点的标志，用于指示不同的属性和行为

	/* private fields, do not use outside kernfs proper 私有字段*/
	struct idr		ino_idr;//用于管理 inode ID 的 IDR 结构，映射 inode 到 kernfs 节点
	u32			last_id_lowbits;//记录上一个使用的 inode ID 的低位部分
	u32			id_highbits;// 记录 inode ID 的高位部分，用于处理 32 位系统的 inode
	struct kernfs_syscall_ops *syscall_ops;//指向与该根节点相关的系统调用操作结构体

	/* list of kernfs_super_info of this root, protected by kernfs_rwsem 超级块信息*/
	struct list_head	supers;//存储与该根节点相关的超级块信息的链表，受 rw 信号量保护

	/* 等待队列和信号量 */
	wait_queue_head_t	deactivate_waitq;//用于处理节点停用时的等待队列
	struct rw_semaphore	kernfs_rwsem;//保护对根节点的读写访问的信号量
	struct rw_semaphore	kernfs_iattr_rwsem;//保护对 inode 属性的读写访问的信号量
	struct rw_semaphore	kernfs_supers_rwsem;//保护超级块访问的读写信号量

	/*RCU 机制*/
	struct rcu_head		rcu;//用于 RCU（Read-Copy Update）机制的头部结构，支持并发访问和延迟释放
};

/* +1 to avoid triggering overflow warning when negating it */
#define KN_DEACTIVATED_BIAS		(INT_MIN + 1)

/* KERNFS_TYPE_MASK and types are defined in include/linux/kernfs.h */

/**
 * kernfs_root - find out the kernfs_root a kernfs_node belongs to
 * @kn: kernfs_node of interest
 *
 * Return: the kernfs_root @kn belongs to.
 */
static inline struct kernfs_root *kernfs_root(struct kernfs_node *kn)
{
	/* if parent exists, it's always a dir; otherwise, @sd is a dir */
	if (kn->parent)
		kn = kn->parent;
	return kn->dir.root;
}

/*
 * mount.c
 */
struct kernfs_super_info {
	struct super_block	*sb;

	/*
	 * The root associated with this super_block.  Each super_block is
	 * identified by the root and ns it's associated with.
	 */
	struct kernfs_root	*root;

	/*
	 * Each sb is associated with one namespace tag, currently the
	 * network namespace of the task which mounted this kernfs
	 * instance.  If multiple tags become necessary, make the following
	 * an array and compare kernfs_node tag against every entry.
	 */
	const void		*ns;

	/* anchored at kernfs_root->supers, protected by kernfs_rwsem */
	struct list_head	node;
};
#define kernfs_info(SB) ((struct kernfs_super_info *)(SB->s_fs_info))

static inline struct kernfs_node *kernfs_dentry_node(struct dentry *dentry)
{
	if (d_really_is_negative(dentry))
		return NULL;
	return d_inode(dentry)->i_private;
}

static inline void kernfs_set_rev(struct kernfs_node *parent,
				  struct dentry *dentry)
{
	dentry->d_time = parent->dir.rev;
}

static inline void kernfs_inc_rev(struct kernfs_node *parent)
{
	parent->dir.rev++;
}

static inline bool kernfs_dir_changed(struct kernfs_node *parent,
				      struct dentry *dentry)
{
	if (parent->dir.rev != dentry->d_time)
		return true;
	return false;
}

extern const struct super_operations kernfs_sops;
extern struct kmem_cache *kernfs_node_cache, *kernfs_iattrs_cache;

/*
 * inode.c
 */
extern const struct xattr_handler * const kernfs_xattr_handlers[];
void kernfs_evict_inode(struct inode *inode);
int kernfs_iop_permission(struct mnt_idmap *idmap,
			  struct inode *inode, int mask);
int kernfs_iop_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		       struct iattr *iattr);
int kernfs_iop_getattr(struct mnt_idmap *idmap,
		       const struct path *path, struct kstat *stat,
		       u32 request_mask, unsigned int query_flags);
ssize_t kernfs_iop_listxattr(struct dentry *dentry, char *buf, size_t size);
int __kernfs_setattr(struct kernfs_node *kn, const struct iattr *iattr);

/*
 * dir.c
 */
extern const struct dentry_operations kernfs_dops;
extern const struct file_operations kernfs_dir_fops;
extern const struct inode_operations kernfs_dir_iops;

struct kernfs_node *kernfs_get_active(struct kernfs_node *kn);
void kernfs_put_active(struct kernfs_node *kn);
int kernfs_add_one(struct kernfs_node *kn);
struct kernfs_node *kernfs_new_node(struct kernfs_node *parent,
				    const char *name, umode_t mode,
				    kuid_t uid, kgid_t gid,
				    unsigned flags);

/*
 * file.c
 */
extern const struct file_operations kernfs_file_fops;

bool kernfs_should_drain_open_files(struct kernfs_node *kn);
void kernfs_drain_open_files(struct kernfs_node *kn);

/*
 * symlink.c
 */
extern const struct inode_operations kernfs_symlink_iops;

/*
 * kernfs locks
 */
extern struct kernfs_global_locks *kernfs_locks;
#endif	/* __KERNFS_INTERNAL_H */
