#include "event-handler.h"

#include <stdio.h>
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

typedef enum event_handler_state {
	EVENT_HANDLER_READY = 0,
	EVENT_HANDLER_RUNNING,
	EVENT_HANDLER_STOP,

	EVENT_HANDLER_ERROR,
} EventHandlerstate;

typedef enum event_worker_state {
	EVENT_WORKER_READY = 0,
	EVENT_WORKER_RUNNING,
	EVENT_WORKER_ERROR,
} EventWorkerstate;

typedef struct event_worker {
	EventHandler handler;

	struct list object_list;

	struct list to_delete;
	mtx_t lock;

	int nobject;

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

	struct list list;
};

struct event_handler {
	struct event_worker worker[MAX_WORKER];

	EventCallback callback;
	unsigned int next_worker;

	EventHandlerstate state;
};

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	handler->callback = NULL;
	handler->next_worker = 0;
	
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

	mtx_unlock(&worker->lock);

	eventfd_write(worker->evfd, 1);
	
	return 0;
}

static void handle_event(EventWorker worker, EventHandler handler,
			struct epoll_event *event)
{
	EventObject object;

	if (event->data.ptr == worker) {
		mtx_lock(&worker->lock);
		LIST_FOREACH_ENTRY_SAFE(&worker->to_delete, object,
					struct event_object, list)
		{
			epoll_ctl(worker->epfd, EPOLL_CTL_DEL, object->fd, 0);
			worker->nobject--;
			event_object_destroy(object);
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

static int event_worker(void *arg)
{
	struct epoll_event events[MAX_EVENTS];
		
	EventWorker worker = arg;
	EventHandler handler = worker->handler;

	worker->epfd = epoll_create1(0);
	if (worker->epfd == -1)
		goto SET_ERR_STATE;

	worker->evfd = eventfd(0, EFD_NONBLOCK);
	if (worker->evfd == -1)
		goto CLOSE_EPOLL;

	struct epoll_event event;
	event.events = EPOLLIN;
	event.data.ptr = worker;
	if (epoll_ctl(worker->epfd, EPOLL_CTL_ADD, worker->evfd, &event) == -1)
		goto CLOSE_EVENT;

	worker->nobject = 0;
	list_init_head(&worker->object_list);
	list_init_head(&worker->to_delete);

	if (mtx_init(&worker->lock, mtx_plain) == thrd_error)
		goto EPOLL_DEL;

	worker->state = EVENT_WORKER_RUNNING;

	while (handler->state == EVENT_HANDLER_READY)
		sleep(1);

	if (handler->state == EVENT_HANDLER_ERROR)
		goto DESTROY_MTX;

	printf("start worker thread(%ld) - epfd: %d - evfd: %d\n",
		thrd_current() % 10000, worker->epfd, worker->evfd
	);

	while (handler->state == EVENT_HANDLER_RUNNING)
	{
		int ret = epoll_wait(worker->epfd, events, MAX_EVENTS, -1);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			goto DESTROY_MTX;
		}

		for (int i = 0; i < ret; i++)
			handle_event(worker, handler, events + i);
	}

	LIST_FOREACH_ENTRY_SAFE(&worker->object_list, object,
			 	struct event_object, list)
	{
		list_del(&object->list);
		event_object_destroy(object);
		worker->nobject--;
	}

	mtx_destroy(&worker->lock);
	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);

	close(worker->evfd);
	close(worker->epfd);

	return 0;

DESTROY_MTX:	mtx_destroy(&worker->lock);
EPOLL_DEL:	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);
CLOSE_EVENT:	close(worker->evfd);
CLOSE_EPOLL:	close(worker->epfd);
SET_ERR_STATE:	worker->state = EVENT_WORKER_ERROR;
RETURN_ERR:	return -1;
}

static int create_worker(EventHandler handler, EventWorker worker)
{
	thrd_t tid;

	worker->state = EVENT_WORKER_READY;
	worker->handler = handler;

	tid = thrd_create(&worker->tid, event_worker, worker);
	if (tid != thrd_success)
		return -1;

	while (worker->state == EVENT_WORKER_READY)
		sleep(1);

	if (worker->state == EVENT_WORKER_ERROR)
		return -1;

	return 0;
}

int event_handler_start(EventHandler handler)
{
	handler->state = EVENT_HANDLER_READY;

	for (int i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];

		if (create_worker(handler, worker) == -1) {
			handler->state = EVENT_HANDLER_ERROR;

			for (int j = i - 1; j >= 0; j--) {
				int retval;

				worker = &handler->worker[j];
				thrd_join(worker->tid, &retval);
			}

			return -1;
		}
	}

	handler->state = EVENT_HANDLER_RUNNING;

	return 0;
}

int event_handler_stop(EventHandler handler)
{
	handler->state = EVENT_HANDLER_STOP;

	for (int i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];
		int retval;

		eventfd_write(worker->evfd, 1);
		thrd_join(worker->tid, &retval);
	}

	return 0;
}

void event_handler_destroy(EventHandler handler)
{
	free(handler);
}

int event_handler_set_callback(EventHandler handler, EventCallback callback)
{
	handler->callback = callback;

	return 0;
}
