/* SPDX-License-Identifier: GPL-2.0 */
/*
 * generic net pointers
 */

#ifndef __NET_GENERIC_H__
#define __NET_GENERIC_H__

#include <linux/bug.h>
#include <linux/rcupdate.h>
#include <net/net_namespace.h>

/*
 * Generic net pointers are to be used by modules to put some private
 * stuff on the struct net without explicit struct net modification
 *
 * The rules are simple:
 * 1. set pernet_operations->id.  After register_pernet_device you
 *    will have the id of your private pointer.
 * 2. set pernet_operations->size to have the code allocate and free
 *    a private structure pointed to from struct net.
 * 3. do not change this pointer while the net is alive;
 * 4. do not try to have any private reference on the net_generic object.
 *
 * After accomplishing all of the above, the private pointer can be
 * accessed with the net_generic() call.
 */

struct net_generic {//一个网络相关的通用结构体。
	union {
		struct {
			unsigned int len;//存储长度信息
			struct rcu_head rcu;//关联的 RCU 头部，用于RCU（Read-Copy-Update）机制
		} s;

		DECLARE_FLEX_ARRAY(void *, ptr);//声明一个灵活数组，可以容纳指针
	};
};

static inline void *net_generic(const struct net *net, unsigned int id)
{
	struct net_generic *ng;
	void *ptr;

	rcu_read_lock();
	ng = rcu_dereference(net->gen);
	ptr = ng->ptr[id];
	rcu_read_unlock();

	return ptr;
}
#endif
