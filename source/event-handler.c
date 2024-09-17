#include "event-handler.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <threads.h>

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
	int nobject;
	mtx_t lock;

	int epfd;
	int evfd;
	thrd_t tid;

	EventWorkerstate state;
} *EventWorker;

typedef struct event_object {
	EventWorker worker;

	int fd;
	void *arg;

	struct list list;

	EventCallback callback;
} *EventObject;

struct event_handler {
	struct event_worker worker[MAX_WORKER];
	mtx_t lock;

	EventCallback callback;

	EventHandlerstate state;
};

static EventWorker find_least_object_worker(EventHandler handler)
{
	EventWorker least = &handler->worker[0];

	for (int i = 1; i < MAX_WORKER; i++) {
		if (handler->worker[i].nobject < least->nobject)
			least = &handler->worker[i];
	}

	return least;
}

static EventObject find_object(EventHandler handler, int fd)
{
	EventWorker worker;

	for (int i = 0; i < MAX_WORKER; i++) {
		worker = &handler->worker[i];

		LIST_FOREACH_ENTRY(&worker->object_list, object,
		     		   struct event_object, list)
			if (object->fd == fd)
				return object;
	}

	return NULL;
}

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	handler->callback = NULL;

	if (mtx_init(&handler->lock, mtx_plain) == -1)
		goto FREE_HANDLER;
	
	return handler;

FREE_HANDLER:	free(handler);
RETURN_NULL:	return NULL;
}

int event_handler_add(EventHandler handler, bool edge_trigger,
		      int fd, void *arg, EventCallback callback)
{
	struct epoll_event event;
	EventObject object;
	EventWorker worker;

	object = malloc(sizeof(struct event_object));
	if (object == NULL)
		return -1;

	object->fd = fd;
	object->arg = arg;
	object->callback = callback;

	event.events = EPOLLIN || ((edge_trigger) ? EPOLLET : 0);
	event.data.ptr = object;

	mtx_lock(&handler->lock);
	
	worker = find_least_object_worker(handler);

	object->worker = worker;

	int retval = epoll_ctl(worker->epfd, EPOLL_CTL_ADD, fd, &event);
	if (retval == -1) {
		free(object);
		mtx_unlock(&handler->lock);
		return -1;
	}
	
	list_add(&worker->object_list, &object->list);
	worker->nobject++;

	mtx_unlock(&handler->lock);

	return 0;
}

int event_handler_del(EventHandler handler, int fd)
{
	EventObject object;
	EventWorker worker;

	mtx_lock(&handler->lock);

	if ((object = find_object(handler, fd)) == NULL)
		goto UNLOCK_MUTEX;

	worker = object->worker;

	mtx_lock(&worker->lock);

	list_del(&object->list);
	list_init_head(&object->list);
	list_add(&worker->to_delete, &object->list);

	mtx_unlock(&worker->lock);

	mtx_unlock(&handler->lock);

	eventfd_write(worker->evfd, 1);
	
	return 0;

UNLOCK_MUTEX:	mtx_unlock(&handler->lock);
		return -1;
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
			free(object);
		}

		list_init_head(&worker->to_delete);

		mtx_unlock(&worker->lock);
		return ;
	}

	object = event->data.ptr;

	if (object->callback)
		object->callback(object->fd, object->arg);
	else if (handler->callback)
		handler->callback(object->fd, object->arg);
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

	while (handler->state == EVENT_HANDLER_RUNNING)
	{
		int ret = epoll_wait(worker->epfd, events, MAX_EVENTS, -1);
		if (ret == -1)
			goto DESTROY_MTX;

		for (int i = 0; i < ret; i++)
			handle_event(worker, handler, events + i);
	}

	LIST_FOREACH_ENTRY_SAFE(&worker->object_list, object,
			 	struct event_object, list)
	{
		list_del(&object->list);
		free(object);
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
	printf("failed to start event_worker\n");
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
	mtx_destroy(&handler->lock);
	free(handler);
}

int event_handler_set_callback(EventHandler handler, EventCallback callback)
{
	handler->callback = callback;

	return 0;
}

int event_handler_set_timer(EventHandler handler, int fd, void *arg, EventCallback callback)
{
	return 0;
}
