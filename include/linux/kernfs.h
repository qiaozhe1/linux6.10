/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * kernfs.h - pseudo filesystem decoupled from vfs locking
 */

#ifndef __LINUX_KERNFS_H
#define __LINUX_KERNFS_H

#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/idr.h>
#include <linux/lockdep.h>
#include <linux/rbtree.h>
#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/uidgid.h>
#include <linux/wait.h>
#include <linux/rwsem.h>
#include <linux/cache.h>

struct file;
struct dentry;
struct iattr;
struct seq_file;
struct vm_area_struct;
struct vm_operations_struct;
struct super_block;
struct file_system_type;
struct poll_table_struct;
struct fs_context;

struct kernfs_fs_context;
struct kernfs_open_node;
struct kernfs_iattrs;

/*
 * NR_KERNFS_LOCK_BITS determines size (NR_KERNFS_LOCKS) of hash
 * table of locks.
 * Having a small hash table would impact scalability, since
 * more and more kernfs_node objects will end up using same lock
 * and having a very large hash table would waste memory.
 *
 * At the moment size of hash table of locks is being set based on
 * the number of CPUs as follows:
 *
 * NR_CPU      NR_KERNFS_LOCK_BITS      NR_KERNFS_LOCKS
 *   1                  1                       2
 *  2-3                 2                       4
 *  4-7                 4                       16
 *  8-15                6                       64
 *  16-31               8                       256
 *  32 and more         10                      1024
 *
 * The above relation between NR_CPU and number of locks is based
 * on some internal experimentation which involved booting qemu
 * with different values of smp, performing some sysfs operations
 * on all CPUs and observing how increase in number of locks impacts
 * completion time of these sysfs operations on each CPU.
 */
#ifdef CONFIG_SMP
#define NR_KERNFS_LOCK_BITS (2 * (ilog2(NR_CPUS < 32 ? NR_CPUS : 32)))
#else
#define NR_KERNFS_LOCK_BITS     1
#endif

#define NR_KERNFS_LOCKS     (1 << NR_KERNFS_LOCK_BITS)

/*
 * There's one kernfs_open_file for each open file and one kernfs_open_node
 * for each kernfs_node with one or more open files.
 *
 * filp->private_data points to seq_file whose ->private points to
 * kernfs_open_file.
 *
 * kernfs_open_files are chained at kernfs_open_node->files, which is
 * protected by kernfs_global_locks.open_file_mutex[i].
 *
 * To reduce possible contention in sysfs access, arising due to single
 * locks, use an array of locks (e.g. open_file_mutex) and use kernfs_node
 * object address as hash keys to get the index of these locks.
 *
 * Hashed mutexes are safe to use here because operations using these don't
 * rely on global exclusion.
 *
 * In future we intend to replace other global locks with hashed ones as well.
 * kernfs_global_locks acts as a holder for all such hash tables.
 */
struct kernfs_global_locks {
	struct mutex open_file_mutex[NR_KERNFS_LOCKS];
};

enum kernfs_node_type {
	KERNFS_DIR		= 0x0001,
	KERNFS_FILE		= 0x0002,
	KERNFS_LINK		= 0x0004,
};

#define KERNFS_TYPE_MASK		0x000f
#define KERNFS_FLAG_MASK		~KERNFS_TYPE_MASK
#define KERNFS_MAX_USER_XATTRS		128
#define KERNFS_USER_XATTR_SIZE_LIMIT	(128 << 10)

enum kernfs_node_flag {
	KERNFS_ACTIVATED	= 0x0010,
	KERNFS_NS		= 0x0020,
	KERNFS_HAS_SEQ_SHOW	= 0x0040,
	KERNFS_HAS_MMAP		= 0x0080,
	KERNFS_LOCKDEP		= 0x0100,
	KERNFS_HIDDEN		= 0x0200,
	KERNFS_SUICIDAL		= 0x0400,
	KERNFS_SUICIDED		= 0x0800,
	KERNFS_EMPTY_DIR	= 0x1000,
	KERNFS_HAS_RELEASE	= 0x2000,
	KERNFS_REMOVING		= 0x4000,
};

/* @flags for kernfs_create_root() */
enum kernfs_root_flag {
	/*
	 * kernfs_nodes are created in the deactivated state and invisible.
	 * They require explicit kernfs_activate() to become visible.  This
	 * can be used to make related nodes become visible atomically
	 * after all nodes are created successfully.
	 */
	KERNFS_ROOT_CREATE_DEACTIVATED		= 0x0001,

	/*
	 * For regular files, if the opener has CAP_DAC_OVERRIDE, open(2)
	 * succeeds regardless of the RW permissions.  sysfs had an extra
	 * layer of enforcement where open(2) fails with -EACCES regardless
	 * of CAP_DAC_OVERRIDE if the permission doesn't have the
	 * respective read or write access at all (none of S_IRUGO or
	 * S_IWUGO) or the respective operation isn't implemented.  The
	 * following flag enables that behavior.
	 */
	KERNFS_ROOT_EXTRA_OPEN_PERM_CHECK	= 0x0002,

	/*
	 * The filesystem supports exportfs operation, so userspace can use
	 * fhandle to access nodes of the fs.
	 */
	KERNFS_ROOT_SUPPORT_EXPORTOP		= 0x0004,

	/*
	 * Support user xattrs to be written to nodes rooted at this root.
	 */
	KERNFS_ROOT_SUPPORT_USER_XATTR		= 0x0008,
};

/* type-specific structures for kernfs_node union members */
struct kernfs_elem_dir {
	unsigned long		subdirs;
	/* children rbtree starts here and goes through kn->rb */
	struct rb_root		children;

	/*
	 * The kernfs hierarchy this directory belongs to.  This fits
	 * better directly in kernfs_node but is here to save space.
	 */
	struct kernfs_root	*root;
	/*
	 * Monotonic revision counter, used to identify if a directory
	 * node has changed during negative dentry revalidation.
	 */
	unsigned long		rev;
};

struct kernfs_elem_symlink {
	struct kernfs_node	*target_kn;
};

struct kernfs_elem_attr {
	const struct kernfs_ops	*ops;
	struct kernfs_open_node __rcu	*open;
	loff_t			size;
	struct kernfs_node	*notify_next;	/* for kernfs_notify() */
};

/*
 * kernfs_node - the building block of kernfs hierarchy.  Each and every
 * kernfs node is represented by single kernfs_node.  Most fields are
 * private to kernfs and shouldn't be accessed directly by kernfs users.
 *
 * As long as count reference is held, the kernfs_node itself is
 * accessible.  Dereferencing elem or any other outer entity requires
 * active reference.
 */
struct kernfs_node {//用于表示 kernfs 文件系统中的一个节点，包含了该节点的多种信息和操作属性
	atomic_t		count;//跟踪对该节点的引用计数，确保在节点被使用时不会被释放。
	atomic_t		active;//表示该节点当前是否处于活动状态，有助于管理节点的生命周期。
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;//在启用锁依赖跟踪的情况下，提供调试信息，便于分析锁的使用情况。
#endif
	/*
	 * Use kernfs_get_parent() and kernfs_name/path() instead of
	 * accessing the following two fields directly.  If the node is
	 * never moved to a different parent, it is safe to access the
	 * parent directly.
	 */
	struct kernfs_node	*parent;//指向该节点的父节点，构成树形结构。
	const char		*name;//节点的名称，作为标识符。

	struct rb_node		rb;//红黑树节点，用于在 kernfs 中进行高效的节点管理和查找。

	const void		*ns;//命名空间标签，帮助标识节点所属的命名空间。
	unsigned int		hash;//基于命名空间和名称的哈希值，加快查找速度。
	unsigned short		flags;//节点的状态标志，用于表示节点的不同属性。
	umode_t			mode;//表示节点的权限和类型，类似于文件系统中的访问控制。

	union {//联合体，表示节点可以是目录、符号链接或属性
		struct kernfs_elem_dir		dir;//目录类型的节点
		struct kernfs_elem_symlink	symlink;//符号链接类型的节点
		struct kernfs_elem_attr		attr;// 属性类型的节点
	};

	/*
	 * 64bit unique ID.  On 64bit ino setups, id is the ino.  On 32bit,
	 * the low 32bits are ino and upper generation.
	 */
	u64			id;//唯一 ID，用于标识该节点，在 64 位系统中表示 inode，32 位系统中包含生成信息

	void			*priv;// 用户自定义数据指针，允许附加用户数据到节点上
	struct kernfs_iattrs	*iattr;//指向节点的 inode 属性结构，存储与 inode 相关的额外信息

	struct rcu_head		rcu;//RCU（Read-Copy Update）机制的头部结构，支持并发访问和延迟释放
};

/*
 * kernfs_syscall_ops may be specified on kernfs_create_root() to support
 * syscalls.  These optional callbacks are invoked on the matching syscalls
 * and can perform any kernfs operations which don't necessarily have to be
 * the exact operation requested.  An active reference is held for each
 * kernfs_node parameter.
 */
struct kernfs_syscall_ops {
	int (*show_options)(struct seq_file *sf, struct kernfs_root *root);

	int (*mkdir)(struct kernfs_node *parent, const char *name,
		     umode_t mode);
	int (*rmdir)(struct kernfs_node *kn);
	int (*rename)(struct kernfs_node *kn, struct kernfs_node *new_parent,
		      const char *new_name);
	int (*show_path)(struct seq_file *sf, struct kernfs_node *kn,
			 struct kernfs_root *root);
};

struct kernfs_node *kernfs_root_to_node(struct kernfs_root *root);

struct kernfs_open_file {
	/* published fields */
	struct kernfs_node	*kn;
	struct file		*file;
	struct seq_file		*seq_file;
	void			*priv;

	/* private fields, do not use outside kernfs proper */
	struct mutex		mutex;
	struct mutex		prealloc_mutex;
	int			event;
	struct list_head	list;
	char			*prealloc_buf;

	size_t			atomic_write_len;
	bool			mmapped:1;
	bool			released:1;
	const struct vm_operations_struct *vm_ops;
};

struct kernfs_ops {// kernfs 文件系统的操作结构体，用于定义与 kernfs 文件交互的各种操作，包括打开、释放、读写、mmap、poll 等
	/*
	 * Optional open/release methods.  Both are called with
	 * @of->seq_file populated.
	 */
	int (*open)(struct kernfs_open_file *of);//用于打开文件的操作，通常在访问文件之前调用
	void (*release)(struct kernfs_open_file *of);// 用于释放文件资源的操作，一般在文件关闭时调用。

	/*
	 * Read is handled by either seq_file or raw_read().
	 *
	 * If seq_show() is present, seq_file path is active.  Other seq
	 * operations are optional and if not implemented, the behavior is
	 * equivalent to single_open().  @sf->private points to the
	 * associated kernfs_open_file.
	 *
	 * read() is bounced through kernel buffer and a read larger than
	 * PAGE_SIZE results in partial operation of PAGE_SIZE.
	 */
	int (*seq_show)(struct seq_file *sf, void *v);//用于序列化显示文件内容。如果启用了 seq_file 路径，该函数会显示内容。

	void *(*seq_start)(struct seq_file *sf, loff_t *ppos);//开始读取序列化数据的方法，返回数据的起始位置
	void *(*seq_next)(struct seq_file *sf, void *v, loff_t *ppos);//获取序列化数据中的下一个数据项
	void (*seq_stop)(struct seq_file *sf, void *v);//停止读取序列化数据的方法

	ssize_t (*read)(struct kernfs_open_file *of, char *buf, size_t bytes,
			loff_t off);//读取数据的方法，接受 kernfs_open_file 指针、缓冲区、读取字节数和偏移量

	/*
	 * write() is bounced through kernel buffer.  If atomic_write_len
	 * is not set, a write larger than PAGE_SIZE results in partial
	 * operations of PAGE_SIZE chunks.  If atomic_write_len is set,
	 * writes upto the specified size are executed atomically but
	 * larger ones are rejected with -E2BIG.
	 */
	size_t atomic_write_len;//原子写入长度限制，表示可以原子写入的最大字节数，超过该值会被拒绝
	/*
	 * "prealloc" causes a buffer to be allocated at open for
	 * all read/write requests.  As ->seq_show uses seq_read()
	 * which does its own allocation, it is incompatible with
	 * ->prealloc.  Provide ->read and ->write with ->prealloc.
	 */
	bool prealloc;// 是否预分配缓冲区，预分配后所有读取/写入请求都会使用已分配的缓冲区
	ssize_t (*write)(struct kernfs_open_file *of, char *buf, size_t bytes,
			 loff_t off);//写入数据的方法，接受 kernfs_open_file 指针、数据缓冲区、字节数和偏移量

	__poll_t (*poll)(struct kernfs_open_file *of,
			 struct poll_table_struct *pt);// 用于文件的 poll 操作，接受 kernfs_open_file 指针和 poll 表

	int (*mmap)(struct kernfs_open_file *of, struct vm_area_struct *vma);//mmap 操作方法，用于文件映射到内存
	loff_t (*llseek)(struct kernfs_open_file *of, loff_t offset, int whence);//llseek 操作，用于文件偏移量的修改
};

/*
 * The kernfs superblock creation/mount parameter context.
 */
struct kernfs_fs_context {
	struct kernfs_root	*root;		/* Root of the hierarchy being mounted */
	void			*ns_tag;	/* Namespace tag of the mount (or NULL) */
	unsigned long		magic;		/* File system specific magic number */

	/* The following are set/used by kernfs_mount() */
	bool			new_sb_created;	/* Set to T if we allocated a new sb */
};

#ifdef CONFIG_KERNFS

static inline enum kernfs_node_type kernfs_type(struct kernfs_node *kn)
{
	return kn->flags & KERNFS_TYPE_MASK;
}

static inline ino_t kernfs_id_ino(u64 id)
{
	/* id is ino if ino_t is 64bit; otherwise, low 32bits */
	if (sizeof(ino_t) >= sizeof(u64))
		return id;
	else
		return (u32)id;
}

static inline u32 kernfs_id_gen(u64 id)
{
	/* gen is fixed at 1 if ino_t is 64bit; otherwise, high 32bits */
	if (sizeof(ino_t) >= sizeof(u64))
		return 1;
	else
		return id >> 32;
}

static inline ino_t kernfs_ino(struct kernfs_node *kn)
{
	return kernfs_id_ino(kn->id);
}

static inline ino_t kernfs_gen(struct kernfs_node *kn)
{
	return kernfs_id_gen(kn->id);
}

/**
 * kernfs_enable_ns - enable namespace under a directory
 * @kn: directory of interest, should be empty
 *
 * This is to be called right after @kn is created to enable namespace
 * under it.  All children of @kn must have non-NULL namespace tags and
 * only the ones which match the super_block's tag will be visible.
 */
static inline void kernfs_enable_ns(struct kernfs_node *kn)
{
	WARN_ON_ONCE(kernfs_type(kn) != KERNFS_DIR);
	WARN_ON_ONCE(!RB_EMPTY_ROOT(&kn->dir.children));
	kn->flags |= KERNFS_NS;
}

/**
 * kernfs_ns_enabled - test whether namespace is enabled
 * @kn: the node to test
 *
 * Test whether namespace filtering is enabled for the children of @ns.
 */
static inline bool kernfs_ns_enabled(struct kernfs_node *kn)
{
	return kn->flags & KERNFS_NS;
}

int kernfs_name(struct kernfs_node *kn, char *buf, size_t buflen);
int kernfs_path_from_node(struct kernfs_node *root_kn, struct kernfs_node *kn,
			  char *buf, size_t buflen);
void pr_cont_kernfs_name(struct kernfs_node *kn);
void pr_cont_kernfs_path(struct kernfs_node *kn);
struct kernfs_node *kernfs_get_parent(struct kernfs_node *kn);
struct kernfs_node *kernfs_find_and_get_ns(struct kernfs_node *parent,
					   const char *name, const void *ns);
struct kernfs_node *kernfs_walk_and_get_ns(struct kernfs_node *parent,
					   const char *path, const void *ns);
void kernfs_get(struct kernfs_node *kn);
void kernfs_put(struct kernfs_node *kn);

struct kernfs_node *kernfs_node_from_dentry(struct dentry *dentry);
struct kernfs_root *kernfs_root_from_sb(struct super_block *sb);
struct inode *kernfs_get_inode(struct super_block *sb, struct kernfs_node *kn);

struct dentry *kernfs_node_dentry(struct kernfs_node *kn,
				  struct super_block *sb);
struct kernfs_root *kernfs_create_root(struct kernfs_syscall_ops *scops,
				       unsigned int flags, void *priv);
void kernfs_destroy_root(struct kernfs_root *root);

struct kernfs_node *kernfs_create_dir_ns(struct kernfs_node *parent,
					 const char *name, umode_t mode,
					 kuid_t uid, kgid_t gid,
					 void *priv, const void *ns);
struct kernfs_node *kernfs_create_empty_dir(struct kernfs_node *parent,
					    const char *name);
struct kernfs_node *__kernfs_create_file(struct kernfs_node *parent,
					 const char *name, umode_t mode,
					 kuid_t uid, kgid_t gid,
					 loff_t size,
					 const struct kernfs_ops *ops,
					 void *priv, const void *ns,
					 struct lock_class_key *key);
struct kernfs_node *kernfs_create_link(struct kernfs_node *parent,
				       const char *name,
				       struct kernfs_node *target);
void kernfs_activate(struct kernfs_node *kn);
void kernfs_show(struct kernfs_node *kn, bool show);
void kernfs_remove(struct kernfs_node *kn);
void kernfs_break_active_protection(struct kernfs_node *kn);
void kernfs_unbreak_active_protection(struct kernfs_node *kn);
bool kernfs_remove_self(struct kernfs_node *kn);
int kernfs_remove_by_name_ns(struct kernfs_node *parent, const char *name,
			     const void *ns);
int kernfs_rename_ns(struct kernfs_node *kn, struct kernfs_node *new_parent,
		     const char *new_name, const void *new_ns);
int kernfs_setattr(struct kernfs_node *kn, const struct iattr *iattr);
__poll_t kernfs_generic_poll(struct kernfs_open_file *of,
			     struct poll_table_struct *pt);
void kernfs_notify(struct kernfs_node *kn);

int kernfs_xattr_get(struct kernfs_node *kn, const char *name,
		     void *value, size_t size);
int kernfs_xattr_set(struct kernfs_node *kn, const char *name,
		     const void *value, size_t size, int flags);

const void *kernfs_super_ns(struct super_block *sb);
int kernfs_get_tree(struct fs_context *fc);
void kernfs_free_fs_context(struct fs_context *fc);
void kernfs_kill_sb(struct super_block *sb);

void kernfs_init(void);

struct kernfs_node *kernfs_find_and_get_node_by_id(struct kernfs_root *root,
						   u64 id);
#else	/* CONFIG_KERNFS */

static inline enum kernfs_node_type kernfs_type(struct kernfs_node *kn)
{ return 0; }	/* whatever */

static inline void kernfs_enable_ns(struct kernfs_node *kn) { }

static inline bool kernfs_ns_enabled(struct kernfs_node *kn)
{ return false; }

static inline int kernfs_name(struct kernfs_node *kn, char *buf, size_t buflen)
{ return -ENOSYS; }

static inline int kernfs_path_from_node(struct kernfs_node *root_kn,
					struct kernfs_node *kn,
					char *buf, size_t buflen)
{ return -ENOSYS; }

static inline void pr_cont_kernfs_name(struct kernfs_node *kn) { }
static inline void pr_cont_kernfs_path(struct kernfs_node *kn) { }

static inline struct kernfs_node *kernfs_get_parent(struct kernfs_node *kn)
{ return NULL; }

static inline struct kernfs_node *
kernfs_find_and_get_ns(struct kernfs_node *parent, const char *name,
		       const void *ns)
{ return NULL; }
static inline struct kernfs_node *
kernfs_walk_and_get_ns(struct kernfs_node *parent, const char *path,
		       const void *ns)
{ return NULL; }

static inline void kernfs_get(struct kernfs_node *kn) { }
static inline void kernfs_put(struct kernfs_node *kn) { }

static inline struct kernfs_node *kernfs_node_from_dentry(struct dentry *dentry)
{ return NULL; }

static inline struct kernfs_root *kernfs_root_from_sb(struct super_block *sb)
{ return NULL; }

static inline struct inode *
kernfs_get_inode(struct super_block *sb, struct kernfs_node *kn)
{ return NULL; }

static inline struct kernfs_root *
kernfs_create_root(struct kernfs_syscall_ops *scops, unsigned int flags,
		   void *priv)
{ return ERR_PTR(-ENOSYS); }

static inline void kernfs_destroy_root(struct kernfs_root *root) { }

static inline struct kernfs_node *
kernfs_create_dir_ns(struct kernfs_node *parent, const char *name,
		     umode_t mode, kuid_t uid, kgid_t gid,
		     void *priv, const void *ns)
{ return ERR_PTR(-ENOSYS); }

static inline struct kernfs_node *
__kernfs_create_file(struct kernfs_node *parent, const char *name,
		     umode_t mode, kuid_t uid, kgid_t gid,
		     loff_t size, const struct kernfs_ops *ops,
		     void *priv, const void *ns, struct lock_class_key *key)
{ return ERR_PTR(-ENOSYS); }

static inline struct kernfs_node *
kernfs_create_link(struct kernfs_node *parent, const char *name,
		   struct kernfs_node *target)
{ return ERR_PTR(-ENOSYS); }

static inline void kernfs_activate(struct kernfs_node *kn) { }

static inline void kernfs_remove(struct kernfs_node *kn) { }

static inline bool kernfs_remove_self(struct kernfs_node *kn)
{ return false; }

static inline int kernfs_remove_by_name_ns(struct kernfs_node *kn,
					   const char *name, const void *ns)
{ return -ENOSYS; }

static inline int kernfs_rename_ns(struct kernfs_node *kn,
				   struct kernfs_node *new_parent,
				   const char *new_name, const void *new_ns)
{ return -ENOSYS; }

static inline int kernfs_setattr(struct kernfs_node *kn,
				 const struct iattr *iattr)
{ return -ENOSYS; }

static inline __poll_t kernfs_generic_poll(struct kernfs_open_file *of,
					   struct poll_table_struct *pt)
{ return -ENOSYS; }

static inline void kernfs_notify(struct kernfs_node *kn) { }

static inline int kernfs_xattr_get(struct kernfs_node *kn, const char *name,
				   void *value, size_t size)
{ return -ENOSYS; }

static inline int kernfs_xattr_set(struct kernfs_node *kn, const char *name,
				   const void *value, size_t size, int flags)
{ return -ENOSYS; }

static inline const void *kernfs_super_ns(struct super_block *sb)
{ return NULL; }

static inline int kernfs_get_tree(struct fs_context *fc)
{ return -ENOSYS; }

static inline void kernfs_free_fs_context(struct fs_context *fc) { }

static inline void kernfs_kill_sb(struct super_block *sb) { }

static inline void kernfs_init(void) { }

#endif	/* CONFIG_KERNFS */

/**
 * kernfs_path - build full path of a given node
 * @kn: kernfs_node of interest
 * @buf: buffer to copy @kn's name into
 * @buflen: size of @buf
 *
 * If @kn is NULL result will be "(null)".
 *
 * Returns the length of the full path.  If the full length is equal to or
 * greater than @buflen, @buf contains the truncated path with the trailing
 * '\0'.  On error, -errno is returned.
 */
static inline int kernfs_path(struct kernfs_node *kn, char *buf, size_t buflen)
{
	return kernfs_path_from_node(kn, NULL, buf, buflen);
}

static inline struct kernfs_node *
kernfs_find_and_get(struct kernfs_node *kn, const char *name)
{
	return kernfs_find_and_get_ns(kn, name, NULL);
}

static inline struct kernfs_node *
kernfs_walk_and_get(struct kernfs_node *kn, const char *path)
{
	return kernfs_walk_and_get_ns(kn, path, NULL);
}

static inline struct kernfs_node *
kernfs_create_dir(struct kernfs_node *parent, const char *name, umode_t mode,
		  void *priv)
{
	return kernfs_create_dir_ns(parent, name, mode,
				    GLOBAL_ROOT_UID, GLOBAL_ROOT_GID,
				    priv, NULL);
}

static inline int kernfs_remove_by_name(struct kernfs_node *parent,
					const char *name)
{
	return kernfs_remove_by_name_ns(parent, name, NULL);
}

static inline int kernfs_rename(struct kernfs_node *kn,
				struct kernfs_node *new_parent,
				const char *new_name)
{
	return kernfs_rename_ns(kn, new_parent, new_name, NULL);
}

#endif	/* __LINUX_KERNFS_H */
