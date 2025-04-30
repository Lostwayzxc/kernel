// SPDX-License-Identifier: GPL-2.0-only
#include <linux/sched/signal.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/rpal.h>
#include <linux/seq_file.h>
#include <linux/pkeys.h>
#include <asm/page.h>

#include "internal.h"

int rpal_open(struct inode *inode,
			     struct file *file)
{
	return 0;
}

ssize_t rpal_read(struct file *file,
			     char __user *buf, size_t count, loff_t *pos)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_poll_data *rpd;
	u64 released_keys[MAX_REQUEST_SERVICE];
	unsigned long flags;
	int nr_key = 0;
	int nr_byte = 0;
	int idx;

	if (!cur)
		return -EINVAL;

	rpd = &cur->rpd;

	spin_lock_irqsave(&rpd->poll_lock, flags);
	idx = find_first_bit(rpd->dead_key_bitmap, RPAL_NR_ID);
	while (idx < RPAL_NR_ID) {
		released_keys[nr_key++] = rpd->dead_keys[idx];
		idx = find_next_bit(rpd->dead_key_bitmap, RPAL_NR_ID, idx + 1);
	}
	spin_unlock_irqrestore(&rpd->poll_lock, flags);
	nr_byte = nr_key * sizeof(u64);

	if (copy_to_user(buf, released_keys, nr_byte)) {
		nr_byte = -EAGAIN;
		goto out;
	}
out:
	return nr_byte;
}

ssize_t rpal_write(struct file *file,
			     const char __user *buf, size_t count, loff_t *ppos)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_ctl_args args;
	unsigned long res;
	int ret;

	if (!cur)
		return -EINVAL;

	if (count != sizeof(args))
		return -EINVAL;

	ret = copy_from_user(&args, buf, sizeof(args));
	if (ret) {
		pr_debug("rpal: copy arguments from userspace failed: %d\n",
			ret);
		goto out;
	}

	res = rpal_ctl(args.cmd, args.arg0, args.arg1);

	ret = put_user(res, args.ret);
	if (ret) {
		pr_debug("copy result to userspace failed\n");
		force_sig_fault(SIGSEGV, SEGV_MAPERR, (void __user *)args.ret);
		goto out;
	}

out:
	return ret ? -EINVAL : count;
}

__poll_t rpal_poll(struct file *filep,
			     struct poll_table_struct *wait)
{
	struct rpal_service *cur = rpal_current_service();
	struct rpal_poll_data *rpd;
	unsigned long flags;
	__poll_t mask = 0;

	if (unlikely(!cur)) {
		rpal_err("Not a rpal service\n");
		goto out;
	}

	rpd = &cur->rpd;

	poll_wait(filep, &rpd->rpal_waitqueue, wait);

	spin_lock_irqsave(&rpd->poll_lock, flags);
	if (find_first_bit(rpd->dead_key_bitmap, RPAL_NR_ID) < RPAL_NR_ID)
		mask |= EPOLLIN | EPOLLRDNORM;
	spin_unlock_irqrestore(&rpd->poll_lock, flags);

out:
	return mask;
}

const struct proc_ops proc_rpal_operations = {
	.proc_open = rpal_open,
	.proc_read = rpal_read,
	.proc_write = rpal_write,
	.proc_mmap = rpal_mmap,
	.proc_poll = rpal_poll,
};

static int __init proc_rpal_init(void)
{
	if (boot_cpu_has(X86_FEATURE_NORPAL))
		return 0;

	proc_create("rpal", 0644, NULL, &proc_rpal_operations);
	return 0;
}
fs_initcall(proc_rpal_init);
