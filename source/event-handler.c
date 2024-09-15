#include "event-handler.h"

#include <stdbool.h>
#include <stdlib.h>
#include <threads.h>

#include <unistd.h>

#include <sys/epoll.h>

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
	struct list events;

	int epfd;
	thrd_t tid;

	int nobject;

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

	EventCallback callback;

	EventHandlerstate state;
};

static EventWorker find_least_object_worker(EventHandler handler)
{
	EventWorker least = &handler->worker[0];

	for (int i = 1; i < MAX_WORKER; i++)
		if (handler->worker[i].nobject < least->nobject)
			least = &handler->worker[i];

	return least;
}

static EventObject find_object_from(EventHandler handler, int fd)
{
	EventWorker worker;

	for (int i = 0; i < MAX_WORKER; i++) {
		worker = &handler->worker[i];

		LIST_FOREACH_ENTRY(&worker->events, object,
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
	
	return handler;

FREE_HANDLER:	free(handler);
RETURN_NULL:	return NULL;
}

int event_handler_add(EventHandler handler,
		      int fd, void *arg, EventCallback callback)
{
	struct epoll_event event;
	EventObject object;
	EventWorker worker;

	worker = find_least_object_worker(handler);

	object = malloc(sizeof(struct event_object));
	if (object == NULL)
		return -1;

	object->worker = worker;
	object->fd = fd;
	object->arg = arg;
	object->callback = callback;

	event.events = EPOLLIN;
	event.data.ptr = object;

	if (epoll_ctl(worker->epfd, EPOLL_CTL_ADD, fd, &event) == -1) {
		free(object);
		return -1;
	}

	list_add(&worker->events, &object->list);

	return 0;
}

int event_handler_del(EventHandler handler, int fd)
{
	EventObject object = find_object_from(handler, fd);
	if (object == NULL)
		return -1;

	if (epoll_ctl(object->worker->epfd, EPOLL_CTL_DEL, fd, 0) == -1)
		return -1;

	list_del(&object->list);
	free(object);

	return 0;
}

static int event_worker(void *arg)
{
	struct epoll_event events[MAX_EVENTS];

	EventWorker worker = arg;
	EventHandler handler = worker->handler;

	worker->epfd = epoll_create1(0);
	if (worker->epfd == -1)
		goto SET_ERR_STATE;

	worker->nobject = 0;
	list_init_head(&worker->events);

	worker->state = EVENT_WORKER_RUNNING;

	while (handler->state == EVENT_HANDLER_READY)
		sleep(1);

	if (handler->state == EVENT_HANDLER_ERROR)
		goto CLOSE_EPOLL;

	while (handler->state == EVENT_HANDLER_RUNNING)
	{
		int ret = epoll_wait(worker->epfd, events, MAX_EVENTS, -1);
		if (ret == -1)
			goto CLOSE_EPOLL;

		for (int i = 0; i < ret; i++) {
			EventObject object = events[i].data.ptr;

			object->callback(object->fd, object->arg);
		}
	}

	close(worker->epfd);

	return 0;

CLOSE_EPOLL:	close(worker->epfd);
SET_ERR_STATE:	worker->state = EVENT_WORKER_ERROR;
RETURN_ERR:	return -1;

}

int event_handler_start(EventHandler handler)
{
	handler->state = EVENT_HANDLER_READY;

	for (int i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];

		worker->handler = handler;
		worker->state = EVENT_WORKER_READY;

		thrd_t tid = thrd_create(&worker->tid, event_worker, worker);
		if (tid != thrd_success)
			goto HANDLE_ERROR;

		while (worker->state == EVENT_WORKER_READY)
			sleep(1);

		if (worker->state == EVENT_WORKER_ERROR)
			goto HANDLE_ERROR;

		continue;

	HANDLE_ERROR:
		handler->state = EVENT_HANDLER_ERROR;

		for (int j = i - 1; j >= 0; j--) {
			int retval;

			worker = &handler->worker[j];
			thrd_join(worker->tid, &retval);
		}

		return -1;
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

		thrd_join(worker->tid, &retval);
	}

	return 0;
}

void event_handler_destroy(EventHandler handler)
{
	free(handler);
}
