// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/slab.h>
#include <linux/bug.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>

#include "internal.h"

/*
 * Unconventionally, value '0' at rpal_id_bitmap means used while
 * '1' indicates the bit (id) is available.
 */
DECLARE_BITMAP(rpal_id_bitmap, RPAL_NR_ID);
struct kmem_cache *service_cache;
atomic64_t service_key_counter;
DEFINE_HASHTABLE(service_hash_table, ilog2(RPAL_NR_ID));
DEFINE_SPINLOCK(hash_table_lock);

static inline void rpal_free_service_id(int id)
{
	set_bit(id, rpal_id_bitmap);
}

static void __rpal_put_service(struct rpal_service *rs)
{
	pr_debug("rpal: free service %d, tgid: %d\n", rs->id,
		rs->leader_thread->pid);
	mmdrop(rs->mm);
	put_task_struct(rs->leader_thread);
	rpal_free_service_id(rs->id);
	kmem_cache_free(service_cache, rs);
}

void rpal_put_service_async_fn(struct work_struct *work)
{
	struct rpal_service *rs =
		container_of(work, struct rpal_service, delayed_put_work.work);

	__rpal_put_service(rs);
}

static int rpal_alloc_service_id(void)
{
	int id;

	do {
		id = find_first_bit(rpal_id_bitmap, RPAL_NR_ID);
		if (id == RPAL_NR_ID) {
			id = RPAL_INVALID_ID;
			break;
		}
	} while (!test_and_clear_bit(id, rpal_id_bitmap));

	return id;
}

static bool is_valid_id(int id)
{
	return id >= 0 && id < RPAL_NR_ID;
}

static u64 rpal_alloc_service_key(void)
{
	u64 key = atomic64_fetch_inc(&service_key_counter);
	/* confirm we do not run out keys */
	if (unlikely(key == _AC(-1, UL)))
		rpal_err("key is exhausted\n");
	return key;
}

/**
 * @brief get new reference to a rpal service, a corresponding
 *  rpal_put_service() should be called later by the caller.
 *
 * @param rs The struct rpal_service to get.
 *
 * @return new reference of struct rpal_service.
 */
struct rpal_service *rpal_get_service(struct rpal_service *rs)
{
	if (!rs)
		return NULL;
	atomic_inc(&rs->refcnt);
	return rs;
}

/**
 * @brief put a reference to a rpal service. If the reference count of
 *  the service turns to be 0, then release its struct rpal_service.
 *
 * @param rs The struct rpal_service to put.
 */
void rpal_put_service(struct rpal_service *rs)
{
	if (!rs)
		return;
	if (atomic_dec_and_test(&rs->refcnt)) {
		INIT_DELAYED_WORK(&rs->delayed_put_work,
				  rpal_put_service_async_fn);
		schedule_delayed_work(&rs->delayed_put_work, HZ * 30);
	}
}

static u32 get_hash_key(u64 key)
{
	return key % RPAL_NR_ID;
}

static void insert_service(struct rpal_service *rs)
{
	unsigned long flags;
	int hash_key;

	hash_key = get_hash_key(rs->key);

	spin_lock_irqsave(&hash_table_lock, flags);
	hash_add(service_hash_table, &rs->hlist, hash_key);
	spin_unlock_irqrestore(&hash_table_lock, flags);
}

static void delete_service(struct rpal_service *rs)
{
	unsigned long flags;

	spin_lock_irqsave(&hash_table_lock, flags);
	hash_del(&rs->hlist);
	spin_unlock_irqrestore(&hash_table_lock, flags);
}

static void rpal_service_data_init(struct rpal_service *rs)
{
	spin_lock_init(&rs->lock);
	mutex_init(&rs->mutex);
}

static struct rpal_service *rpal_register_service(int service_id)
{
	struct rpal_service *rs;

	if (!thread_group_leader(current)) {
		rpal_err("task %d is not group leader %d\n", current->pid,
			 current->tgid);
		goto fail;
	}

	rs = kmem_cache_zalloc(service_cache, GFP_KERNEL);
	if (!rs)
		goto fail;

	rpal_service_data_init(rs);

	rs->leader_thread = get_task_struct(current);
	current->rpal_rs = rs;
	current->mm->rpal_rs = rs;
	rs->mm = current->mm;
	mmgrab(current->mm);

	rs->key = rpal_alloc_service_key();

	rs->id = service_id;

	/*
	 * The reference comes from:
	 * 1. registered service always has one reference
	 * 2. leader_thread also has one reference
	 * 3. mm also hold one reference
	 */
	atomic_set(&rs->refcnt, 3);

	insert_service(rs);

	pr_debug("rpal: register service, key: %llx, id: %d, command: %s, tgid: %d\n",
		rs->key, rs->id, current->comm, current->tgid);

	return rs;

fail:
	rpal_free_service_id(service_id);
	return NULL;
}

void rpal_unregister_service(struct rpal_service *rs)
{
	if (!rs)
		return;

	delete_service(rs);
	if (unlikely(current->mm->rpal_rs != rs)) {
		rpal_err("current->mm->rpal_rs (0x%16lx) != rs (0x%16lx)\n",
			 (unsigned long)current->mm->rpal_rs,
			 (unsigned long)rs);
	}

	pr_debug("rpal: unregister service, id: %d, tgid: %d\n", rs->id,
		rs->leader_thread->tgid);

	rpal_put_service(rs);
}

extern const struct sched_class fair_sched_class;

struct rpal_service *rpal_alloc_and_register_service(void)
{
	int id = -1;

	if (!rpal_inited)
		return NULL;

	if (current->sched_class != &fair_sched_class) {
		rpal_err("Not fair sched class, pid: %d, comm: %s\n",
			 current->pid, current->comm);
		return NULL;
	}

	id = rpal_alloc_service_id();
	if (!is_valid_id(id))
		return NULL;

	return rpal_register_service(id);
}

void exit_rpal(bool group_dead)
{
	struct rpal_service *rs = rpal_current_service();

	if (!rs)
		return;

	if (group_dead)
		rpal_unregister_service(rs);
}

int __init rpal_service_init(void)
{
	service_cache = kmem_cache_create("rpal_service_cache",
					  sizeof(struct rpal_service), 0,
					  SLAB_PANIC, NULL);
	if (!service_cache) {
		rpal_err("service init fail\n");
		return -1;
	}

	bitmap_fill(rpal_id_bitmap, RPAL_NR_ID);
	atomic64_set(&service_key_counter, 1);

	return 0;
}

void rpal_service_exit(void)
{
	kmem_cache_destroy(service_cache);
}
