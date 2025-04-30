/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_RPAL_H_
#define _LINUX_RPAL_H_

#include <linux/hashtable.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/file.h>
#include <linux/page-flags.h>
#include <linux/binfmts.h>

#define RPAL_MAGIC "RPAL"
#define RPAL_MAGIC_OFFSET 12
#define RPAL_MAGIC_LEN 4

#define RPAL_ERROR_MSG "rpal error: "

#define rpal_err(x...) pr_err(RPAL_ERROR_MSG x)
#define rpal_err_ratelimited(x...) pr_err_ratelimited(RPAL_ERROR_MSG x)


/*
 * The first 512GB is reserved due to mmap_min_addr.
 * The last 512GB is dropped since stack will be initially
 * allocated at TASK_SIZE_MAX.
 */
#define RPAL_NR_ID 254
#define RPAL_INVALID_ID -1

struct rpal_service {
	/* Fields below should never change after initialization. */
	char *name;
	/* The struct task_struct of thread group leader. */
	struct task_struct *leader_thread;
	/* address space id of the service which is allocate by rpal_alloc_id(). */
	int id;
	/* key which is unique to each service process */
	u64 key;
	/* mm_struct of the service */
	struct mm_struct *mm;
	/* Fields below may change. */
	spinlock_t lock;
	/* Mutex for service level operations */
	struct mutex mutex;

	/* delayed service put work */
	struct delayed_work delayed_put_work;

	/* Hash table list for this service */
	struct hlist_node hlist;
	atomic_t refcnt;
};


struct rpal_service *rpal_get_service(struct rpal_service *rs);
void rpal_put_service(struct rpal_service *rs);

#if IS_ENABLED(CONFIG_RPAL)
static inline struct rpal_service *rpal_current_service(void)
{
	return current->rpal_rs;
}
void exit_rpal(bool group_dead);
void copy_rpal(struct task_struct *p);
#else
static inline struct rpal_service *rpal_current_service(void) { return NULL; }
static inline void exit_rpal(bool group_dead) { }
static inline void copy_rpal(struct task_struct *p) { }
#endif

/* service.c */
struct rpal_service *rpal_alloc_and_register_service(void);
void rpal_unregister_service(struct rpal_service *rs);
#endif /* _LINUX_RPAL_H_ */
