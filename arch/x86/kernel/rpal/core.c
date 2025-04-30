// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/pkeys.h>
#include <net/af_unix.h>
#include <asm/paravirt_types.h>
#include <linux/mmu_context.h>

#include "internal.h"

bool rpal_inited;
unsigned long rpal_cap;

static inline void rpal_lock_cpu(struct task_struct *tsk)
{
	rpal_set_cpus_allowed_ptr(tsk, true, false);
	if (unlikely(!irqs_disabled())) {
		local_irq_disable();
		rpal_err("%s: irq is enabled\n", __func__);
	}
}

static inline void rpal_unlock_cpu(struct task_struct *tsk)
{
	rpal_set_cpus_allowed_ptr(tsk, false, false);
	if (unlikely(!irqs_disabled())) {
		local_irq_disable();
		rpal_err("%s: irq is enabled\n", __func__);
	}
}

static inline void rpal_unlock_cpu_kernel_ret(struct task_struct *tsk)
{
	rpal_set_cpus_allowed_ptr(tsk, false, true);
}

void rpal_lazy_switch_tail(struct task_struct *tsk)
{
	struct rpal_receiver_epoll_context *rec;

	if (rpal_test_task_thread_flag(current, RPAL_LAZY_SWITCHED_BIT)) {
		rec = current->rpal_rd->rec;
		atomic_cmpxchg(&rec->ep_status,
			       rpal_sd_build_ep_app(tsk->rpal_sd),
			       RPAL_EP_KAPP);
	} else {
		int status;

		rec = tsk->rpal_rd->rec;
		status = atomic_read(&rec->ep_status);
		rpal_unlock_cpu(tsk);
		/*
		 * We must ensure that receiver are not in TASK_RUNNING
		 * state. Otherwise, we may get mixed up, as another
		 * sender may also set RPAL_EP_READY_WAIT_LS, if we
		 * are blocked here for a while.
		 */
		if (status == RPAL_EP_READY_WAIT_LS)
			atomic_cmpxchg(&rec->ep_status, RPAL_EP_READY_WAIT_LS,
				       RPAL_EP_WAIT);
		rpal_unlock_cpu(current);
	}
}

void rpal_kernel_ret(struct pt_regs *regs)
{
	struct rpal_receiver_data *rrd;
	struct task_struct *tsk;
	struct rpal_receiver_epoll_context *rec;

	if (rpal_test_current_thread_flag(RPAL_RECEIVER_KERNEL_RET_BIT)) {
		rrd = current->rpal_rd;
		rec = rrd->rec;
		if (rec->timeout > 0)
			hrtimer_cancel(&rrd->ep_sleeper.timer);
		rpal_remove_ep_wait_list(rrd);
		regs->ax = rpal_ep_send_events(rrd->ep, rec);
		fdput(rrd->f);
		atomic_xchg(&rec->ep_status, RPAL_EP_KSYS);
		rpal_clear_current_thread_flag(RPAL_SYSCALL_ENTER_BIT);
		rpal_clear_current_thread_flag(RPAL_RECEIVER_KERNEL_RET_BIT);
	} else {
		int status;

		tsk = current->rpal_sd->receiver;
		rec = tsk->rpal_rd->rec;
		rpal_clear_task_thread_flag(tsk, RPAL_LAZY_SWITCHED_BIT);
		status = atomic_xchg(&rec->g_status, RPAL_TASK_KERNEL_RET);
		if (unlikely(status != RPAL_TASK_BLOCKED))
			rpal_err("status != RPAL_TASK_BLOCKED\n");
		/* make sure kernel return is finished */
		smp_mb();
		WRITE_ONCE(tsk->rpal_rd->sender, NULL);
		/*
		 * We must unlock receiver first, otherwise we may unlock
		 * receiver which is already locked by another sender.
		 *
		 *  Sender A			Receiver B      Sender C
		 *	lazy switch (A->B)
		 *  kernel return
		 *      unlock cpu A
		 *                      epoll_wait
		 *                                         lazy switch(C->B)
		 *                                         lock cpu B
		 *		unlock cpu B
		 *						BUG()			BUG()
		 */
		rpal_unlock_cpu_kernel_ret(tsk);
		rpal_unlock_cpu_kernel_ret(current);
	}
}

static void rebuild_stack(struct rpal_task_context *ctx, struct pt_regs *regs)
{
	regs->r12 = ctx->r12;
	regs->r13 = ctx->r13;
	regs->r14 = ctx->r14;
	regs->r15 = ctx->r15;
	regs->bx = ctx->rbx;
	regs->bp = ctx->rbp;
	regs->ip = ctx->rip;
	regs->sp = ctx->rsp;
}

static void rebuild_sender_stack(struct rpal_sender_data *rsd,
				 struct pt_regs *regs)
{
	rebuild_stack(&rsd->sec->rtc, regs);
}

static void rebuild_receiver_stack(struct rpal_receiver_data *rrd,
				   struct pt_regs *regs)
{
	rebuild_stack(&rrd->rec->rtc, regs);
}

static inline void update_dst_stack(struct task_struct *next, void *src,
				    unsigned long nr)
{
	void *dst;

	dst = (void *)task_top_of_stack(next) - nr;
	memcpy(dst, src, nr);
	next->thread.sp = (unsigned long)dst;
}

asmlinkage void rpal_schedule(struct task_struct *next);

/*
 * rpal_do_kernel_context_switch - the main routine of RPAL lazy switch
 * @next: task to switch to
 * @regs: the user pt_regs saved in kernel entry
 * @src: stack content start address, which may be copied to the next
 *       task's kernel stack
 * @size: the size of the stack content that need to be copied
 * @reserve_stack: whether or not to keep the next task's kernel stack
 *				    content unchanged.
 *
 * This function performs the lazy switch. When switch from sender to
 * receiver, we need to lock both task to current CPU to avoid double
 * control flow when we perform lazy switch and after then. epoll fd
 * need to be handled carefully here since we skip the fdput() in
 * epoll_wait().
 *
 * When switch from receiver to sender, we have a fastpath to let
 * receiver enter epoll wait state, avoiding returning to userspace.
 *
 * Note, this function is called with either current task's kernel stack
 * (where reserve_stack is false) or an IST stack (where reserve_stack is
 * true). As this function will not switch kernel stack to next task's kernel
 * stack, it must be used combined with some assemblly code and
 * rpal_lazy_switch_tail(). Refer to entry_SYSCALL_64 for kernel stack example
 * and idtentry_part where paranoid==1 for IST stack example.
 */
static struct task_struct *
rpal_do_kernel_context_switch(struct task_struct *next, struct pt_regs *regs,
			      void *src, unsigned long size, bool reserve_stack)
{
	struct task_struct *prev = current;

	if (rpal_test_task_thread_flag(next, RPAL_LAZY_SWITCHED_BIT)) {
		rpal_resume_ep(next);
		current->rpal_sd->receiver = next;
		rpal_lock_cpu(current);
		rpal_lock_cpu(next);
		rpal_try_to_wake_up(next);
		if (likely(!reserve_stack))
			update_dst_stack(next, src, size);
		rebuild_sender_stack(current->rpal_sd, regs);
		do {
			struct rpal_receiver_epoll_context *rec =
				next->rpal_rd->rec;
			struct rpal_sender_epoll_context *sec =
				current->rpal_sd->sec;
			u64 slice = rdtsc_ordered() - sec->start_time;

			rec->total_time += slice;
			sec->total_time += slice;
		} while (0);
		rpal_schedule(next);
		rpal_fdput(next->rpal_rd->f);
	} else {
		if (likely(!reserve_stack))
			update_dst_stack(next, src, size);
		rebuild_receiver_stack(current->rpal_rd, regs);
		regs->orig_ax = __NR_epoll_wait;
		rpal_ep_poll_nosched(current->rpal_rd, regs);
		next->rpal_sd->sec->start_time = rdtsc_ordered();
		rpal_schedule(next);
		rpal_clear_task_thread_flag(prev, RPAL_LAZY_SWITCHED_BIT);
		prev->rpal_rd->sender = NULL;
	}
	if (unlikely(!irqs_disabled())) {
		local_irq_disable();
		rpal_err("%s: irq is enabled\n", __func__);
	}
	return next;
}

struct task_struct *rpal_find_next_task(unsigned long fsbase)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_service *tgt;
	struct task_struct *tsk = NULL;
	int i;

	tgt = rpal_get_mapped_service_by_addr(cur, fsbase);
	if (unlikely(!tgt)) {
		pr_debug("rpal debug: cannot find legal rs, fsbase: 0x%016lx\n",
			 fsbase);
		return NULL;
	}
	for (i = 0; i < RPAL_MAX_RECEIVER_NUM; ++i) {
		if (tgt->fs_tsk_map[i].fsbase == fsbase) {
			tsk = tgt->fs_tsk_map[i].tsk;
			break;
		}
	}
	rpal_put_service(tgt);

	return tsk;
}


static inline struct task_struct *rpal_get_sender_task(void)
{
	struct task_struct *next;

	next = current->rpal_rd->sender;
	current->rpal_rd->sender = NULL;

	return next;
}

static inline struct task_struct *rpal_misidentify(void)
{
	struct task_struct *next = NULL;
	struct rpal_service *cur = rpal_current_service();
	unsigned long fsbase;

	fsbase = rdfsbase();
	if (unlikely(!rpal_is_correct_address(cur, fsbase))) {
		if (rpal_test_current_thread_flag(RPAL_LAZY_SWITCHED_BIT)) {
			/* current is receiver, next is sender */
			next = rpal_get_sender_task();
			if (unlikely(next == NULL)) {
				rpal_err("cannot find sender task\n");
				goto out;
			}
		} else {
			/* current is sender, next is receiver */
			next = rpal_find_next_task(fsbase);
			if (unlikely(next == NULL)) {
				rpal_err(
					"cannot find receiver task, fsbase: 0x%016lx\n",
					fsbase);
				goto out;
			}
			if (unlikely(rpal_test_task_thread_flag(
				    next, RPAL_LAZY_SWITCHED_BIT)))
				panic("rpal error: double lazy switched, next: 0x%016lx, prev sender: 0x%016lx\n",
				      (unsigned long)next,
				      (unsigned long)next->rpal_rd->sender);
			rpal_set_task_thread_flag(next, RPAL_LAZY_SWITCHED_BIT);
			next->rpal_rd->sender = current;
		}
	}
out:
	return next;
}

static bool in_ret_section(struct rpal_service *rs, unsigned long ip)
{
	return ip >= rs->rcs.ret_begin && ip < rs->rcs.ret_end;
}

/*
 * rpal_kernel_wrfsbase - fastpath when rpal call returns
 * @regs: pt_regs saved in kernel entry
 *
 * If the user is executing rpal call return code and it does
 * not update fsbase yet, force fsbase update to perform a
 * lazy switch immediately.
 */
static inline void rpal_kernel_wrfsbase(struct pt_regs *regs)
{
	struct rpal_service *cur = rpal_current_service();
	struct task_struct *sender = current->rpal_rd->sender;

	if (in_ret_section(cur, regs->ip))
		wrfsbase(sender->thread.fsbase);
}

/*
 * rpal_skip_receiver_code - skip rpal call return code
 * @next: the next task to be lazy switched to.
 * @regs: pt_regs saved in kernel entry
 *
 * If the user is executing rpal call return code and we are about
 * to perform a lazy switch, skip the remaining return code to
 * release receiver's stack. This avoids stack conflict when there
 * are more than one senders calls the receiver.
 */
static inline void rpal_skip_receiver_code(struct task_struct *next,
					   struct pt_regs *regs)
{
	rebuild_sender_stack(next->rpal_sd, regs);
}

/*
 * rpal_skip_receiver_code - skip lazy switch when rpal call return
 * @next: the next task to be lazy switched to.
 * @regs: pt_regs saved in kernel entry
 *
 * If the user is executing rpal call return code and we have not
 * performed a lazy switch, there is no need to perform lazy switch
 * now. Update fsbase and other states to avoid lazy switch.
 */
static inline struct task_struct *
rpal_skip_lazy_switch(struct task_struct *next, struct pt_regs *regs)
{
	struct rpal_service *tgt;

	tgt = next->rpal_rs;
	if (in_ret_section(tgt, regs->ip)) {
		wrfsbase(current->thread.fsbase);
#ifdef CONFIG_RPAL_PKU
		if (rpal_pku_enabled() && rpal_current_service()->pku_on) {
			rpal_set_current_pkru(
				rpal_pkru_union(
					rpal_pkey_to_pkru(rpal_current_service()->pkey),
					rpal_pkey_to_pkru(next->rpal_rs->pkey)),
				RPAL_PKRU_SET);
		}
#endif
		rebuild_sender_stack(current->rpal_sd, regs);
		rpal_clear_task_thread_flag(next, RPAL_LAZY_SWITCHED_BIT);
		next->rpal_rd->sender = NULL;
		next = NULL;
	}
	return next;
}

static struct task_struct *rpal_fix_critical_section(struct task_struct *next,
						     struct pt_regs *regs)
{
	struct rpal_service *cur = rpal_current_service();

	if (rpal_test_task_thread_flag(next, RPAL_LAZY_SWITCHED_BIT))
		next = rpal_skip_lazy_switch(next, regs);
	/* !RPAL_LAZY_SWITCHED_BIT */
	else if (rpal_is_correct_address(cur, regs->ip)) {
		rpal_skip_receiver_code(next, regs);
#ifdef CONFIG_RPAL_PKU
		if (rpal_pku_enabled() && cur->pku_on)
			write_pkru(rpal_pkru_union(
				rpal_pkey_to_pkru(next->rpal_rs->pkey),
				rdpkru()));
	}
#endif

	return next;
}

static inline struct task_struct *
rpal_kernel_context_switch(struct pt_regs *regs, int offset, bool reserve_stack)
{
	struct task_struct *next = NULL;
	void *src = (void *)regs - offset;
	unsigned long size = sizeof(struct pt_regs) + offset;

	if (rpal_test_current_thread_flag(RPAL_LAZY_SWITCHED_BIT))
		rpal_kernel_wrfsbase(regs);

	next = rpal_misidentify();
	if (unlikely(next != NULL)) {
		next = rpal_fix_critical_section(next, regs);
		if (next)
			next = rpal_do_kernel_context_switch(
				next, regs, src, size, reserve_stack);
	}

	return next;
}

__visible struct task_struct *rpal_syscall_64_context_switch(struct pt_regs *regs, unsigned long nr)
{
	struct task_struct *next;

	next = rpal_kernel_context_switch(regs, 0, false);

	return next;
}

__visible struct task_struct *rpal_exception_context_switch(struct pt_regs *regs)
{
	struct task_struct *next;

	next = rpal_kernel_context_switch(regs, 0, false);

	return next;
}

DEFINE_PER_CPU(bool, rpal_nmi_handle) = false;
DEFINE_PER_CPU(bool, rpal_nmi) = false;

__visible struct task_struct *rpal_nmi_context_switch(struct pt_regs *regs)
{
	struct task_struct *next = NULL;
	void *src = (void *)regs;
	unsigned long size = sizeof(struct pt_regs);

	if (rpal_test_current_thread_flag(RPAL_LAZY_SWITCHED_BIT))
		rpal_kernel_wrfsbase(regs);

	next = rpal_misidentify();
	if (unlikely(next != NULL)) {
		next = rpal_fix_critical_section(next, regs);
		if (next) {
			__this_cpu_write(rpal_nmi_handle, true);
			/* avoid wait in amd_pmu_check_overflow */
			__this_cpu_write(rpal_nmi, true);
			next = rpal_do_kernel_context_switch(next, regs, src,
							     size, false);
			__this_cpu_write(rpal_nmi, false);
		}
	}

	return next;
}

static long rpal_cmd_get_api_version_and_cap(void __user *p)
{
	struct rpal_version_info rvi;
	int ret;

	rvi.compat_version = RPAL_COMPAT_VERSION;
	rvi.api_version = RPAL_API_VERSION;
	rvi.cap = rpal_cap;

	ret = copy_to_user(p, &rvi, sizeof(rvi));
	if (ret)
		goto fail;

	return 0;

fail:
	return -1;
}

static long rpal_cmd_register_thread(unsigned long arg0, unsigned long arg1)
{
	long ret;

	switch (arg1) {
	case RPAL_REGISTER_SENDER_THREAD:
		ret = rpal_register_sender(arg0);
		break;
	case RPAL_REGISTER_RECEIVER_THREAD:
		ret = rpal_register_receiver(arg0);
		break;
	case RPAL_UNREGISTER_SENDER_THREAD:
		ret = rpal_unregister_sender();
		break;
	case RPAL_UNREGISTER_RECEIVER_THREAD:
		ret = rpal_unregister_receiver();
		break;
	default:
		ret = -RPAL_ERR_BAD_ARG;
		break;
	}

	return ret;
}


static void *rpal_uds_peer_data(struct sock *psk, int *pfd)
{
	void *ep = NULL;
	unsigned long flags;
	struct socket_wq *wq;
	wait_queue_entry_t *entry;
	wait_queue_head_t *whead;

	rcu_read_lock();
	wq = rcu_dereference(psk->sk_wq);
	if (!skwq_has_sleeper(wq))
		goto unlock_rcu;

	whead = &wq->wait;

	spin_lock_irqsave(&whead->lock, flags);
	if (list_empty(&whead->head)) {
		pr_debug("rpal debug: [%d] cannot find epitem entry\n",
			 current->pid);
		goto unlock_spin;
	}
	entry = list_first_entry(&whead->head, wait_queue_entry_t, entry);
	*pfd = rpal_get_epitemfd(entry);
	if (*pfd < 0) {
		pr_debug("rpal debug: [%d] cannot find epitem fd\n",
			 current->pid);
		goto unlock_spin;
	}
	ep = rpal_get_epitemep(entry);

unlock_spin:
	spin_unlock_irqrestore(&whead->lock, flags);
unlock_rcu:
	rcu_read_unlock();
	return ep;
}

static int rpal_find_receiver_rid(int id, void *ep)
{
	struct task_struct *tsk;
	struct rpal_service *cur, *tgt;
	int rid = -1;

	cur = rpal_current_service();

	tgt = rpal_get_mapped_service_by_id(cur, id);
	if (tgt == NULL)
		goto out;

	for_each_thread(tgt->leader_thread, tsk) {
		if (!rpal_test_task_thread_flag(tsk, RPAL_IS_RECEIVER_BIT))
			continue;
		if (tsk->rpal_rd->ep == ep) {
			rid = tsk->rpal_rd->rec->rid;
			break;
		}
	}

	rpal_put_service(tgt);
out:
	return rid;
}

static long rpal_uds_fdmap(int service_id, int cfd)
{
	void *ep;
	int sfd = -1;
	int rid = -1;
	long ret = -1;
	struct fd f;
	struct socket *sock;
	struct sock *peer_sk;

	f = fdget(cfd);
	if (!f.file)
		goto fd_put;

	sock = sock_from_file(f.file);
	if (!sock)
		goto fd_put;

	peer_sk = unix_peer_get(sock->sk);
	if (peer_sk == NULL)
		goto fd_put;
	ep = rpal_uds_peer_data(peer_sk, &sfd);
	if (ep == NULL) {
		pr_debug("rpal debug: [%d] cannot find epitem ep\n",
			 current->pid);
		goto peer_sock_put;
	}
	rid = rpal_find_receiver_rid(service_id, ep);
	if (rid < 0) {
		pr_debug("rpal debug: [%d] rpal: cannot find epitem rid\n",
			 current->pid);
		goto peer_sock_put;
	}
	ret = (long)rid << 32 | (long)sfd;

peer_sock_put:
	sock_put(peer_sk);
fd_put:
	if (f.file)
		fdput(f);
	return ret;
}

long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1)
{
	struct rpal_service *cur = rpal_current_service();
	long ret;

	switch (cmd) {
	case RPAL_CMD_GET_API_VERSION_AND_CAP:
		ret = rpal_cmd_get_api_version_and_cap((void __user *)arg0);
		break;
	case RPAL_CMD_GET_SERVICE_KEY:
		ret = (long)cur->key;
		break;
	case RPAL_CMD_REQUEST_SERVICE:
		ret = rpal_request_service((u64)arg0, (void __user *)arg1);
		break;
	case RPAL_CMD_RELEASE_SERVICE:
		ret = rpal_release_service((u64)arg0);
		break;
	case RPAL_CMD_ENABLE_SERVICE:
		ret = rpal_enable_service((void __user *)arg0,
					  (void __user *)arg1, false);
		break;
	case RPAL_CMD_DISABLE_SERVICE:
		ret = rpal_disable_service();
		break;
	case RPAL_CMD_REGISTER_THREAD:
		ret = rpal_cmd_register_thread(arg0, arg1);
		break;
	case RPAL_CMD_UDS_FDMAP:
		ret = rpal_uds_fdmap((int)(arg0), (int)arg1);
		break;
	case RPAL_CMD_GET_SERVICE_ID:
		ret = (long)cur->id;
		break;
	case RPAL_CMD_GET_SERVICE_PKEY:
#ifdef CONFIG_RPAL_PKU
		ret = rpal_pku_enabled() ? cur->pkey : -1;
#else
		ret = -1;
#endif
		break;
	default:
		ret = -RPAL_ERR_BAD_ARG;
		break;
	}
	return ret;
}

static bool check_hardware_features(void)
{
	if (!boot_cpu_has(X86_FEATURE_FSGSBASE)) {
		rpal_err("no fsgsbase feature\n");
		return false;
	}

	if (!arch_pkeys_enabled()) {
		rpal_err("MPK is not enabled\n");
		return false;
	}

	return true;
}

int __init rpal_init(void)
{
	int ret = 0;

	rpal_cap = 0;

	if (boot_cpu_has(X86_FEATURE_NORPAL))
		return 0;

	if (!check_hardware_features())
		goto fail;

	ret = rpal_service_init();
	if (ret)
		goto fail;

	ret = rpal_thread_init();
	if (ret)
		goto thread_init_fail;

	rpal_set_cap(RPAL_CAP_PKU);
	rpal_inited = true;
	return 0;

thread_init_fail:
	rpal_service_exit();
fail:
	rpal_err("rpal init fail\n");
	return -1;
}

subsys_initcall(rpal_init);
