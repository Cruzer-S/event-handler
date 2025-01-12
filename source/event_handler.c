#include "event_handler.h"

#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

#include "Cruzer-S/list/list.h"

#define EVENT_SIZE 1024

#define EPOLL_ADD(EPFD, FD, PTR) 				\
	epoll_ctl(						\
		EPFD, EPOLL_CTL_ADD, FD,			\
		&(struct epoll_event) {				\
	   		.events = EPOLLIN | EPOLLET,		\
			.data.ptr = PTR 			\
		}						\
	)
#define EPOLL_DEL(EPFD, FD)					\
	epoll_ctl(						\
		EPFD, EPOLL_CTL_DEL, FD, NULL			\
	)

typedef struct event *Event;

struct event {
	int fd;
	EventCallback callback;
	void *arg;

	struct list list;
};

struct event_handler {
	pthread_t worker;
	bool is_running;

	int epfd;
	int evfd;

	struct list events;
	struct list to_delete;

	pthread_spinlock_t event_lock;
};

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	list_init_head(&handler->events);
	list_init_head(&handler->to_delete);

	pthread_spin_init(&handler->event_lock, PTHREAD_PROCESS_PRIVATE);

	handler->epfd = epoll_create(1);
	if (handler->epfd == -1)
		goto FREE_HANDLER;

	handler->evfd = eventfd(0, EFD_NONBLOCK);
	if (handler->evfd == -1)
		goto CLOSE_EPFD;

	if (EPOLL_ADD(handler->epfd, handler->evfd, handler) != 0)
		goto CLOSE_EVFD;

	return handler;

CLOSE_EVFD:	close(handler->evfd);
CLOSE_EPFD:	close(handler->epfd);
FREE_HANDLER:	free(handler);
RETURN_NULL:	return NULL;
}

int event_handler_add(EventHandler handler, int fd,
		      EventCallback callback, void *arg)
{
	Event event;

	event = malloc(sizeof(struct event));
	if (event == NULL)
		return -1;

	event->fd = fd;
	event->callback = callback;
	event->arg = arg;

	if (EPOLL_ADD(handler->epfd, fd, event) != 0) {
		free(event);
		return -1;
	}

	pthread_spin_lock(&handler->event_lock);
	list_init_head(&event->list);
	list_add(&handler->events, &event->list);
	pthread_spin_unlock(&handler->event_lock);

	eventfd_write(handler->evfd, 1);

	return 0;
}

static void *event_handler(void *args)
{
	struct epoll_event events[EVENT_SIZE];
	EventHandler handler = args;
	bool do_cleanup = false;

	while ( handler->is_running )
	{
		int retval = epoll_wait(handler->epfd, events, EVENT_SIZE, -1);
		if (retval == -1) {
			if (errno == EAGAIN)
				continue;

			return NULL;
		}

		for (int i = 0; i < retval; i++) {
			Event event = events[i].data.ptr;

			if (events[i].data.ptr == handler) {
				do_cleanup = true;
				continue;
			}

			event->callback(event->fd, event->arg);
		}

		if (do_cleanup) {
			LIST_FOREACH_ENTRY_SAFE(&handler->to_delete, event,
						struct event, list) 
			{
				pthread_spin_lock(&handler->event_lock);
				list_del(&event->list);
				EPOLL_DEL(handler->epfd, event->fd);
				pthread_spin_unlock(&handler->event_lock);

				free(event);
			}

			eventfd_t value;
			eventfd_write(handler->evfd, 0);
			eventfd_read(handler->evfd, &value);

			do_cleanup = false;
		}
	}

	return handler;
}

int event_handler_start(EventHandler handler)
{
	handler->is_running = true;

	if (pthread_create(&handler->worker, NULL,
		    	   event_handler, handler) != 0)
		return -1;

	return 0;
}

int event_handler_stop(EventHandler handler)
{
	handler->is_running = false;
	eventfd_write(handler->evfd, 1);

	pthread_join(handler->worker, NULL);

	return 0;
}

int event_handler_del(EventHandler handler, int fd)
{
	pthread_spin_lock(&handler->event_lock);

	LIST_FOREACH_ENTRY_SAFE(&handler->events, event, struct event, list)
	{
		if (event->fd == fd) {
			list_del(&event->list);
			list_init_head(&event->list);
			list_add(&handler->to_delete, &event->list);
			pthread_spin_unlock(&handler->event_lock);
			return 0;
		}
	}

	pthread_spin_unlock(&handler->event_lock);

	return -1;
}

void event_handler_destroy(EventHandler handler)
{
	EPOLL_DEL(handler->epfd, handler->evfd);

	close(handler->evfd);
	close(handler->epfd);

	pthread_spin_destroy(&handler->event_lock);

	free(handler);
}
