#include "event.h"

#include <stdlib.h>

Event event_create(int fd, EventCallback callback, void *arg)
{
	Event event;

	event = malloc(sizeof(struct event));
	if ( !event )
		return NULL;

	event->fd = fd;
	event->callback = callback;
	event->arg = arg;

	list_init_head(&event->list);

	return 0;
}

void event_destroy(Event event)
{
	free(event);
}
