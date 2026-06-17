/*
 * Zen IO scheduler for Blk-MQ
 * Primarily based on Noop, deadline, and SIO IO schedulers.
 *
 * Copyright (C) 2012 Brandon Berhent <bbedward@gmail.com>
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

enum zen_data_dir { ASYNC, SYNC };

static const int sync_expire  = HZ / 2;
static const int async_expire = 5 * HZ;
static const int fifo_batch = 1;

struct zen_data {
	struct list_head fifo_list[2];
	unsigned int batching;
	int fifo_expire[2];
	int fifo_batch;
};

static void zen_requests_merged(struct request_queue *q, struct request *req, struct request *next)
{
	if (!list_empty(&req->queuelist) && !list_empty(&next->queuelist)) {
		if (time_before((unsigned long)next->fifo_time, (unsigned long)req->fifo_time)) {
			list_move(&req->queuelist, &next->queuelist);
			req->fifo_time = next->fifo_time;
		}
	}
	list_del_init(&next->queuelist);
}

static struct request *zen_expired_request(struct zen_data *zdata, int ddir)
{
	struct request *rq;

	if (list_empty(&zdata->fifo_list[ddir]))
		return NULL;

	rq = list_first_entry(&zdata->fifo_list[ddir], struct request, queuelist);
	if (time_after(jiffies, (unsigned long)rq->fifo_time))
		return rq;

	return NULL;
}

static struct request *zen_check_fifo(struct zen_data *zdata)
{
	struct request *rq_sync = zen_expired_request(zdata, SYNC);
	struct request *rq_async = zen_expired_request(zdata, ASYNC);

	if (rq_async && rq_sync) {
		if (time_after((unsigned long)rq_async->fifo_time, (unsigned long)rq_sync->fifo_time))
			return rq_sync;
	} else if (rq_sync) {
		return rq_sync;
	} else if (rq_async) {
		return rq_async;
	}

	return NULL;
}

static struct request *zen_choose_request(struct zen_data *zdata)
{
	if (!list_empty(&zdata->fifo_list[SYNC]))
		return list_first_entry(&zdata->fifo_list[SYNC], struct request, queuelist);
	if (!list_empty(&zdata->fifo_list[ASYNC]))
		return list_first_entry(&zdata->fifo_list[ASYNC], struct request, queuelist);

	return NULL;
}

static struct request *zen_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct elevator_queue *e = hctx->queue->elevator;
	struct zen_data *zdata = e->elevator_data;
	struct request *rq = NULL;

	if (zdata->batching > zdata->fifo_batch) {
		zdata->batching = 0;
		rq = zen_check_fifo(zdata);
	}

	if (!rq) {
		rq = zen_choose_request(zdata);
		if (!rq)
			return NULL;
	}

	list_del_init(&rq->queuelist);
	zdata->batching++;

	return rq;
}

static void zen_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *list, bool at_head)
{
	struct elevator_queue *e = hctx->queue->elevator;
	struct zen_data *zdata = e->elevator_data;

	while (!list_empty(list)) {
		struct request *rq = list_first_entry(list, struct request, queuelist);
		const int sync = rq_is_sync(rq);

		list_del_init(&rq->queuelist);

		if (zdata->fifo_expire[sync])
			rq->fifo_time = jiffies + zdata->fifo_expire[sync];
		else
			rq->fifo_time = jiffies;

		if (at_head)
			list_add(&rq->queuelist, &zdata->fifo_list[sync]);
		else
			list_add_tail(&rq->queuelist, &zdata->fifo_list[sync]);
	}
}

static int zen_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct zen_data *zdata;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	zdata = kmalloc_node(sizeof(*zdata), GFP_KERNEL, q->node);
	if (!zdata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = zdata;

	INIT_LIST_HEAD(&zdata->fifo_list[SYNC]);
	INIT_LIST_HEAD(&zdata->fifo_list[ASYNC]);
	zdata->fifo_expire[SYNC] = sync_expire;
	zdata->fifo_expire[ASYNC] = async_expire;
	zdata->fifo_batch = fifo_batch;
	zdata->batching = 0;

	q->elevator = eq;
	return 0;
}

static void zen_exit_sched(struct elevator_queue *e)
{
	struct zen_data *zdata = e->elevator_data;
	kfree(zdata);
}

/* Sysfs interface */
static ssize_t zen_var_show(int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t zen_var_store(int *var, const char *page, size_t count)
{
	*var = simple_strtol(page, NULL, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV) \
static ssize_t __FUNC(struct elevator_queue *e, char *page) \
{ \
	struct zen_data *zdata = e->elevator_data; \
	int __data = __VAR; \
	if (__CONV) \
		__data = jiffies_to_msecs(__data); \
		return zen_var_show(__data, (page)); \
}
SHOW_FUNCTION(zen_sync_expire_show, zdata->fifo_expire[SYNC], 1);
SHOW_FUNCTION(zen_async_expire_show, zdata->fifo_expire[ASYNC], 1);
SHOW_FUNCTION(zen_fifo_batch_show, zdata->fifo_batch, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV) \
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count) \
{ \
	struct zen_data *zdata = e->elevator_data; \
	int __data; \
	int ret = zen_var_store(&__data, (page), count); \
	if (__data < (MIN)) \
		__data = (MIN); \
		else if (__data > (MAX)) \
			__data = (MAX); \
			if (__CONV) \
				*(__PTR) = msecs_to_jiffies(__data); \
				else \
					*(__PTR) = __data; \
					return ret; \
}
STORE_FUNCTION(zen_sync_expire_store, &zdata->fifo_expire[SYNC], 0, INT_MAX, 1);
STORE_FUNCTION(zen_async_expire_store, &zdata->fifo_expire[ASYNC], 0, INT_MAX, 1);
STORE_FUNCTION(zen_fifo_batch_store, &zdata->fifo_batch, 0, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
__ATTR(name, 0644, zen_##name##_show, zen_##name##_store)

static struct elv_fs_entry zen_attrs[] = {
	DD_ATTR(sync_expire),
	DD_ATTR(async_expire),
	DD_ATTR(fifo_batch),
	__ATTR_NULL
};

static struct elevator_type iosched_zen = {
	.ops.mq = {
		.requests_merged	= zen_requests_merged,
		.dispatch_request	= zen_dispatch_request,
		.insert_requests	= zen_insert_requests,
		.init_sched		= zen_init_sched,
		.exit_sched		= zen_exit_sched,
	},
	.elevator_attrs = zen_attrs,
	.elevator_name = "zen",
	.elevator_owner = THIS_MODULE,
};

static int __init zen_init(void)
{
	return elv_register(&iosched_zen);
}

static void __exit zen_exit(void)
{
	elv_unregister(&iosched_zen);
}

module_init(zen_init);
module_exit(zen_exit);

MODULE_AUTHOR("Brandon Berhent");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Zen IO scheduler for Blk-MQ");
MODULE_VERSION("1.0");
