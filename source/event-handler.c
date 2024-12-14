#include "event-handler.h"

#include "event-worker.h"

#include <stdlib.h>

#include <sys/eventfd.h>

#include "Cruzer-S/list/list.h"

#define MAX_WORKER 4

struct event_handler {
	EventWorker worker[MAX_WORKER];

	EventCallback callback;

	pthread_barrier_t barrier;
};

EventHandler event_handler_create(void)
{
	EventHandler handler;

	handler = malloc(sizeof(struct event_handler));
	if (handler == NULL)
		return NULL;

	handler->callback = NULL;	
	
	return handler;
}

int event_handler_add(EventHandler handler, EventObject object)
{
	EventWorker worker;
	int retval;

	worker = handler->worker[0];
	for (int i = 1; i < MAX_WORKER; i++)
		if (handler->worker[i]->nobject < worker->nobject)
			worker = handler->worker[i];

	object->worker = worker;

	pthread_mutex_lock(&worker->lock);
	list_add(&worker->add_list, &object->list);
	pthread_mutex_unlock(&worker->lock);

	eventfd_write(worker->evfd, 1);

	return 0;
}

int event_handler_del(EventHandler handler, EventObject object)
{
	EventWorker worker;

	worker = object->worker;

	pthread_mutex_lock(&worker->lock);

	event_object_hold(object);

	list_del(&object->list);
	list_init_head(&object->list);
	list_add(&worker->del_list, &object->list);

	pthread_mutex_unlock(&worker->lock);

	eventfd_write(worker->evfd, 1);
	
	return 0;
}

int event_handler_start(EventHandler handler)
{
	int i;

	for (i = 0; i < MAX_WORKER; i++) {
		handler->worker[i] = event_worker_create(handler);
		if (handler->worker[i] == NULL)
			goto STOP_WORKER;

		if (event_worker_start(handler->worker[i]) == -1) {
			event_worker_destroy(handler->worker[i]);
			goto STOP_WORKER;
		}
	}

	return 0;

STOP_WORKER:	while (--i >= 0) {
			event_worker_stop(handler->worker[i]);
			event_worker_destroy(handler->worker[i]);
		}
		return -1;
}

int event_handler_stop(EventHandler handler)
{
	for (int i = 0; i < MAX_WORKER; i++)
		event_worker_stop(handler->worker[i]);

	return 0;
}

void event_handler_destroy(EventHandler handler)
{
	free(handler);
}
