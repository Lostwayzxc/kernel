// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/slab.h>
#include <linux/printk.h>

#include <asm/pgtable.h>
#include <asm/fsgsbase.h>

#include "internal.h"

struct kmem_cache *sender_cache, *receiver_cache, *common_cache;

void rpal_common_data_init(struct rpal_common_data *rcd)
{
	rcd->bp_task = current;
	rcd->service_id = rpal_current_service()->id;
	rcd->last_clock = 0;
}

struct rpal_shared_page *rpal_find_shared_page(struct rpal_service *rs,
					       unsigned long addr)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_shared_page *rsp, *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&cur->lock, flags);
	list_for_each_entry(rsp, &rs->shared_pages, list) {
		if (rsp->user_start <= addr && addr < rsp->user_end) {
			ret = rpal_get_shared_page(rsp);
			break;
		}
	}
	spin_unlock_irqrestore(&cur->lock, flags);

	return ret;
}

long rpal_register_sender(unsigned long addr)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_shared_page *rsp;
	struct rpal_sender_data *rsd;
	long ret = 0;

	if (rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT) ||
			rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}

	rsp = rpal_find_shared_page(cur, addr);
	if (!rsp) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}

	rsd = kmem_cache_zalloc(sender_cache, GFP_KERNEL);
	if (rsd == NULL) {
		ret = -RPAL_ERR_RETRY;
		goto put_shared_page;
	}

	if (current->rpal_cd == NULL) {
		current->rpal_cd = kmem_cache_zalloc(common_cache, GFP_KERNEL);
		if (current->rpal_cd == NULL) {
			ret = -RPAL_ERR_RETRY;
			goto free_sender;
		}
		rpal_common_data_init(current->rpal_cd);
		if (rpal_init_thread_pending(current->rpal_cd)) {
			kmem_cache_free(common_cache, current->rpal_cd);
			ret = -RPAL_ERR_RETRY;
			goto free_sender;
		}
	}
	rsd->rcd = current->rpal_cd;
	rsd->rsp = rsp;

	rsd->sec = (struct rpal_sender_epoll_context *)(addr - rsp->user_start +
							rsp->kernel_start);
	rsd->receiver = NULL;
	rsd->sec->start_time = 0;
	rsd->sec->total_time = 0;

	current->rpal_sd = rsd;
	rpal_set_current_thread_flag(RPAL_IS_SENDER_BIT);

	pr_debug("rpal debug: [%d] register_sender, shared_addr 0x%016lx\n",
		 current->pid, (unsigned long)rsd->sec);

	atomic_inc(&cur->thread_cnt);

	return 0;

free_sender:
	kmem_cache_free(sender_cache, rsd);
put_shared_page:
	rpal_put_shared_page(rsp);
out:
	return ret;
}

long rpal_unregister_sender(void)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_sender_data *rsd = current->rpal_sd;
	long ret = 0;

	if (!rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT)) {
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}
	if (unlikely(!rsd)) {
		rpal_err("task %d has no sender data\n", current->pid);
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}

	rpal_put_shared_page(rsd->rsp);
	rpal_clear_current_thread_flag(RPAL_IS_SENDER_BIT);
	kmem_cache_free(sender_cache, rsd);
	if (!rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		rpal_free_thread_pending(current->rpal_cd);
		kmem_cache_free(common_cache, current->rpal_cd);
		current->rpal_cd = NULL;
	}

	pr_debug("rpal debug: [%d] unregister sender, ret: %ld\n", current->pid,
		 ret);

	atomic_dec(&cur->thread_cnt);

out:
	return ret;
}

bool set_fs_tsk_map(void)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_fsbase_tsk_map *ftm;
	unsigned long fsbase = rdfsbase();
	bool success = false;
	int i = 0;

	for (i = 0; i < RPAL_MAX_RECEIVER_NUM; ++i) {
		ftm = &cur->fs_tsk_map[i];
		if (ftm->fsbase == 0 &&
		    cmpxchg64(&ftm->fsbase, 0, fsbase) == 0) {
			ftm->tsk = current;
			success = true;
			break;
		}
	}

	return success;
}

bool clear_fs_tsk_map(void)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_fsbase_tsk_map *ftm;
	unsigned long fsbase = rdfsbase();
	bool success = false;
	int i = 0;

	for (i = 0; i < RPAL_MAX_RECEIVER_NUM; ++i) {
		ftm = &cur->fs_tsk_map[i];
		if (ftm->fsbase == fsbase) {
			ftm->tsk = NULL;
			barrier();
			ftm->fsbase = 0;
			success = true;
			break;
		}
	}

	return success;
}

long rpal_register_receiver(unsigned long addr)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_receiver_data *rrd;
	struct rpal_shared_page *rsp;
	long ret = 0;

	if (rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT) ||
			rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}

	rsp = rpal_find_shared_page(cur, addr);
	if (!rsp) {
		ret = -RPAL_ERR_BAD_SERVICE_STATUS;
		goto out;
	}

	rrd = kmem_cache_zalloc(receiver_cache, GFP_KERNEL);
	if (rrd == NULL) {
		ret = -RPAL_ERR_RETRY;
		goto put_shared_page;
	}

	if (current->rpal_cd == NULL) {
		current->rpal_cd = kmem_cache_zalloc(common_cache, GFP_KERNEL);
		if (current->rpal_cd == NULL) {
			ret = -RPAL_ERR_RETRY;
			goto free_receiver;
		}
		rpal_common_data_init(current->rpal_cd);
		if (rpal_init_thread_pending(current->rpal_cd)) {
			kmem_cache_free(common_cache, current->rpal_cd);
			ret = -RPAL_ERR_RETRY;
			goto free_receiver;
		}
	}
	rrd->rcd = current->rpal_cd;
	rrd->rsp = rsp;

	rrd->rec =
		(struct rpal_receiver_epoll_context *)(addr - rsp->user_start +
						       rsp->kernel_start);
	rrd->sender = NULL;
	rrd->ep = NULL;

	if (!set_fs_tsk_map()) {
		ret = -RPAL_ERR_REACH_LIMIT;
		goto free;
	}
	rrd->rec->total_time = 0;
	current->rpal_rd = rrd;
	rpal_set_current_thread_flag(RPAL_IS_RECEIVER_BIT);

	rpal_insert_wake_list(cur, rrd);

	pr_debug("rpal: debug [%d] register_receiver, shared_addr 0x%016lx\n",
		 current->pid, (unsigned long)rrd->rec);

	atomic_inc(&cur->thread_cnt);

	return 0;

free:
	if (!rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT))
		kmem_cache_free(common_cache, current->rpal_cd);
free_receiver:
	kmem_cache_free(receiver_cache, rrd);
put_shared_page:
	rpal_put_shared_page(rsp);
out:
	return ret;
}

long rpal_unregister_receiver(void)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_receiver_data *rrd = current->rpal_rd;
	long ret = 0;

	if (!rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}
	if (unlikely(!rrd)) {
		rpal_err("task %d has no receiver data\n", current->pid);
		ret = -RPAL_ERR_BAD_THREAD_STATUS;
		goto out;
	}

	/* Waiting for sender kernel ret */
	smp_rmb();
	while (READ_ONCE(rrd->sender))
		schedule();

	clear_fs_tsk_map();

	rpal_put_shared_page(rrd->rsp);
	rpal_remove_wake_list(cur, rrd);
	rpal_clear_current_thread_flag(RPAL_IS_RECEIVER_BIT);
	kmem_cache_free(receiver_cache, rrd);
	if (!rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT)) {
		rpal_free_thread_pending(current->rpal_cd);
		kmem_cache_free(common_cache, current->rpal_cd);
		current->rpal_cd = NULL;
	}

	pr_debug("rpal debug: [%d] unregister receiver\n", current->pid);

	atomic_dec(&cur->thread_cnt);

out:
	return ret;
}

void copy_rpal(struct task_struct *p)
{
	struct rpal_service *cur = rpal_current_service();

	p->rpal_rs = rpal_get_service(cur);
}

void rpal_rebuild_receiver_context_on_exit(void)
{
	struct task_struct *receiver = NULL;
	struct rpal_sender_data *rsd = current->rpal_sd;
	struct rpal_sender_epoll_context *sec = rsd->sec;
	struct rpal_receiver_data *rrd;
	struct rpal_receiver_epoll_context *rec;
	unsigned long fsbase;
	int status = rpal_sd_build_ep_app(rsd);

	if (!rpal_is_valid_magic(sec->magic))
		goto out;

	fsbase = sec->ec.fsbase;
	if (rpal_is_correct_address(rpal_current_service(), fsbase)) {
		/* fsbase may be 0 if sender never issue an rpal call */
		if (fsbase != 0)
			rpal_err("unexpect address in %s, addr: 0x%016lx\n", __func__,
				 fsbase);
		goto out;
	}

	receiver = rpal_find_next_task(fsbase);
	if (!receiver)
		goto out;

	rrd = receiver->rpal_rd;
	if (!rrd)
		goto out;

	rec = rrd->rec;

	if (atomic_read(&rec->ep_status) == status) {
		atomic_cmpxchg(&rec->g_status, RPAL_TASK_BLOCKED,
			       RPAL_TASK_KERNEL_RET);
		atomic_cmpxchg(&rec->ep_status, status, RPAL_EP_WAIT);
	}

out:
	return;
}

void exit_rpal_thread(void)
{
	struct rpal_service *cur = rpal_current_service();

	if (rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT)) {
		/* sender may corrupt receiver's state if unexpectedly exited, so rebuild it */
		rpal_rebuild_receiver_context_on_exit();
		rpal_unregister_sender();
		pr_debug("rpal debug: exit sender thread: %d\n", current->pid);
	}

	if (rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		rpal_unregister_receiver();
		pr_debug("rpal debug: exit receiver thread: %d\n",
			 current->pid);
	}

	current->rpal_rs = NULL;
	rpal_put_service(cur);
}

int rpal_rebuild_sender_context_on_fault(struct pt_regs *regs,
					 unsigned long addr, int error_code)
{
	if (rpal_test_current_thread_flag(RPAL_IS_SENDER_BIT)) {
		struct rpal_sender_epoll_context *sec = current->rpal_sd->sec;
		unsigned long erip, ersp;
		int magic;

		magic = sec->magic;
		erip = sec->ec.erip;
		ersp = sec->ec.ersp;
		if (rpal_is_valid_magic(magic)) {
			/* rebuild if receiver's memory is wrong(e.g. oom) */
			rpal_rebuild_receiver_context_on_exit();
			regs->ax = error_code;
			regs->ip = erip;
			regs->sp = ersp;
			sec->magic = 0;
#ifdef CONFIG_RPAL_PKU
			if (rpal_pku_enabled()) {
				rpal_set_current_pkru(
					rpal_pkey_to_pkru(
						rpal_current_service()->pkey),
					RPAL_PKRU_SET);
			}
#endif
			if (!rpal_is_correct_address(rpal_current_service(), regs->ip)) {
				/* receiver has crashed */
				sec->total_time += rdtsc_ordered() - sec->start_time;
			}
			return 0;
		}
	}
	return -RPAL_ERR_INVAL;
}

int __init rpal_thread_init(void)
{
	sender_cache = kmem_cache_create("rpal_sender_cache",
					 sizeof(struct rpal_sender_data), 0,
					 SLAB_PANIC, NULL);
	if (!sender_cache)
		goto fail;

	receiver_cache = kmem_cache_create("rpal_receiver_cache",
					   sizeof(struct rpal_receiver_data), 0,
					   SLAB_PANIC, NULL);
	if (!receiver_cache)
		goto free_sender_cache;

	common_cache = kmem_cache_create("rpal_common_cache",
					 sizeof(struct rpal_common_data), 0,
					 SLAB_PANIC, NULL);
	if (!common_cache)
		goto free_receiver_cache;

	return 0;

free_receiver_cache:
	kmem_cache_destroy(receiver_cache);
free_sender_cache:
	kmem_cache_destroy(sender_cache);
fail:
	rpal_err("rpal thread init fail\n");
	return -1;
}
