#include "event-handler.h"

#include <stdbool.h>
#include <stdlib.h>
#include <threads.h>

#include <unistd.h>

#include <sys/epoll.h>

#include "Cruzer-S/list/list.h"

#define MAX_WORKER 4

typedef enum event_handler_status {
	EVENT_HANDLER_READY = 0,
	EVENT_HANDLER_RUNNING,
	EVENT_HANDLER_STOP,

	EVENT_HANDLER_ERROR,
} EventHandlerStatus;

typedef enum event_worker_status {
	EVENT_WORKER_READY = 0,
	EVENT_WORKER_RUNNING,
	EVENT_WORKER_ERROR,
} EventWorkerStatus;

typedef struct event_worker {
	EventHandler handler;
	struct list events;

	int epfd;
	thrd_t tid;

	int nobject;

	EventWorkerStatus status;
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

	EventHandlerStatus status;
};

static EventWorker find_least_object_worker(EventHandler handler)
{
	EventWorker least = &handler->worker[0];

	for (int i = 1; i < MAX_WORKER; i++)
		if (handler->worker[i].nobject < least->nobject)
			least = &handler->worker[i];

	return least;
}

static EventWorker find_worker_has_fd(EventHandler handler, int fd)
{
	EventWorker worker;

	for (int i = 0; i < MAX_WORKER; i++) {
		worker = &handler->worker[i];

		LIST_FOREACH_ENTRY(&worker->events, object,
		     		   struct event_object, list)
			if (object->fd == fd)
				return worker;
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

	EventWorker worker;

	worker = find_least_object_worker(handler);

	event.events = EPOLLIN;
	event.data.ptr = arg;

	if (epoll_ctl(worker->epfd, EPOLL_CTL_ADD, fd, &event) == -1)
		return -1;

	return 0;
}

int event_handler_del(EventHandler handler, int fd)
{
	EventWorker worker = find_worker_has_fd(handler, fd);
	if (worker == NULL)
		return -1;

	if (epoll_ctl(worker->epfd, EPOLL_CTL_DEL, fd, 0) == -1)
		return -1;

	return 0;
}

static int event_worker(void *arg)
{
	EventWorker worker = arg;
	EventHandler handler = worker->handler;

	worker->epfd = epoll_create1(0);
	if (worker->epfd == -1)
		goto RETURN_ERR;

	worker->nobject = 0;
	list_init_head(&worker->events);

	while (handler->status == EVENT_HANDLER_READY)
		sleep(1);

	if (handler->status == EVENT_HANDLER_ERROR)
		goto CLOSE_EPOLL;

	while (handler->status == EVENT_HANDLER_RUNNING)
	{
		// do something
	}

	close(worker->epfd);

	return 0;

CLOSE_EPOLL:	close(worker->epfd);
RETURN_ERR:	return -1;

}

int event_handler_start(EventHandler handler)
{
	handler->status = EVENT_HANDLER_READY;

	for (int i = 0; i < MAX_WORKER; i++) {
		EventWorker worker = &handler->worker[i];

		worker->status = EVENT_WORKER_READY;
		thrd_t tid = thrd_create(&worker->tid, event_worker, worker);
		if (tid != thrd_success)
			goto HANDLE_ERROR;

		while (worker->status == EVENT_WORKER_READY)
			sleep(1);

		if (worker->status == EVENT_WORKER_ERROR)
			goto HANDLE_ERROR;

		continue;

	HANDLE_ERROR:
		handler->status = EVENT_HANDLER_ERROR;

		for (int j = i - 1; j >= 0; j--) {
			int retval;

			worker = &handler->worker[j];
			thrd_join(worker->tid, &retval);
		}

		return -1;
	}

	handler->status = EVENT_HANDLER_RUNNING;

	return 0;
}

int event_handler_stop(EventHandler handler)
{
	handler->status = EVENT_HANDLER_STOP;

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
