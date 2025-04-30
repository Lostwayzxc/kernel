// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/signal.h>
#include <linux/rpal.h>
#include <linux/pkeys.h>
#include <asm/fpu/internal.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

#include "internal.h"

DECLARE_BITMAP(rpal_pkey_bitmap, 16);

void rpal_service_pku_init(struct rpal_service *rs)
{
	u16 all_pkeys_mask = ((1U << arch_max_pkey()) - 1);
	struct mm_struct *mm = current->mm;

	/* We consume all pkeys so that no pkeys will be allocated by others */
	mmap_write_lock(mm);
	if (mm->context.pkey_allocation_map != 0x1)
		rpal_err("pkey has been allocated: %u\n",
			 mm->context.pkey_allocation_map);
	mm->context.pkey_allocation_map = all_pkeys_mask;
	mmap_write_unlock(mm);
}

static inline u32 rpal_get_new_val(u32 old_val, u32 new_val, int mode)
{
	switch (mode) {
	case RPAL_PKRU_SET:
		return new_val;
	case RPAL_PKRU_UNION:
		return rpal_pkru_union(old_val, new_val);
	case RPAL_PKRU_INTERSECT:
		return rpal_pkru_intersect(old_val, new_val);
	default:
		rpal_err("%s: invalid mode: %d\n", __func__, mode);
		return old_val;
	}
}

int rpal_get_task_pkey(struct task_struct *tsk)
{
	int ret = -RPAL_ERR_NO_SERVICE;
	struct rpal_service *rs = rpal_get_task_service(tsk);

	if (rs) {
		ret = rs->pkey;
		rpal_put_service(rs);
	}
	return ret;
}

void rpal_set_current_pkru(u32 val, int mode)
{
	u32 new_val;

	fpregs_lock();
	new_val = rpal_get_new_val(rdpkru(), val, mode);
	write_pkru(new_val);
	fpregs_unlock();
}

int rpal_set_task_fpu_pkru(struct task_struct *task, u32 val, int mode)
{
	struct thread_struct *t = &task->thread;

	val = rpal_get_new_val(t->pkru, val, mode);
	t->pkru = val;

	return 0;
}

struct task_function_data {
	struct task_struct *task;
	u32 val;
	int mode;
	int ret;
};

static void rpal_set_remote_pkru(void *data)
{
	struct task_function_data *tfd = data;
	struct task_struct *task = tfd->task;

	if (task) {
		/* -EAGAIN */
		if (task_cpu(task) != smp_processor_id())
			return;

		tfd->ret = -ESRCH;
		if (task == current) {
			rpal_set_current_pkru(tfd->val, tfd->mode);
			tfd->ret = 0;
		} else {
			tfd->ret = rpal_set_task_fpu_pkru(task, tfd->val,
							  tfd->mode);
		}
		return;
	}
}

void rpal_set_pku_schedule_tail(struct task_struct *prev)
{
	if (rpal_test_current_thread_flag(RPAL_IS_RECEIVER_BIT)) {
		struct rpal_service *cur = rpal_current_service();
		u32 val = rpal_pkey_to_pkru(cur->pkey);

		rpal_set_current_pkru(val, RPAL_PKRU_SET);
	} else {
		struct rpal_service *cur = rpal_current_service();
		u32 val = rpal_pkey_to_pkru(cur->pkey);

		val = rpal_pkru_union(
			val,
			rpal_pkey_to_pkru(
				current->rpal_sd->receiver->rpal_rs->pkey));
		rpal_set_current_pkru(val, RPAL_PKRU_SET);
	}
}


static int rpal_task_function_call(struct task_struct *task, u32 val, int mode)
{
	struct task_function_data data = {
		.task = task,
		.val = val,
		.mode = mode,
		.ret = -EAGAIN,
	};
	int ret;

	for (;;) {
		smp_call_function_single(task_cpu(task), rpal_set_remote_pkru,
					 &data, 1);
		ret = data.ret;

		if (ret != -EAGAIN)
			break;

		cond_resched();
	}

	return ret;
}

static void rpal_set_task_pkru(struct task_struct *task, u32 val, int mode)
{
	if (task == current)
		rpal_set_current_pkru(val, mode);
	else
		rpal_task_function_call(task, val, mode);
}

struct rpal_pkru_info {
	u32 val;
	struct task_struct *task;
};

static void rpal_set_group_pkru(u32 val, int mode)
{
	struct task_struct *p;

	for_each_thread(current, p) {
		rpal_set_task_pkru(p, val, mode);
	}
}

int rpal_pkey_setup(struct rpal_service *rs, int pkey)
{
	int err, val;

	val = rpal_pkey_to_pkru(pkey);

	mmap_write_lock(current->mm);
	if (rs->pku_on) {
		mmap_write_unlock(current->mm);
		return 0;
	}
	rs->pkey = pkey;
	/* others must see rs->pkey before rs->pku_on */
	barrier();
	rs->pku_on = true;
	mmap_write_unlock(current->mm);
	rpal_set_group_pkru(val, RPAL_PKRU_UNION);
	err = do_rpal_mprotect_pkey(rs->base, RPAL_ADDR_SPACE_SIZE, pkey);
	if (unlikely(err))
		rpal_err("do_rpal_mprotect_key error: %d\n", err);
	rpal_set_group_pkru(val, RPAL_PKRU_SET);
	return 0;
}

void rpal_service_pkey_init(struct task_struct *tsk)
{
	if (rpal_pku_enabled()) {
		u16 all_pkeys_mask = ((1U << arch_max_pkey()) - 1);
		struct mm_struct *mm = tsk->mm;

		/* We consume all pkeys so that no pkeys will be allocated by others */
		mmap_write_lock(mm);
		if (mm->context.pkey_allocation_map != 0x1)
			rpal_err("pkey has been allocated: %u\n",
				 mm->context.pkey_allocation_map);
		mm->context.pkey_allocation_map = all_pkeys_mask;
		mmap_write_unlock(mm);
	}
}

void rpal_service_pkey_exit(struct task_struct *tsk)
{
	struct mm_struct *mm = tsk->mm;

	mmap_write_lock(mm);
	/* 0x1 is initial value of pkey allocation map */
	mm->context.pkey_allocation_map = 0x1;
	mmap_write_unlock(mm);
}

int rpal_alloc_pkey(struct rpal_service *rs, int pkey)
{
	int ret;

	if (pkey >= 0 && pkey < arch_max_pkey())
		return pkey;

	do {
		ret = find_first_zero_bit(rpal_pkey_bitmap, arch_max_pkey());
		if (ret == arch_max_pkey())
			break;
		if (!test_and_set_bit(ret, rpal_pkey_bitmap))
			break;
	} while (1);

	return ret;
}

void rpal_pku_init(void)
{
	bitmap_zero(rpal_pkey_bitmap, arch_max_pkey());
}
