#include "event-object.h"

#include <stdlib.h>

#include "Cruzer-S/list/list.h"

typedef struct event_worker *EventWorker;

struct event_object {
	EventWorker worker;

	int fd;
	bool edge_triggered;

	EventCallback callback;
	void *arg;

	struct list list;
};

EventObject event_object_create(int fd, bool edge_triggered,
				void *arg, EventCallback callback)
{
	EventObject object;

	object = malloc(sizeof(struct event_object));
	if (object == NULL)
		return NULL;

	object->worker = NULL;

	object->fd = fd;
	object->edge_triggered = edge_triggered;

	object->callback = callback;
	object->arg = arg;

	list_init_head(&object->list);

	return object;
}

int event_object_get_fd(EventObject object)
{
	return object->fd;
}

void *event_object_get_arg(EventObject object)
{
	return object->arg;
}

int event_object_set_timer(EventObject object, int timeout)
{
	return 0;
}

void event_object_destroy(EventObject object)
{
	free(object);
}

