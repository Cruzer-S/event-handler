#include "event-worker.h"

#include "event-handler.h"
#include "event-object.h"

#include <stdlib.h>
#include <errno.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#define MAX_EVENTS 1024

static void *event_worker(void *arg);

EventWorker event_worker_create(EventHandler handler)
{
	EventWorker worker = malloc(sizeof(struct event_worker));
	if (worker == NULL)
		goto RETURN_NULL;

	worker->epfd = epoll_create1(0);
	if (worker->epfd == -1)
		goto FREE_WORKER;

	worker->evfd = eventfd(0, EFD_NONBLOCK);
	if (worker->evfd == -1)
		goto CLOSE_EPOLL;

	struct epoll_event event = {
		.events = EPOLLIN,
		.data.ptr = worker
	};
	if (epoll_ctl(worker->epfd, EPOLL_CTL_ADD, worker->evfd, &event) == -1)
		goto CLOSE_EVENT;

	list_init_head(&worker->add_list);
	list_init_head(&worker->del_list);

	pthread_mutex_init(&worker->lock, NULL);

	worker->nobject = 0;	

	return worker;

DELETE_EPOLL:	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);
		pthread_mutex_destroy(&worker->lock);
CLOSE_EVENT:	close(worker->evfd);
CLOSE_EPOLL:	close(worker->epfd);
FREE_WORKER:	free(worker);
RETURN_NULL:	return NULL;
}

int event_worker_start(EventWorker worker)
{
	worker->state = EVENT_WORKER_RUNNING;
	if (pthread_create(&worker->tid, NULL, event_worker, worker) != 0) {
		worker->state = EVENT_WORKER_ERROR;
		return -1;
	}

	return 0;
}

int event_worker_stop(EventWorker worker)
{
	void *retval;

	worker->state = EVENT_WORKER_STOP;

	eventfd_write(worker->evfd, 1);
	pthread_join(worker->tid, &retval);

	return 0;
}

static void handle_event(EventWorker worker, struct epoll_event *event)
{
	EventObject object;

	if (event->data.ptr == worker) {
		pthread_mutex_lock(&worker->lock);

		LIST_FOREACH_ENTRY_SAFE(&worker->del_list, obj,
					struct event_object, list)
		{
			list_del(&obj->list);
			epoll_ctl(worker->epfd, EPOLL_CTL_DEL, obj->fd, 0);
			worker->nobject--;
		}
		list_init_head(&worker->del_list);

		LIST_FOREACH_ENTRY_SAFE(&worker->add_list, obj,
					struct event_object, list)
		{
			struct epoll_event ev = {
				.data.ptr = obj,
				.events = (obj->edge_triggered) ?
					  EPOLLIN | EPOLLET : EPOLLIN
			};

			epoll_ctl(worker->epfd, EPOLL_CTL_ADD, obj->fd, &ev);

			list_add(&worker->object_list, &obj->list);

			worker->nobject++;
		}
		list_init_head(&worker->add_list);

		pthread_mutex_unlock(&worker->lock);
		return ;
	}

	object = event->data.ptr;

	if (object->callback)
		object->callback(object);
}

void event_worker_destroy(EventWorker worker)
{
	epoll_ctl(worker->epfd, EPOLL_CTL_DEL, worker->evfd, NULL);

	close(worker->evfd);
	close(worker->epfd);

	pthread_mutex_destroy(&worker->lock);
}

static void *event_worker(void *arg)
{
	struct epoll_event events[MAX_EVENTS];
		
	EventWorker worker = arg;

	while (worker->state == EVENT_WORKER_READY)
		sleep(1);
	
	while (worker->state == EVENT_WORKER_RUNNING) {
		int ret = epoll_wait(worker->epfd, events, MAX_EVENTS, -1);
		if (ret == -1) {
			if (errno == EINTR)
				continue;

			goto DESTROY_WORKER;
		}

		for (int i = 0; i < ret; i++)
			handle_event(worker, events + i);
	}

	pthread_mutex_lock(&worker->lock);
	LIST_FOREACH_ENTRY_SAFE(&worker->object_list, obj,
			 	struct event_object, list)
	{
		list_del(&obj->list);

		epoll_ctl(worker->epfd, EPOLL_CTL_DEL, obj->fd, 0);
		if (obj->callback)
			obj->callback(obj);

		worker->nobject--;
	}
	pthread_mutex_lock(&worker->lock);

	event_worker_destroy(worker);

	return 0;

DESTROY_WORKER:	event_worker_destroy(worker);
SEND_ERR_SIGNAL:worker->state = EVENT_WORKER_ERROR;
		return NULL;
}
