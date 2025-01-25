#include "event_handler.h"

#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>

#include <unistd.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>

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

struct event_handler {
	pthread_t worker;
	bool is_running;

	int epfd;
	int evfd;

	struct list events;
};

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		goto RETURN_NULL;

	list_init_head(&handler->events);

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

int event_handler_add(EventHandler handler, Event event)
{
	list_add(&handler->events, &event->list);

	if (EPOLL_ADD(handler->epfd, event->fd, event) != 0)
		return -1;

	return 0;
}

static void *event_handler(void *args)
{
	struct epoll_event events[EVENT_SIZE];
	EventHandler handler = args;

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
				eventfd_t value;
				eventfd_read(handler->evfd, &value);
				continue;
			}

			event->callback(event->fd, event->arg);
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

int event_handler_del(EventHandler handler, Event event)
{
	list_del(&event->list);
	EPOLL_DEL(handler->epfd, event->fd);

	return 0;
}

void event_handler_destroy(EventHandler handler)
{
	EPOLL_DEL(handler->epfd, handler->evfd);

	close(handler->evfd);
	close(handler->epfd);

	free(handler);
}

struct list *event_handler_get_events(EventHandler handler)
{
	return &handler->events;
}
