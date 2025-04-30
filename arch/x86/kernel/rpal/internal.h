/* SPDX-License-Identifier: GPL-2.0 */

#define RPAL_COMPAT_VERSION 1
#define RPAL_API_VERSION 1

/* Error Code */
#define RPAL_ERR_BAD_ARG 1
#define RPAL_ERR_NO_SERVICE 2
#define RPAL_ERR_MAPPED 3
#define RPAL_ERR_RETRY 4
#define RPAL_ERR_BAD_SERVICE_STATUS 5
#define RPAL_ERR_BAD_THREAD_STATUS 6
#define RPAL_ERR_REACH_LIMIT 7
#define RPAL_ERR_NOMEM 8
#define RPAL_ERR_NOMAPPING 9
#define RPAL_ERR_INVAL 10

/* rpal_ctl command */
enum rpal_command_type {
	RPAL_CMD_GET_API_VERSION_AND_CAP,
	RPAL_CMD_GET_SERVICE_KEY,
	RPAL_CMD_REQUEST_SERVICE,
	RPAL_CMD_RELEASE_SERVICE,
	RPAL_CMD_ENABLE_SERVICE,
	RPAL_CMD_DISABLE_SERVICE,
	RPAL_CMD_REGISTER_THREAD,
	RPAL_CMD_UDS_FDMAP,
	RPAL_CMD_GET_SERVICE_ID,
	RPAL_NR_CMD,
};

enum {
	RPAL_REQUEST_MAP,
	RPAL_REVERSE_MAP,
};

enum {
	RPAL_REGISTER_SENDER_THREAD,
	RPAL_REGISTER_RECEIVER_THREAD,
	RPAL_UNREGISTER_SENDER_THREAD,
	RPAL_UNREGISTER_RECEIVER_THREAD,
};

enum rpal_task_status {
	RPAL_TASK_DONE,
	RPAL_TASK_BLOCKED,
	RPAL_TASK_KERNEL_RET,
};

#define RPAL_EP_SID_SHIFT 24
#define RPAL_EP_ID_SHIFT 8
#define RPAL_EP_STATUS_MASK ((1 << RPAL_EP_ID_SHIFT) - 1)
#define RPAL_EP_SID_MASK (~((1 << RPAL_EP_SID_SHIFT) - 1))
#define RPAL_EP_ID_MASK (~(0 | RPAL_EP_STATUS_MASK | RPAL_EP_SID_MASK))
#define RPAL_EP_MAX_ID ((1 << (RPAL_EP_SID_SHIFT - RPAL_EP_ID_SHIFT)) - 1)

extern bool rpal_inited;

static inline struct rpal_shared_page *
rpal_get_shared_page(struct rpal_shared_page *rsp)
{
	atomic_inc(&rsp->refcnt);
	return rsp;
}

static inline void rpal_put_shared_page(struct rpal_shared_page *rsp)
{
	atomic_dec(&rsp->refcnt);
}

static inline unsigned long
rpal_sd_build_ep_app(const struct rpal_sender_data *rsd)
{
	return ((rsd->rcd->service_id << RPAL_EP_SID_SHIFT) |
		(rsd->sec->sender_id << RPAL_EP_ID_SHIFT) | RPAL_EP_APP);
}

static inline bool rpal_is_valid_magic(int magic)
{
	if (magic == RPAL_ERROR_MAGIC)
		return true;

	return false;
}

/* service.c */
int __init rpal_service_init(void);
void rpal_service_exit(void);
long rpal_ctl(unsigned long cmd, unsigned long arg0, unsigned long arg1);
long rpal_request_service(u64 key, void __user *to);
long rpal_release_service(u64 key);
long rpal_enable_service(void __user *u_data, void __user *k_data, bool is_new);
long rpal_disable_service(void);
void rpal_remove_wake_list(struct rpal_service *rs,
			   struct rpal_receiver_data *rrd);
void rpal_insert_wake_list(struct rpal_service *rs,
			   struct rpal_receiver_data *rrd);
struct task_struct *rpal_find_next_task(unsigned long fsbase);
struct rpal_service *rpal_get_mapped_service_by_id(struct rpal_service *rs,
	int id);
struct rpal_service *rpal_get_mapped_service_by_addr(struct rpal_service *rs,
	unsigned long addr);

/* mm.c */
int rpal_mmap(struct file *filp, struct vm_area_struct *vma);
int rpal_map_service(struct rpal_service *tgt);
void rpal_unmap_service(struct rpal_service *tgt);

/* thread.c */
void exit_rpal_thread(void);
long rpal_register_sender(unsigned long addr);
long rpal_unregister_sender(void);
long rpal_register_receiver(unsigned long addr);
long rpal_unregister_receiver(void);
int __init rpal_thread_init(void);
