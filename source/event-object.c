#include "event-object.h"

#include <stdlib.h>

#include "Cruzer-S/list/list.h"

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

	object->refcnt = 1;

	list_init_head(&object->list);

	return object;
}

void event_object_hold(EventObject object)
{
	object->refcnt++;
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
	if (--object->refcnt <= 0)
		free(object);
}
