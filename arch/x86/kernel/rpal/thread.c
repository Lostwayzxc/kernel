// SPDX-License-Identifier: GPL-2.0-only
#include <linux/rpal.h>
#include <linux/slab.h>
#include <linux/printk.h>

#include <asm/pgtable.h>
#include <asm/fsgsbase.h>

#include "internal.h"

void copy_rpal(struct task_struct *p)
{
	struct rpal_service *cur = rpal_current_service();

	p->rpal_rs = rpal_get_service(cur);
}
