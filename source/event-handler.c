#include "event-handler.h"

#include <stdbool.h>
#include <stdlib.h>
#include <threads.h>
#include <errno.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "Cruzer-S/list/list.h"

#define MAX_EVENTS 1024
#define MAX_WORKER 4

typedef enum event_worker_state {
	EVENT_WORKER_SETUP = 0,
	EVENT_WORKER_READY,
	EVENT_WORKER_RUNNING,
	EVENT_WORKER_ERROR,
} EventWorkerstate;

typedef struct event_worker {
	EventHandler handler;

	struct list object_list;

	struct list to_delete;

	int nobject;
	mtx_t lock;

	int epfd;
	int evfd;
	thrd_t tid;

	EventWorkerstate state;
} *EventWorker;

struct event_object {
	EventWorker worker;

	int fd;
	bool edge_triggered;

	EventCallback callback;
	void *arg;

	int refcnt;

	struct list list;
};

struct event_handler {
	struct event_worker worker[MAX_WORKER];

	EventCallback callback;
	unsigned int next_worker;

	struct {
		cnd_t cond;
		mtx_t mutex;
	} signal, barrier;
};

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	handler->callback = NULL;
	handler->next_worker = 0;

	cnd_init(&handler->signal.cond);
	mtx_init(&handler->signal.mutex, mtx_plain);

	cnd_init(&handler->barrier.cond);
	mtx_init(&handler->barrier.mutex, mtx_plain);
	
	return handler;

FREE_HANDLER:	free(handler);
RETURN_NULL:	return NULL;
}

int event_handler_add(EventHandler handler, EventObject object)
{
	EventWorker worker;
	struct epoll_event event = {
		.events = EPOLLIN || ((object->edge_triggered) ? EPOLLET : 0),
		.data.ptr = object
	};

	handler->next_worker = (handler->next_worker + 1) % MAX_WORKER;
	worker = &handler->worker[handler->next_worker];

	object->worker = worker;

	int retval = epoll_ctl(worker->epfd, EPOLL_CTL_ADD,
			       object->fd, &event);
	if (retval == -1)
		return -1;
	
	list_add(&worker->object_list, &object->list);
	worker->nobject++;

	return 0;
}

int event_handler_del(EventHandler handler, EventObject object)
{
	EventWorker worker;

	worker = object->worker;

	mtx_lock(&worker->lock);

	list_del(&object->list);
	list_init_head(&object->list);
	list_add(&worker->to_delete, &object->list);

	event_object_inc_refcnt(object);

	mtx_unlock(&worker->lock);

	eventfd_write(worker->evfd, 1);
	
	return 0;
}

static void handle_event(EventWorker worker, struct epoll_event *event)
{
	EventObject object;
	EventHandler handler = worker->handler;

	if (event->data.ptr == worker) {
		mtx_lock(&worker->lock);
		LIST_FOREACH_ENTRY_SAFE(&worker->to_delete, object,
					struct event_object, list)
		{
			epoll_ctl(worker->epfd, EPOLL_CTL_DEL, object->fd, 0);
			event_object_destroy(object);
			worker->nobject--;
		}

		list_init_head(&worker->to_delete);

		mtx_unlock(&worker->lock);
		return ;
	}

	object = event->data.ptr;

	if (object->callback)
		object->callback(object);
	else if (handler->callback)
		handler->callback(object);
}

int event_worker_init(EventWorker worker)
{
	worker->epfd = epoll_create1(0);
	if (worker->epfd == -1)
		goto RETURN_ERR;

	worker->evfd = eventfd(0, EFD_NONBLOCK);
	if (worker->evfd == -1)
		goto CLOSE_EPOLL;

	struct epoll_event event = {
		.events = EPOLLIN,
		.data.ptr = worker
	};
	if (epoll_ctl(worker->epfd, EPOLL_CTL_ADD, worker->evfd, &event) == -1)
		goto CLOSE_EVENT;
	
	if (mtx_init(&worker->lock, mtx_plain) != thrd_success)
		goto DELETE_EPOLL;

	list_init_head(&worker->object_list);
	list_init_head(&worker->to_delete);

	worker->nobject = 0;

	return 0;

DELETE_EPOLL:	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);
CLOSE_EVENT:	close(worker->evfd);
CLOSE_EPOLL:	close(worker->epfd);
RETURN_ERR:	return -1;
}

static void event_worker_destroy(EventWorker worker)
{
	mtx_destroy(&worker->lock);
	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);
	close(worker->evfd);
	close(worker->epfd);
}

static void event_worker_barrier(EventWorker worker)
{
	EventHandler handler = worker->handler;

	mtx_lock(&handler->barrier.mutex);

	mtx_lock(&handler->signal.mutex);
	cnd_signal(&handler->signal.cond);
	mtx_unlock(&handler->signal.mutex);

	cnd_wait(&handler->barrier.cond, &handler->barrier.mutex);

	mtx_unlock(&handler->barrier.mutex);
}

static int event_worker(void *arg)
{
	struct epoll_event events[MAX_EVENTS];
		
	EventWorker worker = arg;
	EventHandler handler = worker->handler;

	worker->state = EVENT_WORKER_SETUP;
	if (event_worker_init(worker) == -1)
		goto SEND_ERR_SIGNAL;

	worker->state = EVENT_WORKER_READY;
	event_worker_barrier(worker);
	
	while (worker->state == EVENT_WORKER_RUNNING)
	{
		int ret = epoll_wait(worker->epfd, events, MAX_EVENTS, -1);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			goto DESTROY_WORKER;
		}

		for (int i = 0; i < ret; i++)
			handle_event(worker, events + i);
	}

	LIST_FOREACH_ENTRY_SAFE(&worker->object_list, object,
			 	struct event_object, list)
	{
		list_del(&object->list);

		if (object->callback) {
			object->callback(object);
			event_object_destroy(object);
		}

		worker->nobject--;
	}

	event_worker_destroy(worker);

	return 0;

DESTROY_WORKER:	event_worker_destroy(worker);
SEND_ERR_SIGNAL:worker->state = EVENT_WORKER_ERROR;
		mtx_lock(&handler->signal.mutex);
		cnd_signal(&handler->signal.cond);
		mtx_unlock(&handler->signal.mutex);
		return -1;
}

int event_handler_start(EventHandler handler)
{
	int i;

	for (i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];

		worker->handler = handler;

		mtx_lock(&handler->signal.mutex);

		if (thrd_create(&worker->tid, event_worker, worker) != thrd_success) {
			mtx_unlock(&handler->signal.mutex);
			i--;
			goto SEND_ERR_SIGNAL;
		}

		cnd_wait(&handler->signal.cond, &handler->signal.mutex);

		mtx_unlock(&handler->signal.mutex);

		if (worker->state == EVENT_WORKER_ERROR)
			goto SEND_ERR_SIGNAL;

		worker->state = EVENT_WORKER_RUNNING;
	}

	mtx_lock(&handler->barrier.mutex);
	cnd_broadcast(&handler->barrier.cond);
	mtx_unlock(&handler->barrier.mutex);

	return 0;

SEND_ERR_SIGNAL:mtx_lock(&handler->barrier.mutex);
		cnd_broadcast(&handler->barrier.cond);
		mtx_unlock(&handler->barrier.mutex);
		while (i-- >= 0) {
			EventWorker worker = &handler->worker[i];
			int ret;

			thrd_join(worker->tid, &ret);
		}
		return -1;
}

int event_handler_stop(EventHandler handler)
{
	for (int i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];
		int retval;

		worker->state = EVENT_WORKER_ERROR;

		eventfd_write(worker->evfd, 1);
		thrd_join(worker->tid, &retval);
	}

	return 0;
}

void event_handler_destroy(EventHandler handler)
{
	mtx_destroy(&handler->signal.mutex);
	cnd_destroy(&handler->signal.cond);

	mtx_destroy(&handler->barrier.mutex);
	cnd_destroy(&handler->barrier.cond);

	free(handler);
}

int event_handler_set_callback(EventHandler handler, EventCallback callback)
{
	handler->callback = callback;

	return 0;
}
