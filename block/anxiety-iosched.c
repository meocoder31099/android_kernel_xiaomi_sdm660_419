// SPDX-License-Identifier: GPL-2.0
/*
 * Anxiety I/O Scheduler for Blk-MQ
 *
 * Copyright (c) 2020, Tyler Nijmeh <tylernij@gmail.com>
 */

#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

/* Batch ini banyak permintaan sinkronis sekaligus */
#define	DEFAULT_SYNC_RATIO	(8)

/* Jalankan setiap batch sebanyak ini */
#define DEFAULT_BATCH_COUNT	(4)

struct anxiety_data {
	struct list_head sync_queue;
	struct list_head async_queue;

	/* Tunables */
	uint8_t sync_ratio;
	uint8_t batch_count;

	/* State untuk Blk-MQ pull model */
	uint8_t current_batch;
	uint8_t current_sync_count;
	bool dispatch_async;
};

static void anxiety_requests_merged(struct request_queue *q, struct request *rq,
									struct request *next)
{
	list_del_init(&next->queuelist);
}

static struct request *anxiety_dispatch_request(struct blk_mq_hw_ctx *hctx)
{
	struct elevator_queue *e = hctx->queue->elevator;
	struct anxiety_data *adata = e->elevator_data;
	struct request *rq = NULL;

	if (list_empty(&adata->sync_queue) && list_empty(&adata->async_queue)) {
		adata->current_batch = 0;
		adata->current_sync_count = 0;
		adata->dispatch_async = false;
		return NULL;
	}

	if (adata->current_batch >= adata->batch_count) {
		adata->current_batch = 0;
		adata->current_sync_count = 0;
		adata->dispatch_async = false;
	}

	/* Utamakan dispatch async jika sudah waktunya */
	if (adata->dispatch_async) {
		adata->dispatch_async = false;
		if (!list_empty(&adata->async_queue)) {
			rq = list_first_entry(&adata->async_queue, struct request, queuelist);
			list_del_init(&rq->queuelist);
			adata->current_batch++;
			adata->current_sync_count = 0;
			return rq;
		}
	}

	/* Ambil request dari antrean sync */
	if (!list_empty(&adata->sync_queue) && adata->current_sync_count < adata->sync_ratio) {
		rq = list_first_entry(&adata->sync_queue, struct request, queuelist);
		list_del_init(&rq->queuelist);
		adata->current_sync_count++;
		if (adata->current_sync_count >= adata->sync_ratio) {
			adata->dispatch_async = true;
		}
		return rq;
	}

	/* Mekanisme fallback jika salah satu queue kosong */
	if (!list_empty(&adata->sync_queue)) {
		rq = list_first_entry(&adata->sync_queue, struct request, queuelist);
		list_del_init(&rq->queuelist);
		adata->current_sync_count++;
		if (adata->current_sync_count >= adata->sync_ratio) {
			adata->dispatch_async = true;
		}
		return rq;
	} else if (!list_empty(&adata->async_queue)) {
		rq = list_first_entry(&adata->async_queue, struct request, queuelist);
		list_del_init(&rq->queuelist);
		adata->current_batch++;
		adata->current_sync_count = 0;
		adata->dispatch_async = false;
		return rq;
	}

	return NULL;
}

static void anxiety_insert_requests(struct blk_mq_hw_ctx *hctx, struct list_head *list, bool at_head)
{
	struct elevator_queue *e = hctx->queue->elevator;
	struct anxiety_data *adata = e->elevator_data;

	while (!list_empty(list)) {
		struct request *rq = list_first_entry(list, struct request, queuelist);
		list_del_init(&rq->queuelist);

		if (rq_is_sync(rq)) {
			if (at_head)
				list_add(&rq->queuelist, &adata->sync_queue);
			else
				list_add_tail(&rq->queuelist, &adata->sync_queue);
		} else {
			if (at_head)
				list_add(&rq->queuelist, &adata->async_queue);
			else
				list_add_tail(&rq->queuelist, &adata->async_queue);
		}
	}
}

static int anxiety_init_sched(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq;
	struct anxiety_data *adata;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	adata = kmalloc_node(sizeof(*adata), GFP_KERNEL, q->node);
	if (!adata) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	INIT_LIST_HEAD(&adata->sync_queue);
	INIT_LIST_HEAD(&adata->async_queue);
	adata->sync_ratio = DEFAULT_SYNC_RATIO;
	adata->batch_count = DEFAULT_BATCH_COUNT;
	adata->current_batch = 0;
	adata->current_sync_count = 0;
	adata->dispatch_async = false;

	eq->elevator_data = adata;
	q->elevator = eq;
	return 0;
}

static void anxiety_exit_sched(struct elevator_queue *e)
{
	struct anxiety_data *adata = e->elevator_data;
	kfree(adata);
}

/* Sysfs interface */
static ssize_t anxiety_sync_ratio_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%u\n", adata->sync_ratio);
}

static ssize_t anxiety_sync_ratio_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	int ret = kstrtou8(page, 0, &adata->sync_ratio);
	if (ret < 0)
		return ret;
	return count;
}

static ssize_t anxiety_batch_count_show(struct elevator_queue *e, char *page)
{
	struct anxiety_data *adata = e->elevator_data;
	return snprintf(page, PAGE_SIZE, "%u\n", adata->batch_count);
}

static ssize_t anxiety_batch_count_store(struct elevator_queue *e, const char *page, size_t count)
{
	struct anxiety_data *adata = e->elevator_data;
	int ret = kstrtou8(page, 0, &adata->batch_count);
	if (ret < 0)
		return ret;
	if (adata->batch_count < 1)
		adata->batch_count = 1;
	return count;
}

static struct elv_fs_entry anxiety_attrs[] = {
	__ATTR(sync_ratio, 0644, anxiety_sync_ratio_show, anxiety_sync_ratio_store),
	__ATTR(batch_count, 0644, anxiety_batch_count_show, anxiety_batch_count_store),
	__ATTR_NULL
};

static struct elevator_type elevator_anxiety = {
	.ops.mq = {
		.requests_merged	= anxiety_requests_merged,
		.dispatch_request	= anxiety_dispatch_request,
		.insert_requests	= anxiety_insert_requests,
		.init_sched		= anxiety_init_sched,
		.exit_sched		= anxiety_exit_sched,
	},
	.elevator_name = "anxiety",
	.elevator_attrs = anxiety_attrs,
	.elevator_owner = THIS_MODULE,
};

static int __init anxiety_init(void)
{
	return elv_register(&elevator_anxiety);
}

static void __exit anxiety_exit(void)
{
	elv_unregister(&elevator_anxiety);
}

module_init(anxiety_init);
module_exit(anxiety_exit);

MODULE_AUTHOR("Tyler Nijmeh");
MODULE_LICENSE("GPLv3");
MODULE_DESCRIPTION("Anxiety I/O scheduler for Blk-MQ");
