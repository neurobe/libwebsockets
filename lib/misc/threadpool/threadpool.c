/*
 * libwebsockets - threadpool api
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "core/private.h"

#include <pthread.h>
#include <string.h>
#include <stdio.h>

struct lws_threadpool;

struct lws_threadpool_task {
	struct lws_threadpool_task *task_queue_next;

	struct lws_threadpool *tp;
	char name[32];
	struct lws_threadpool_task_args args;
	time_t created;
	time_t entered_state;

	pthread_cond_t wake_idle;

	enum lws_threadpool_task_status status;
};

struct lws_pool {
	struct lws_threadpool *tp;
	pthread_t thread;
	pthread_mutex_t lock; /* part of task wake_idle */
	struct lws_threadpool_task *task;
	time_t acquired;
	int worker_index;
};

struct lws_threadpool {
	pthread_mutex_t lock; /* protects all pool lists */
	pthread_cond_t wake_idle;
	struct lws_pool *pool_list;

	struct lws_threadpool_task *task_queue_head;
	struct lws_threadpool_task *task_done_head;

	char name[32];

	int threads_in_pool;
	int queue_depth;
	int done_queue_depth;
	int max_queue_depth;
	int running_tasks;

	unsigned int destroying:1;
};

void
lws_threadpool_dump(struct lws_threadpool *tp)
{
#if defined(_DEBUG)
	struct lws_threadpool_task **c;
	time_t now = time(NULL);
	int n, count;

	pthread_mutex_lock(&tp->lock); /* ======================== tpool lock */

	lwsl_notice("%s: tp: %s, Queued: %d, Run: %d, Done: %d\n", __func__,
		    tp->name, tp->queue_depth, tp->running_tasks,
		    tp->done_queue_depth);

	count = 0;
	c = &tp->task_queue_head;
	while (*c) {
		lwsl_notice("  - queued: %s (%ds ago)\n", (*c)->name,
			    (int)(now - (*c)->created));
		count++;

		c = &(*c)->task_queue_next;
	}

	if (count != tp->queue_depth)
		lwsl_err("%s: tp says queue depth %d, but actually %d\n",
			 __func__, tp->queue_depth, count);

	count = 0;
	for (n = 0; n < tp->threads_in_pool; n++) {
		struct lws_pool *pool = &tp->pool_list[n];
		struct lws_threadpool_task *task = pool->task;

		if (task) {
			lwsl_notice("  - worker %d: task: %s (%ds ago),"
				    " state: %d (%ds ago)\n", n, task->name,
				    (int)(now - pool->acquired), task->status,
				    (int)(now - task->entered_state));
			count++;
		}
	}

	if (count != tp->running_tasks)
		lwsl_err("%s: tp says %d running_tasks, but actually %d\n",
			 __func__, tp->running_tasks, count);

	count = 0;
	c = &tp->task_done_head;
	while (*c) {
		lwsl_notice("  - done task %s (age %ds): state %d (%ds ago)\n",
			    (*c)->name, (int)((*c)->created - now),
			    (*c)->status, (int)(now - (*c)->entered_state));
		count++;

		c = &(*c)->task_queue_next;
	}

	if (count != tp->done_queue_depth)
		lwsl_err("%s: tp says done_queue_depth %d, but actually %d\n",
			 __func__, tp->done_queue_depth, count);

	pthread_mutex_unlock(&tp->lock); /* --------------- tp unlock */
#endif
}

static void
state_transition(struct lws_threadpool_task *task,
		 enum lws_threadpool_task_status status)
{
	task->entered_state = time(NULL);
	task->status = status;
}

static void
lws_threadpool_task_cleanup_destroy(struct lws_threadpool_task *task)
{
	if (task->args.cleanup)
		task->args.cleanup(task->args.wsi, task->args.user);

	if (task->args.wsi)
		task->args.wsi->tp_task = NULL;

	lwsl_debug("%s: tp %p: cleaned finished task for wsi %p\n",
		    __func__, task->tp, task->args.wsi);

	lws_free(task);
}

static void
__lws_threadpool_reap(struct lws_threadpool_task *task)
{
	struct lws_threadpool_task **c, *t = NULL;
	struct lws_threadpool *tp = task->tp;

	/* remove the task from the done queue */

	c = &tp->task_done_head;

	while (*c) {
		if ((*c) == task) {
			t = *c;
			*c = t->task_queue_next;
			t->task_queue_next = NULL;
			tp->done_queue_depth--;

			lwsl_debug("%s: tp %s: reaped task wsi %p\n", __func__,
				   tp->name, task->args.wsi);

			break;
		}
		c = &(*c)->task_queue_next;
	}

	if (!t)
		lwsl_err("%s: task %p not in done queue\n", __func__, task);

	/* call the task's cleanup and delete the task itself */

	lws_threadpool_task_cleanup_destroy(task);
}

static int
lws_threadpool_worker_sync(struct lws_pool *pool,
			   struct lws_threadpool_task *task)
{
	enum lws_threadpool_task_status temp;
	struct timespec abstime;
	struct lws *wsi;
	int tries = 15;

	/* block until writable acknowledges */
	lwsl_debug("%s: %p: LWS_TP_RETURN_SYNC in\n", __func__, task);
	pthread_mutex_lock(&pool->lock);


	temp = task->status;
	state_transition(task, LWS_TP_STATUS_SYNCING);
	while (tries--) {
		wsi = task->args.wsi;

		/*
		 * if the wsi is no longer attached to this task, there is nothing we
		 * can sync to usefully.  Since the work wants to sync, it means we
		 * should inform it it can't continue usefully by stopping it.
		 */

		if (!wsi) {
			lwsl_err("%s: %s: %s: No longer bound to any wsi to sync to\n",
				 __func__, pool->tp->name, task->name);

			state_transition(task, LWS_TP_STATUS_STOPPING);
			goto done;
		}

		/*
		 * So this is the maximum time between SYNC asking for a callback on
		 * writable and actually getting it we are willing to sit still for.
		 *
		 * If it is exceeded, we will stop the task.
		 */
		abstime.tv_sec = time(NULL) + 2;
		abstime.tv_nsec = 0;

		lws_callback_on_writable(wsi);

		/*
		 * so the danger here is that we asked for a writable callback
		 * on the wsi, but for whatever reason, we are never going to
		 * get one.  To avoid deadlocking forever, we allow a set time
		 * for the sync to happen naturally, otherwise the cond wait
		 * times out and we stop the task.
		 */

		if (pthread_cond_timedwait(&task->wake_idle, &pool->lock, &abstime) ==
		    ETIMEDOUT) {

			if (!tries) {
				lwsl_err("%s: %s: %s: SYNC timed out (associated wsi %p)\n",
						__func__, pool->tp->name, task->name, task->args.wsi);

				state_transition(task, LWS_TP_STATUS_STOPPING);
				goto done;
			}

			continue;
		} else
			break;
	}

	if (task->status == LWS_TP_STATUS_SYNCING)
		state_transition(task, temp);

	lwsl_debug("%s: %p: LWS_TP_RETURN_SYNC out\n", __func__, task);

done:
	pthread_mutex_unlock(&pool->lock);

	return 0;
}

static void *
lws_threadpool_worker(void *d)
{
	struct lws_threadpool_task **c, **c2, *task;
	struct lws_pool *pool = d;
	struct lws_threadpool *tp = pool->tp;

	while (!tp->destroying) {

		/* we have no running task... wait and get one from the queue */

		pthread_mutex_lock(&tp->lock); /* =================== tp lock */

		/*
		 * if there's no task already waiting in the queue, wait for
		 * the wake_idle condition to signal us that might have changed
		 */
		if (!tp->task_queue_head)
			pthread_cond_wait(&tp->wake_idle, &tp->lock);

		c = &tp->task_queue_head;
		c2 = NULL;
		task = NULL;
		pool->task = NULL;

		/* look at the queue tail */
		while (*c) {
			c2 = c;
			c = &(*c)->task_queue_next;
		}

		/* is there a task at the queue tail? */
		if (c2 && *c2) {
			pool->task = task = *c2;
			pool->acquired = time(NULL);
			/* remove it from the queue */
			*c2 = task->task_queue_next;
			task->task_queue_next = NULL;
			tp->queue_depth--;
			/* mark it as running */
			state_transition(task, LWS_TP_STATUS_RUNNING);
		}

		/* someone else got it first... wait and try again */
		if (!task) {
			pthread_mutex_unlock(&tp->lock);  /* ------ tp unlock */
			continue;
		}

		/* we have acquired a new task */

		lwsl_debug("%s: %s: worker %d: taking task %s for wsi %p\n",
			   __func__, tp->name, pool->worker_index, task->name,
			   task->args.wsi);
		tp->running_tasks++;

		pthread_mutex_unlock(&tp->lock); /* --------------- tp unlock */

		/*
		 * 1) The task can return with LWS_TP_RETURN_CHECKING_IN to
		 * "resurface" periodically, and get called again with
		 * cont = 1 immediately to indicate it is picking up where it
		 * left off if the task is not being "stopped".
		 *
		 * This allows long tasks to respond to requests to stop in
		 * a clean and opaque way.
		 *
		 * 2) The task can return with LWS_TP_RETURN_SYNC to register
		 * a "callback on writable" request on the service thread and
		 * block until it hears back from the WRITABLE handler.
		 *
		 * This allows the work on the thread to be synchronized to the
		 * previous work being dispatched cleanly.
		 *
		 * 3) The task can return with LWS_TP_RETURN_FINISHED to
		 * indicate its work is completed nicely.
		 *
		 * 4) The task can return with LWS_TP_RETURN_STOPPED to indicate
		 * it stopped and cleaned up after incomplete work.
		 */

		do {
			if (tp->destroying || !task->args.wsi)
				state_transition(task, LWS_TP_STATUS_STOPPING);

			switch (task->args.task(task->args.user,
						task->status)) {
			case LWS_TP_RETURN_CHECKING_IN:
				/* if not destroying the tp, continue */
				break;
			case LWS_TP_RETURN_SYNC:
				/* block until writable acknowledges */
				lws_threadpool_worker_sync(pool, task);
				break;
			case LWS_TP_RETURN_FINISHED:
				state_transition(task, LWS_TP_STATUS_FINISHED);
				break;
			case LWS_TP_RETURN_STOPPED:
				state_transition(task, LWS_TP_STATUS_STOPPED);
				break;
			}
		} while (task->status == LWS_TP_STATUS_RUNNING);

		pthread_mutex_lock(&tp->lock); /* =================== tp lock */

		tp->running_tasks--;

		if (pool->task->status == LWS_TP_STATUS_STOPPING)
			state_transition(task, LWS_TP_STATUS_STOPPED);

		/* move the task to the done queue */

		pool->task->task_queue_next = tp->task_done_head;
		tp->task_done_head = task;
		tp->done_queue_depth++;

		if (!pool->task->args.wsi &&
		    (pool->task->status == LWS_TP_STATUS_STOPPED ||
		     pool->task->status == LWS_TP_STATUS_FINISHED))
			/*
			 * there is no longer any wsi attached, so nothing is
			 * going to take care of reaping us.  So we must take
			 * care of it ourselves.
			 */
			__lws_threadpool_reap(task);
		else

			/* signal the associated wsi to take a fresh look at
			 * task status */

			if (pool->task->args.wsi)
				lws_callback_on_writable(pool->task->args.wsi);


		pool->task = NULL;
		pthread_mutex_unlock(&tp->lock); /* --------------- tp unlock */
	}

	/* threadpool is being destroyed */

	pthread_exit(NULL);

	return NULL;
}

struct lws_threadpool *
lws_threadpool_create(const struct lws_threadpool_create_args *args,
		      const char *format, ...)
{
	struct lws_threadpool *tp;
	va_list ap;
	int n;

	tp = lws_malloc(sizeof(*tp) + (sizeof(struct lws_pool) * args->threads),
			"threadpool alloc");
	if (!tp)
		return NULL;

	memset(tp, 0, sizeof(*tp) + (sizeof(struct lws_pool) * args->threads));
	tp->pool_list = (struct lws_pool *)(tp + 1);
	tp->max_queue_depth = args->max_queue_depth;

	va_start(ap, format);
	n = vsnprintf(tp->name, sizeof(tp->name) - 1, format, ap);
	va_end(ap);

	pthread_mutex_init(&tp->lock, NULL);
	pthread_cond_init(&tp->wake_idle, NULL);

	for (n = 0; n < args->threads; n++) {
		tp->pool_list[n].tp = tp;
		tp->pool_list[n].worker_index = n;
		pthread_mutex_init(&tp->pool_list[n].lock, NULL);
		if (pthread_create(&tp->pool_list[n].thread, NULL,
				   lws_threadpool_worker, &tp->pool_list[n])) {
			lwsl_err("thread creation failed\n");
		} else
			tp->threads_in_pool++;
	}

	return tp;
}

void
lws_threadpool_finish(struct lws_threadpool *tp)
{
	struct lws_threadpool_task **c, *task;

	pthread_mutex_lock(&tp->lock); /* ======================== tpool lock */

	/* nothing new can start, running jobs will abort as STOPPED and the
	 * pool threads will exit ASAP (they are joined in destroy) */
	tp->destroying = 1;

	/* stop everyone in the pending queue and move to the done queue */

	c = &tp->task_queue_head;
	while (*c) {
		task = *c;
		*c = task->task_queue_next;
		task->task_queue_next = tp->task_done_head;
		tp->task_done_head = task;
		state_transition(task, LWS_TP_STATUS_STOPPED);
		tp->queue_depth--;
		tp->done_queue_depth++;

		c = &task->task_queue_next;
	}

	pthread_mutex_unlock(&tp->lock); /* -------------------- tpool unlock */

	pthread_cond_broadcast(&tp->wake_idle);
}

void
lws_threadpool_destroy(struct lws_threadpool *tp)
{
	struct lws_threadpool_task *task, *next;
	void *retval;
	int n;

	pthread_mutex_lock(&tp->lock); /* ======================== tpool lock */

	tp->destroying = 1;
	pthread_cond_broadcast(&tp->wake_idle);
	pthread_mutex_unlock(&tp->lock); /* -------------------- tpool unlock */

	lws_threadpool_dump(tp);

	for (n = 0; n < tp->threads_in_pool; n++) {
		task = tp->pool_list[n].task;

		/* he could be sitting waiting for SYNC */

		if (task != NULL)
			pthread_cond_broadcast(&task->wake_idle);

		pthread_join(tp->pool_list[n].thread, &retval);
		pthread_mutex_destroy(&tp->pool_list[n].lock);
	}
	lwsl_info("%s: all threadpools exited\n", __func__);

	task = tp->task_done_head;
	while (task) {
		next = task->task_queue_next;
		lws_threadpool_task_cleanup_destroy(task);
		tp->done_queue_depth--;
		task = next;
	}

	pthread_mutex_destroy(&tp->lock);

	lws_free(tp);
}

/*
 * we want to stop and destroy the task and related priv.  The wsi may no
 * longer exist.
 */

int
lws_threadpool_dequeue(struct lws_threadpool_task *task)
{
	struct lws_threadpool *tp = task->tp;
	struct lws_threadpool_task **c;
	int n;

	pthread_mutex_lock(&tp->lock); /* ======================== tpool lock */

	c = &tp->task_queue_head;

	/* is he queued waiting for a chance to run?  Mark him as stopped and
	 * move him on to the done queue */

	while (*c) {
		if ((*c) == task) {
			*c = task->task_queue_next;
			task->task_queue_next = tp->task_done_head;
			tp->task_done_head = task;
			state_transition(task, LWS_TP_STATUS_STOPPED);
			tp->queue_depth--;
			tp->done_queue_depth++;

			lwsl_debug("%s: tp %p: removed queued task wsi %p\n",
				    __func__, tp, task->args.wsi);

			break;
		}
		c = &(*c)->task_queue_next;
	}

	/* is he on the done queue? */

	c = &tp->task_done_head;
	while (*c) {
		if ((*c) == task) {
			*c = task->task_queue_next;
			task->task_queue_next = NULL;
			lws_threadpool_task_cleanup_destroy(task);
			tp->done_queue_depth--;
			goto bail;
		}
		c = &(*c)->task_queue_next;
	}

	/* he's not in the queue... is he already running on a thread? */

	for (n = 0; n < tp->threads_in_pool; n++) {
		if (!tp->pool_list[n].task || tp->pool_list[n].task != task)
			continue;

		/*
		 * ensure we don't collide with tests or changes in the
		 * worker thread
		 */
		pthread_mutex_lock(&tp->pool_list[n].lock);

		/*
		 * mark him as having been requested to stop...
		 * the caller will hear about it in his service thread
		 * context as a request to close
		 */
		state_transition(task, LWS_TP_STATUS_STOPPING);

		/* disconnect from wsi, and wsi from task */

		task->args.wsi->tp_task = NULL;
		task->args.wsi = NULL;

		pthread_mutex_unlock(&tp->pool_list[n].lock);

		lwsl_debug("%s: tp %p: request stop running task "
			    "for wsi %p\n", __func__, tp, task->args.wsi);

		break;
	}

	if (n == tp->threads_in_pool) {
		/* can't find it */
		lwsl_notice("%s: tp %p: no task for wsi %p, decoupling\n",
			    __func__, tp, task->args.wsi);
		task->args.wsi->tp_task = NULL;
		task->args.wsi = NULL;
	}

bail:
	pthread_mutex_unlock(&tp->lock); /* -------------------- tpool unlock */

	return 0;
}

struct lws_threadpool_task *
lws_threadpool_enqueue(struct lws_threadpool *tp,
		       const struct lws_threadpool_task_args *args,
		       const char *format, ...)
{
	struct lws_threadpool_task *task = NULL;
	va_list ap;

	if (tp->destroying)
		return NULL;

	pthread_mutex_lock(&tp->lock); /* ======================== tpool lock */

	/*
	 * if there's room on the queue, the job always goes on the queue
	 * first, then any free thread may pick it up after the wake_idle
	 */

	if (tp->queue_depth == tp->max_queue_depth) {
		lwsl_notice("%s: queue reached limit %d\n", __func__,
			    tp->max_queue_depth);

		goto bail;
	}

	/*
	 * create the task object
	 */

	task = lws_malloc(sizeof(*task), __func__);
	if (!task)
		goto bail;

	memset(task, 0, sizeof(*task));
	pthread_cond_init(&task->wake_idle, NULL);
	task->args = *args;
	task->tp = tp;
	task->created = time(NULL);

	va_start(ap, format);
	vsnprintf(task->name, sizeof(task->name) - 1, format, ap);
	va_end(ap);

	/*
	 * add him on the tp task queue
	 */

	task->task_queue_next = tp->task_queue_head;
	state_transition(task, LWS_TP_STATUS_QUEUED);
	tp->task_queue_head = task;
	tp->queue_depth++;

	/*
	 * mark the wsi itself as depending on this tp (so wsi close for
	 * whatever reason can clean up)
	 */

	args->wsi->tp_task = task;

	lwsl_debug("%s: tp %s: enqueued task %s, new queue depth %d\n",
		   __func__, tp->name, task->name, tp->queue_depth);

	/* alert any idle thread there's something new on the task list */

	pthread_cond_broadcast(&tp->wake_idle);

bail:
	pthread_mutex_unlock(&tp->lock); /* -------------------- tpool unlock */

	return task;
}

/* this should be called from the service thread */

enum lws_threadpool_task_status
lws_threadpool_task_status(struct lws_threadpool_task *task)
{
	enum lws_threadpool_task_status status;

	if (!task)
		return LWS_TP_STATUS_STOPPED;

	status = task->status;

	if (status == LWS_TP_STATUS_FINISHED ||
	    status == LWS_TP_STATUS_STOPPED) {
		struct lws_threadpool *tp = task->tp;

		pthread_mutex_lock(&tp->lock); /* ================ tpool lock */
		__lws_threadpool_reap(task);
		pthread_mutex_unlock(&tp->lock); /* ------------ tpool unlock */
	}

	return status;
}

void
lws_threadpool_task_sync(struct lws_threadpool_task *task, int stop)
{
	lwsl_debug("%s\n", __func__);

	if (stop)
		state_transition(task, LWS_TP_STATUS_STOPPING);

	pthread_cond_signal(&task->wake_idle);
}
