// SPDX-License-Identifier: GPL-2.0
/*
 * padata.c - generic interface to process data streams in parallel
 *
 * See Documentation/core-api/padata.rst for more information.
 *
 * Copyright (C) 2008, 2009 secunet Security Networks AG
 * Copyright (C) 2008, 2009 Steffen Klassert <steffen.klassert@secunet.com>
 *
 * Copyright (c) 2020 Oracle and/or its affiliates.
 * Author: Daniel Jordan <daniel.m.jordan@oracle.com>
 */

#include <linux/completion.h>
#include <linux/export.h>
#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/padata.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/rcupdate.h>

#define	PADATA_WORK_ONSTACK	1	/* Work's memory is on stack */

struct padata_work {
	struct work_struct	pw_work;
	struct list_head	pw_list;  /* padata_free_works linkage */
	void			*pw_data;
};

static DEFINE_SPINLOCK(padata_works_lock);
static struct padata_work *padata_works;
static LIST_HEAD(padata_free_works);

struct padata_mt_job_state {//状态管理器，用于协调和管理多线程任务的执行
	spinlock_t		lock;// 自旋锁，用于同步对任务状态的并发访问
	struct completion	completion;//完成量，用于协调所有线程的执行完成
	struct padata_mt_job	*job;//指向描述多线程任务的结构体指针
	int			nworks;//需要执行的工作单元数
	int			nworks_fini;//已经完成的工作单元数
	unsigned long		chunk_size;//每个线程处理的任务块大小
};

static void padata_free_pd(struct parallel_data *pd);
static void __init padata_mt_helper(struct work_struct *work);

static int padata_index_to_cpu(struct parallel_data *pd, int cpu_index)
{
	int cpu, target_cpu;

	target_cpu = cpumask_first(pd->cpumask.pcpu);
	for (cpu = 0; cpu < cpu_index; cpu++)
		target_cpu = cpumask_next(target_cpu, pd->cpumask.pcpu);

	return target_cpu;
}

static int padata_cpu_hash(struct parallel_data *pd, unsigned int seq_nr)
{
	/*
	 * Hash the sequence numbers to the cpus by taking
	 * seq_nr mod. number of cpus in use.
	 */
	int cpu_index = seq_nr % cpumask_weight(pd->cpumask.pcpu);

	return padata_index_to_cpu(pd, cpu_index);
}

static struct padata_work *padata_work_alloc(void)
{
	struct padata_work *pw;

	lockdep_assert_held(&padata_works_lock);//检查 `padata_works_lock` 是否被持有，确保调用该函数时已加锁

	if (list_empty(&padata_free_works))//检查 `padata_free_works` 列表是否为空
		return NULL;	//如果列表为空，返回 `NULL`，表示没有可用的 `padata_work`

	pw = list_first_entry(&padata_free_works, struct padata_work, pw_list);//获取 `padata_free_works` 列表的第一个 `padata_work` 条目
	list_del(&pw->pw_list);//从 `padata_free_works` 列表中删除该条目
	return pw;
}

/*
 * This function is marked __ref because this function may be optimized in such
 * a way that it directly refers to work_fn's address, which causes modpost to
 * complain when work_fn is marked __init. This scenario was observed with clang
 * LTO, where padata_work_init() was optimized to refer directly to
 * padata_mt_helper() because the calls to padata_work_init() with other work_fn
 * values were eliminated or inlined.
 */
static void __ref padata_work_init(struct padata_work *pw, work_func_t work_fn,
				   void *data, int flags)
{
	if (flags & PADATA_WORK_ONSTACK)
		INIT_WORK_ONSTACK(&pw->pw_work, work_fn);
	else
		INIT_WORK(&pw->pw_work, work_fn);
	pw->pw_data = data;
}

/*多线程任务分配函数*/
static int __init padata_work_alloc_mt(int nworks, void *data,
				       struct list_head *head)
{
	int i;

	spin_lock_bh(&padata_works_lock);//关闭软中断，并获取锁，保护 `padata_works` 列表的并发访问
	/* Start at 1 because the current task participates in the job. */
	for (i = 1; i < nworks; ++i) {//遍历需要分配的工作结构体数量
		struct padata_work *pw = padata_work_alloc();//分配新的 `padata_work` 结构体

		if (!pw)
			break;
		padata_work_init(pw, padata_mt_helper, data, 0);//初始化 `padata_work`，绑定处理函数和数据
		list_add(&pw->pw_list, head);//将工作项添加到任务列表 `head`
	}
	spin_unlock_bh(&padata_works_lock);//释放锁，恢复软中断状态

	return i;
}

static void padata_work_free(struct padata_work *pw)
{
	lockdep_assert_held(&padata_works_lock);
	list_add(&pw->pw_list, &padata_free_works);
}

static void __init padata_works_free(struct list_head *works)
{
	struct padata_work *cur, *next;

	if (list_empty(works))
		return;

	spin_lock_bh(&padata_works_lock);
	list_for_each_entry_safe(cur, next, works, pw_list) {
		list_del(&cur->pw_list);
		padata_work_free(cur);
	}
	spin_unlock_bh(&padata_works_lock);
}

static void padata_parallel_worker(struct work_struct *parallel_work)
{
	struct padata_work *pw = container_of(parallel_work, struct padata_work,
					      pw_work);
	struct padata_priv *padata = pw->pw_data;

	local_bh_disable();
	padata->parallel(padata);
	spin_lock(&padata_works_lock);
	padata_work_free(pw);
	spin_unlock(&padata_works_lock);
	local_bh_enable();
}

/**
 * padata_do_parallel - padata parallelization function
 *
 * @ps: padatashell
 * @padata: object to be parallelized
 * @cb_cpu: pointer to the CPU that the serialization callback function should
 *          run on.  If it's not in the serial cpumask of @pinst
 *          (i.e. cpumask.cbcpu), this function selects a fallback CPU and if
 *          none found, returns -EINVAL.
 *
 * The parallelization callback function will run with BHs off.
 * Note: Every object which is parallelized by padata_do_parallel
 * must be seen by padata_do_serial.
 *
 * Return: 0 on success or else negative error code.
 */
int padata_do_parallel(struct padata_shell *ps,
		       struct padata_priv *padata, int *cb_cpu)
{
	struct padata_instance *pinst = ps->pinst;
	int i, cpu, cpu_index, err;
	struct parallel_data *pd;
	struct padata_work *pw;

	rcu_read_lock_bh();

	pd = rcu_dereference_bh(ps->pd);

	err = -EINVAL;
	if (!(pinst->flags & PADATA_INIT) || pinst->flags & PADATA_INVALID)
		goto out;

	if (!cpumask_test_cpu(*cb_cpu, pd->cpumask.cbcpu)) {
		if (cpumask_empty(pd->cpumask.cbcpu))
			goto out;

		/* Select an alternate fallback CPU and notify the caller. */
		cpu_index = *cb_cpu % cpumask_weight(pd->cpumask.cbcpu);

		cpu = cpumask_first(pd->cpumask.cbcpu);
		for (i = 0; i < cpu_index; i++)
			cpu = cpumask_next(cpu, pd->cpumask.cbcpu);

		*cb_cpu = cpu;
	}

	err = -EBUSY;
	if ((pinst->flags & PADATA_RESET))
		goto out;

	refcount_inc(&pd->refcnt);
	padata->pd = pd;
	padata->cb_cpu = *cb_cpu;

	spin_lock(&padata_works_lock);
	padata->seq_nr = ++pd->seq_nr;
	pw = padata_work_alloc();
	spin_unlock(&padata_works_lock);

	if (!pw) {
		/* Maximum works limit exceeded, run in the current task. */
		padata->parallel(padata);
	}

	rcu_read_unlock_bh();

	if (pw) {
		padata_work_init(pw, padata_parallel_worker, padata, 0);
		queue_work(pinst->parallel_wq, &pw->pw_work);
	}

	return 0;
out:
	rcu_read_unlock_bh();

	return err;
}
EXPORT_SYMBOL(padata_do_parallel);

/*
 * padata_find_next - Find the next object that needs serialization.
 *
 * Return:
 * * A pointer to the control struct of the next object that needs
 *   serialization, if present in one of the percpu reorder queues.
 * * NULL, if the next object that needs serialization will
 *   be parallel processed by another cpu and is not yet present in
 *   the cpu's reorder queue.
 */
static struct padata_priv *padata_find_next(struct parallel_data *pd,
					    bool remove_object)
{
	struct padata_priv *padata;
	struct padata_list *reorder;
	int cpu = pd->cpu;

	reorder = per_cpu_ptr(pd->reorder_list, cpu);

	spin_lock(&reorder->lock);
	if (list_empty(&reorder->list)) {
		spin_unlock(&reorder->lock);
		return NULL;
	}

	padata = list_entry(reorder->list.next, struct padata_priv, list);

	/*
	 * Checks the rare case where two or more parallel jobs have hashed to
	 * the same CPU and one of the later ones finishes first.
	 */
	if (padata->seq_nr != pd->processed) {
		spin_unlock(&reorder->lock);
		return NULL;
	}

	if (remove_object) {
		list_del_init(&padata->list);
		++pd->processed;
		pd->cpu = cpumask_next_wrap(cpu, pd->cpumask.pcpu, -1, false);
	}

	spin_unlock(&reorder->lock);
	return padata;
}

static void padata_reorder(struct parallel_data *pd)
{
	struct padata_instance *pinst = pd->ps->pinst;
	int cb_cpu;
	struct padata_priv *padata;
	struct padata_serial_queue *squeue;
	struct padata_list *reorder;

	/*
	 * We need to ensure that only one cpu can work on dequeueing of
	 * the reorder queue the time. Calculating in which percpu reorder
	 * queue the next object will arrive takes some time. A spinlock
	 * would be highly contended. Also it is not clear in which order
	 * the objects arrive to the reorder queues. So a cpu could wait to
	 * get the lock just to notice that there is nothing to do at the
	 * moment. Therefore we use a trylock and let the holder of the lock
	 * care for all the objects enqueued during the holdtime of the lock.
	 */
	if (!spin_trylock_bh(&pd->lock))
		return;

	while (1) {
		padata = padata_find_next(pd, true);

		/*
		 * If the next object that needs serialization is parallel
		 * processed by another cpu and is still on it's way to the
		 * cpu's reorder queue, nothing to do for now.
		 */
		if (!padata)
			break;

		cb_cpu = padata->cb_cpu;
		squeue = per_cpu_ptr(pd->squeue, cb_cpu);

		spin_lock(&squeue->serial.lock);
		list_add_tail(&padata->list, &squeue->serial.list);
		spin_unlock(&squeue->serial.lock);

		queue_work_on(cb_cpu, pinst->serial_wq, &squeue->work);
	}

	spin_unlock_bh(&pd->lock);

	/*
	 * The next object that needs serialization might have arrived to
	 * the reorder queues in the meantime.
	 *
	 * Ensure reorder queue is read after pd->lock is dropped so we see
	 * new objects from another task in padata_do_serial.  Pairs with
	 * smp_mb in padata_do_serial.
	 */
	smp_mb();

	reorder = per_cpu_ptr(pd->reorder_list, pd->cpu);
	if (!list_empty(&reorder->list) && padata_find_next(pd, false))
		queue_work(pinst->serial_wq, &pd->reorder_work);
}

static void invoke_padata_reorder(struct work_struct *work)
{
	struct parallel_data *pd;

	local_bh_disable();
	pd = container_of(work, struct parallel_data, reorder_work);
	padata_reorder(pd);
	local_bh_enable();
}

static void padata_serial_worker(struct work_struct *serial_work)
{
	struct padata_serial_queue *squeue;
	struct parallel_data *pd;
	LIST_HEAD(local_list);
	int cnt;

	local_bh_disable();
	squeue = container_of(serial_work, struct padata_serial_queue, work);
	pd = squeue->pd;

	spin_lock(&squeue->serial.lock);
	list_replace_init(&squeue->serial.list, &local_list);
	spin_unlock(&squeue->serial.lock);

	cnt = 0;

	while (!list_empty(&local_list)) {
		struct padata_priv *padata;

		padata = list_entry(local_list.next,
				    struct padata_priv, list);

		list_del_init(&padata->list);

		padata->serial(padata);
		cnt++;
	}
	local_bh_enable();

	if (refcount_sub_and_test(cnt, &pd->refcnt))
		padata_free_pd(pd);
}

/**
 * padata_do_serial - padata serialization function
 *
 * @padata: object to be serialized.
 *
 * padata_do_serial must be called for every parallelized object.
 * The serialization callback function will run with BHs off.
 */
void padata_do_serial(struct padata_priv *padata)
{
	struct parallel_data *pd = padata->pd;
	int hashed_cpu = padata_cpu_hash(pd, padata->seq_nr);
	struct padata_list *reorder = per_cpu_ptr(pd->reorder_list, hashed_cpu);
	struct padata_priv *cur;
	struct list_head *pos;

	spin_lock(&reorder->lock);
	/* Sort in ascending order of sequence number. */
	list_for_each_prev(pos, &reorder->list) {
		cur = list_entry(pos, struct padata_priv, list);
		if (cur->seq_nr < padata->seq_nr)
			break;
	}
	list_add(&padata->list, pos);
	spin_unlock(&reorder->lock);

	/*
	 * Ensure the addition to the reorder list is ordered correctly
	 * with the trylock of pd->lock in padata_reorder.  Pairs with smp_mb
	 * in padata_reorder.
	 */
	smp_mb();

	padata_reorder(pd);
}
EXPORT_SYMBOL(padata_do_serial);

static int padata_setup_cpumasks(struct padata_instance *pinst)
{
	struct workqueue_attrs *attrs;
	int err;

	attrs = alloc_workqueue_attrs();
	if (!attrs)
		return -ENOMEM;

	/* Restrict parallel_wq workers to pd->cpumask.pcpu. */
	cpumask_copy(attrs->cpumask, pinst->cpumask.pcpu);
	err = apply_workqueue_attrs(pinst->parallel_wq, attrs);
	free_workqueue_attrs(attrs);

	return err;
}

static void __init padata_mt_helper(struct work_struct *w)//多线程辅助任务执行函数
{
	struct padata_work *pw = container_of(w, struct padata_work, pw_work);//从 `work_struct` 获取 `padata_work` 结构体
	struct padata_mt_job_state *ps = pw->pw_data;//获取当前任务的状态结构
	struct padata_mt_job *job = ps->job;//获取多线程任务结构体
	bool done;//标志任务是否已全部完成

	spin_lock(&ps->lock);

	while (job->size > 0) {//如果还有未完成的任务块
		unsigned long start, size, end;//定义任务块的起始地址、大小和结束地址

		start = job->start;//记录任务块的起始地址
		/* 计算任务块的大小，使 `end` 位置对齐到 `chunk_size` 边界，
		 * 如果剩余任务不足一个块大小，则 `size` 为剩余任务量。 
		 */
		size = roundup(start + 1, ps->chunk_size) - start;//计算对齐后的块大小
		size = min(size, job->size);//确保任务块大小不超过剩余任务量
		end = start + size;//计算任务块的结束地址

		job->start = end;//更新任务的起始位置
		job->size -= size;//更新剩余任务的大小

		spin_unlock(&ps->lock);//解锁以允许其他线程访问共享数据
		job->thread_fn(start, end, job->fn_arg);//执行任务函数，处理 `start` 到 `end` 之间的数据
		spin_lock(&ps->lock);//重新加锁准备处理下一任务块
	}

	++ps->nworks_fini;//增加已完成的工作线程计数
	done = (ps->nworks_fini == ps->nworks);//检查是否所有任务线程都完成任务
	spin_unlock(&ps->lock);//

	if (done)//如果所有任务线程已完成任务
		complete(&ps->completion);//// 触发 `completion` 信号，通知主线程任务结束
}

/**
 * padata_do_multithreaded - run a multithreaded job
 * @job: Description of the job.
 *
 * See the definition of struct padata_mt_job for more details.
 */
void __init padata_do_multithreaded(struct padata_mt_job *job)//执行多线程任务的函数
{
	/* In case threads finish at different times. */
	static const unsigned long load_balance_factor = 4;//负载平衡因子，用于调整任务分配
	struct padata_work my_work, *pw;//padata_work 结构体，表示一个工作单元
	struct padata_mt_job_state ps;//用于管理多线程任务的状态
	LIST_HEAD(works);//初始化一个链表头，用于存储所有的工作单元
	int nworks, nid;//nworks 是线程数，nid 是 NUMA 节点 ID
	static atomic_t last_used_nid __initdata;//上次使用的 NUMA 节点 ID，使用原子变量存储

	if (job->size == 0)//如果任务的大小为 0，则直接返回
		return;

	/* Ensure at least one thread when size < min_chunk. */
	nworks = max(job->size / max(job->min_chunk, job->align), 1ul);//计算需要多少个线程来处理任务
	nworks = min(nworks, job->max_threads);// 确保线程数不超过最大允许的线程

	if (nworks == 1) {//如果只需要一个线程
		/* Single thread, no coordination needed, cut to the chase. */
		job->thread_fn(job->start, job->start + job->size, job->fn_arg);//直接在当前线程中执行任务
		return;
	}

	spin_lock_init(&ps.lock);//初始化自旋锁，用于线程同步
	init_completion(&ps.completion);//初始化完成量，用于等待所有线程完成
	ps.job	       = job;//设置多线程任务状态的job指针
	ps.nworks      = padata_work_alloc_mt(nworks, &ps, &works);//根据线程个数分配工作单元
	ps.nworks_fini = 0;//初始化已完成的工作单元计数


	/*
	 * Chunk size is the amount of work a helper does per call to the
	 * thread function.  Load balance large jobs between threads by
	 * increasing the number of chunks, guarantee at least the minimum
	 * chunk size from the caller, and honor the caller's alignment.
	 */
	ps.chunk_size = job->size / (ps.nworks * load_balance_factor);//计算每个线程的数据块大小
	ps.chunk_size = max(ps.chunk_size, job->min_chunk);//确保块大小不小于最小块大小
	ps.chunk_size = roundup(ps.chunk_size, job->align);//确保块大小符合对齐要求

	list_for_each_entry(pw, &works, pw_list)//遍历所有工作单元
		if (job->numa_aware) {//如果任务需要考虑NUMA
			int old_node = atomic_read(&last_used_nid);//读取上次使用的NUMA节点

			do {
				nid = next_node_in(old_node, node_states[N_CPU]);//获取下一个NUMA节点
			} while (!atomic_try_cmpxchg(&last_used_nid, &old_node, nid));//尝试更新last_used_nid
			queue_work_node(nid, system_unbound_wq, &pw->pw_work);//将工作单元分配到指定的NUMA 节点上执行
		} else {
			queue_work(system_unbound_wq, &pw->pw_work);//将工作单元提交到默认的工作队列中执行
		}

	/* Use the current thread, which saves starting a workqueue worker. */
	padata_work_init(&my_work, padata_mt_helper, &ps, PADATA_WORK_ONSTACK);//初始化当前线程的工作单元
	padata_mt_helper(&my_work.pw_work);//使用当前线程处理任务的一部分

	/* Wait for all the helpers to finish. */
	wait_for_completion(&ps.completion);//等待所有线程完成任务

	destroy_work_on_stack(&my_work.pw_work);//销毁当前线程的工作单元
	padata_works_free(&works);//释放所有工作单元
}

static void __padata_list_init(struct padata_list *pd_list)
{
	INIT_LIST_HEAD(&pd_list->list);
	spin_lock_init(&pd_list->lock);
}

/* Initialize all percpu queues used by serial workers */
static void padata_init_squeues(struct parallel_data *pd)
{
	int cpu;
	struct padata_serial_queue *squeue;

	for_each_cpu(cpu, pd->cpumask.cbcpu) {
		squeue = per_cpu_ptr(pd->squeue, cpu);
		squeue->pd = pd;
		__padata_list_init(&squeue->serial);
		INIT_WORK(&squeue->work, padata_serial_worker);
	}
}

/* Initialize per-CPU reorder lists */
static void padata_init_reorder_list(struct parallel_data *pd)
{
	int cpu;
	struct padata_list *list;

	for_each_cpu(cpu, pd->cpumask.pcpu) {
		list = per_cpu_ptr(pd->reorder_list, cpu);
		__padata_list_init(list);
	}
}

/* Allocate and initialize the internal cpumask dependend resources. */
static struct parallel_data *padata_alloc_pd(struct padata_shell *ps)
{
	struct padata_instance *pinst = ps->pinst;
	struct parallel_data *pd;

	pd = kzalloc(sizeof(struct parallel_data), GFP_KERNEL);
	if (!pd)
		goto err;

	pd->reorder_list = alloc_percpu(struct padata_list);
	if (!pd->reorder_list)
		goto err_free_pd;

	pd->squeue = alloc_percpu(struct padata_serial_queue);
	if (!pd->squeue)
		goto err_free_reorder_list;

	pd->ps = ps;

	if (!alloc_cpumask_var(&pd->cpumask.pcpu, GFP_KERNEL))
		goto err_free_squeue;
	if (!alloc_cpumask_var(&pd->cpumask.cbcpu, GFP_KERNEL))
		goto err_free_pcpu;

	cpumask_and(pd->cpumask.pcpu, pinst->cpumask.pcpu, cpu_online_mask);
	cpumask_and(pd->cpumask.cbcpu, pinst->cpumask.cbcpu, cpu_online_mask);

	padata_init_reorder_list(pd);
	padata_init_squeues(pd);
	pd->seq_nr = -1;
	refcount_set(&pd->refcnt, 1);
	spin_lock_init(&pd->lock);
	pd->cpu = cpumask_first(pd->cpumask.pcpu);
	INIT_WORK(&pd->reorder_work, invoke_padata_reorder);

	return pd;

err_free_pcpu:
	free_cpumask_var(pd->cpumask.pcpu);
err_free_squeue:
	free_percpu(pd->squeue);
err_free_reorder_list:
	free_percpu(pd->reorder_list);
err_free_pd:
	kfree(pd);
err:
	return NULL;
}

static void padata_free_pd(struct parallel_data *pd)
{
	free_cpumask_var(pd->cpumask.pcpu);
	free_cpumask_var(pd->cpumask.cbcpu);
	free_percpu(pd->reorder_list);
	free_percpu(pd->squeue);
	kfree(pd);
}

static void __padata_start(struct padata_instance *pinst)
{
	pinst->flags |= PADATA_INIT;
}

static void __padata_stop(struct padata_instance *pinst)
{
	if (!(pinst->flags & PADATA_INIT))
		return;

	pinst->flags &= ~PADATA_INIT;

	synchronize_rcu();
}

/* Replace the internal control structure with a new one. */
static int padata_replace_one(struct padata_shell *ps)
{
	struct parallel_data *pd_new;

	pd_new = padata_alloc_pd(ps);
	if (!pd_new)
		return -ENOMEM;

	ps->opd = rcu_dereference_protected(ps->pd, 1);
	rcu_assign_pointer(ps->pd, pd_new);

	return 0;
}

static int padata_replace(struct padata_instance *pinst)
{
	struct padata_shell *ps;
	int err = 0;

	pinst->flags |= PADATA_RESET;

	list_for_each_entry(ps, &pinst->pslist, list) {
		err = padata_replace_one(ps);
		if (err)
			break;
	}

	synchronize_rcu();

	list_for_each_entry_continue_reverse(ps, &pinst->pslist, list)
		if (refcount_dec_and_test(&ps->opd->refcnt))
			padata_free_pd(ps->opd);

	pinst->flags &= ~PADATA_RESET;

	return err;
}

/* If cpumask contains no active cpu, we mark the instance as invalid. */
static bool padata_validate_cpumask(struct padata_instance *pinst,
				    const struct cpumask *cpumask)
{
	if (!cpumask_intersects(cpumask, cpu_online_mask)) {
		pinst->flags |= PADATA_INVALID;
		return false;
	}

	pinst->flags &= ~PADATA_INVALID;
	return true;
}

static int __padata_set_cpumasks(struct padata_instance *pinst,
				 cpumask_var_t pcpumask,
				 cpumask_var_t cbcpumask)
{
	int valid;
	int err;

	valid = padata_validate_cpumask(pinst, pcpumask);
	if (!valid) {
		__padata_stop(pinst);
		goto out_replace;
	}

	valid = padata_validate_cpumask(pinst, cbcpumask);
	if (!valid)
		__padata_stop(pinst);

out_replace:
	cpumask_copy(pinst->cpumask.pcpu, pcpumask);
	cpumask_copy(pinst->cpumask.cbcpu, cbcpumask);

	err = padata_setup_cpumasks(pinst) ?: padata_replace(pinst);

	if (valid)
		__padata_start(pinst);

	return err;
}

/**
 * padata_set_cpumask - Sets specified by @cpumask_type cpumask to the value
 *                      equivalent to @cpumask.
 * @pinst: padata instance
 * @cpumask_type: PADATA_CPU_SERIAL or PADATA_CPU_PARALLEL corresponding
 *                to parallel and serial cpumasks respectively.
 * @cpumask: the cpumask to use
 *
 * Return: 0 on success or negative error code
 */
int padata_set_cpumask(struct padata_instance *pinst, int cpumask_type,
		       cpumask_var_t cpumask)
{
	struct cpumask *serial_mask, *parallel_mask;
	int err = -EINVAL;

	cpus_read_lock();
	mutex_lock(&pinst->lock);

	switch (cpumask_type) {
	case PADATA_CPU_PARALLEL:
		serial_mask = pinst->cpumask.cbcpu;
		parallel_mask = cpumask;
		break;
	case PADATA_CPU_SERIAL:
		parallel_mask = pinst->cpumask.pcpu;
		serial_mask = cpumask;
		break;
	default:
		 goto out;
	}

	err =  __padata_set_cpumasks(pinst, parallel_mask, serial_mask);

out:
	mutex_unlock(&pinst->lock);
	cpus_read_unlock();

	return err;
}
EXPORT_SYMBOL(padata_set_cpumask);

#ifdef CONFIG_HOTPLUG_CPU

static int __padata_add_cpu(struct padata_instance *pinst, int cpu)
{
	int err = 0;

	if (cpumask_test_cpu(cpu, cpu_online_mask)) {
		err = padata_replace(pinst);

		if (padata_validate_cpumask(pinst, pinst->cpumask.pcpu) &&
		    padata_validate_cpumask(pinst, pinst->cpumask.cbcpu))
			__padata_start(pinst);
	}

	return err;
}

static int __padata_remove_cpu(struct padata_instance *pinst, int cpu)
{
	int err = 0;

	if (!cpumask_test_cpu(cpu, cpu_online_mask)) {
		if (!padata_validate_cpumask(pinst, pinst->cpumask.pcpu) ||
		    !padata_validate_cpumask(pinst, pinst->cpumask.cbcpu))
			__padata_stop(pinst);

		err = padata_replace(pinst);
	}

	return err;
}

static inline int pinst_has_cpu(struct padata_instance *pinst, int cpu)
{
	return cpumask_test_cpu(cpu, pinst->cpumask.pcpu) ||
		cpumask_test_cpu(cpu, pinst->cpumask.cbcpu);
}

static int padata_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct padata_instance *pinst;
	int ret;

	pinst = hlist_entry_safe(node, struct padata_instance, cpu_online_node);
	if (!pinst_has_cpu(pinst, cpu))
		return 0;

	mutex_lock(&pinst->lock);
	ret = __padata_add_cpu(pinst, cpu);
	mutex_unlock(&pinst->lock);
	return ret;
}

static int padata_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct padata_instance *pinst;
	int ret;

	pinst = hlist_entry_safe(node, struct padata_instance, cpu_dead_node);
	if (!pinst_has_cpu(pinst, cpu))
		return 0;

	mutex_lock(&pinst->lock);
	ret = __padata_remove_cpu(pinst, cpu);
	mutex_unlock(&pinst->lock);
	return ret;
}

static enum cpuhp_state hp_online;
#endif

static void __padata_free(struct padata_instance *pinst)
{
#ifdef CONFIG_HOTPLUG_CPU
	cpuhp_state_remove_instance_nocalls(CPUHP_PADATA_DEAD,
					    &pinst->cpu_dead_node);
	cpuhp_state_remove_instance_nocalls(hp_online, &pinst->cpu_online_node);
#endif

	WARN_ON(!list_empty(&pinst->pslist));

	free_cpumask_var(pinst->cpumask.pcpu);
	free_cpumask_var(pinst->cpumask.cbcpu);
	destroy_workqueue(pinst->serial_wq);
	destroy_workqueue(pinst->parallel_wq);
	kfree(pinst);
}

#define kobj2pinst(_kobj)					\
	container_of(_kobj, struct padata_instance, kobj)
#define attr2pentry(_attr)					\
	container_of(_attr, struct padata_sysfs_entry, attr)

static void padata_sysfs_release(struct kobject *kobj)
{
	struct padata_instance *pinst = kobj2pinst(kobj);
	__padata_free(pinst);
}

struct padata_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct padata_instance *, struct attribute *, char *);
	ssize_t (*store)(struct padata_instance *, struct attribute *,
			 const char *, size_t);
};

static ssize_t show_cpumask(struct padata_instance *pinst,
			    struct attribute *attr,  char *buf)
{
	struct cpumask *cpumask;
	ssize_t len;

	mutex_lock(&pinst->lock);
	if (!strcmp(attr->name, "serial_cpumask"))
		cpumask = pinst->cpumask.cbcpu;
	else
		cpumask = pinst->cpumask.pcpu;

	len = snprintf(buf, PAGE_SIZE, "%*pb\n",
		       nr_cpu_ids, cpumask_bits(cpumask));
	mutex_unlock(&pinst->lock);
	return len < PAGE_SIZE ? len : -EINVAL;
}

static ssize_t store_cpumask(struct padata_instance *pinst,
			     struct attribute *attr,
			     const char *buf, size_t count)
{
	cpumask_var_t new_cpumask;
	ssize_t ret;
	int mask_type;

	if (!alloc_cpumask_var(&new_cpumask, GFP_KERNEL))
		return -ENOMEM;

	ret = bitmap_parse(buf, count, cpumask_bits(new_cpumask),
			   nr_cpumask_bits);
	if (ret < 0)
		goto out;

	mask_type = !strcmp(attr->name, "serial_cpumask") ?
		PADATA_CPU_SERIAL : PADATA_CPU_PARALLEL;
	ret = padata_set_cpumask(pinst, mask_type, new_cpumask);
	if (!ret)
		ret = count;

out:
	free_cpumask_var(new_cpumask);
	return ret;
}

#define PADATA_ATTR_RW(_name, _show_name, _store_name)		\
	static struct padata_sysfs_entry _name##_attr =		\
		__ATTR(_name, 0644, _show_name, _store_name)
#define PADATA_ATTR_RO(_name, _show_name)		\
	static struct padata_sysfs_entry _name##_attr = \
		__ATTR(_name, 0400, _show_name, NULL)

PADATA_ATTR_RW(serial_cpumask, show_cpumask, store_cpumask);
PADATA_ATTR_RW(parallel_cpumask, show_cpumask, store_cpumask);

/*
 * Padata sysfs provides the following objects:
 * serial_cpumask   [RW] - cpumask for serial workers
 * parallel_cpumask [RW] - cpumask for parallel workers
 */
static struct attribute *padata_default_attrs[] = {
	&serial_cpumask_attr.attr,
	&parallel_cpumask_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(padata_default);

static ssize_t padata_sysfs_show(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	struct padata_instance *pinst;
	struct padata_sysfs_entry *pentry;
	ssize_t ret = -EIO;

	pinst = kobj2pinst(kobj);
	pentry = attr2pentry(attr);
	if (pentry->show)
		ret = pentry->show(pinst, attr, buf);

	return ret;
}

static ssize_t padata_sysfs_store(struct kobject *kobj, struct attribute *attr,
				  const char *buf, size_t count)
{
	struct padata_instance *pinst;
	struct padata_sysfs_entry *pentry;
	ssize_t ret = -EIO;

	pinst = kobj2pinst(kobj);
	pentry = attr2pentry(attr);
	if (pentry->show)
		ret = pentry->store(pinst, attr, buf, count);

	return ret;
}

static const struct sysfs_ops padata_sysfs_ops = {
	.show = padata_sysfs_show,
	.store = padata_sysfs_store,
};

static const struct kobj_type padata_attr_type = {
	.sysfs_ops = &padata_sysfs_ops,
	.default_groups = padata_default_groups,
	.release = padata_sysfs_release,
};

/**
 * padata_alloc - allocate and initialize a padata instance
 * @name: used to identify the instance
 *
 * Return: new instance on success, NULL on error
 */
struct padata_instance *padata_alloc(const char *name)
{
	struct padata_instance *pinst;

	pinst = kzalloc(sizeof(struct padata_instance), GFP_KERNEL);
	if (!pinst)
		goto err;

	pinst->parallel_wq = alloc_workqueue("%s_parallel", WQ_UNBOUND, 0,
					     name);
	if (!pinst->parallel_wq)
		goto err_free_inst;

	cpus_read_lock();

	pinst->serial_wq = alloc_workqueue("%s_serial", WQ_MEM_RECLAIM |
					   WQ_CPU_INTENSIVE, 1, name);
	if (!pinst->serial_wq)
		goto err_put_cpus;

	if (!alloc_cpumask_var(&pinst->cpumask.pcpu, GFP_KERNEL))
		goto err_free_serial_wq;
	if (!alloc_cpumask_var(&pinst->cpumask.cbcpu, GFP_KERNEL)) {
		free_cpumask_var(pinst->cpumask.pcpu);
		goto err_free_serial_wq;
	}

	INIT_LIST_HEAD(&pinst->pslist);

	cpumask_copy(pinst->cpumask.pcpu, cpu_possible_mask);
	cpumask_copy(pinst->cpumask.cbcpu, cpu_possible_mask);

	if (padata_setup_cpumasks(pinst))
		goto err_free_masks;

	__padata_start(pinst);

	kobject_init(&pinst->kobj, &padata_attr_type);
	mutex_init(&pinst->lock);

#ifdef CONFIG_HOTPLUG_CPU
	cpuhp_state_add_instance_nocalls_cpuslocked(hp_online,
						    &pinst->cpu_online_node);
	cpuhp_state_add_instance_nocalls_cpuslocked(CPUHP_PADATA_DEAD,
						    &pinst->cpu_dead_node);
#endif

	cpus_read_unlock();

	return pinst;

err_free_masks:
	free_cpumask_var(pinst->cpumask.pcpu);
	free_cpumask_var(pinst->cpumask.cbcpu);
err_free_serial_wq:
	destroy_workqueue(pinst->serial_wq);
err_put_cpus:
	cpus_read_unlock();
	destroy_workqueue(pinst->parallel_wq);
err_free_inst:
	kfree(pinst);
err:
	return NULL;
}
EXPORT_SYMBOL(padata_alloc);

/**
 * padata_free - free a padata instance
 *
 * @pinst: padata instance to free
 */
void padata_free(struct padata_instance *pinst)
{
	kobject_put(&pinst->kobj);
}
EXPORT_SYMBOL(padata_free);

/**
 * padata_alloc_shell - Allocate and initialize padata shell.
 *
 * @pinst: Parent padata_instance object.
 *
 * Return: new shell on success, NULL on error
 */
struct padata_shell *padata_alloc_shell(struct padata_instance *pinst)
{
	struct parallel_data *pd;
	struct padata_shell *ps;

	ps = kzalloc(sizeof(*ps), GFP_KERNEL);
	if (!ps)
		goto out;

	ps->pinst = pinst;

	cpus_read_lock();
	pd = padata_alloc_pd(ps);
	cpus_read_unlock();

	if (!pd)
		goto out_free_ps;

	mutex_lock(&pinst->lock);
	RCU_INIT_POINTER(ps->pd, pd);
	list_add(&ps->list, &pinst->pslist);
	mutex_unlock(&pinst->lock);

	return ps;

out_free_ps:
	kfree(ps);
out:
	return NULL;
}
EXPORT_SYMBOL(padata_alloc_shell);

/**
 * padata_free_shell - free a padata shell
 *
 * @ps: padata shell to free
 */
void padata_free_shell(struct padata_shell *ps)
{
	struct parallel_data *pd;

	if (!ps)
		return;

	mutex_lock(&ps->pinst->lock);
	list_del(&ps->list);
	pd = rcu_dereference_protected(ps->pd, 1);
	if (refcount_dec_and_test(&pd->refcnt))
		padata_free_pd(pd);
	mutex_unlock(&ps->pinst->lock);

	kfree(ps);
}
EXPORT_SYMBOL(padata_free_shell);

void __init padata_init(void)//并行加速框架初始化函数
{
	unsigned int i, possible_cpus;//`i` 用于循环，`possible_cpus` 记录系统中可能的 CPU 数量
#ifdef CONFIG_HOTPLUG_CPU
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "padata:online",
				      padata_cpu_online, NULL);//注册 CPU 上线时的回调函数
	if (ret < 0)
		goto err;//注册失败，跳转到错误处理部分
	hp_online = ret;//记录成功注册的状态 ID

	ret = cpuhp_setup_state_multi(CPUHP_PADATA_DEAD, "padata:dead",
				      NULL, padata_cpu_dead);//注册 CPU 离线时的回调函数
	if (ret < 0)
		goto remove_online_state;//注册失败，撤销之前注册的 "online" 状态
#endif

	possible_cpus = num_possible_cpus();//获取系统中可能的 CPU 数量
	padata_works = kmalloc_array(possible_cpus, sizeof(struct padata_work),
				     GFP_KERNEL);//分配 `padata_work` 数组，用于保存每个 CPU 的任务结构
	if (!padata_works)
		goto remove_dead_state;//分配失败，释放 "dead" 状态，退出初始化

	for (i = 0; i < possible_cpus; ++i)//将 `padata_work` 结构添加到 `padata_free_works` 链表中
		list_add(&padata_works[i].pw_list, &padata_free_works);

	return;//成功返回

remove_dead_state:
#ifdef CONFIG_HOTPLUG_CPU
	cpuhp_remove_multi_state(CPUHP_PADATA_DEAD);//撤销 "dead" 状态的注册
remove_online_state:
	cpuhp_remove_multi_state(hp_online);//撤销 "online" 状态的注册
err:
#endif
	pr_warn("padata: initialization failed\n");//打印警告信息，提示初始化失败
}
