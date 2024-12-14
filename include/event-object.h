#ifndef EVENT_OBJECT_H__
#define EVENT_OBJECT_H__

#include <stdbool.h>

#include "Cruzer-S/list/list.h"

typedef struct event_object *EventObject;
typedef void (*EventCallback)(EventObject );

typedef struct event_worker *EventWorker;

struct event_object {
	EventWorker worker;
	int fd;
	bool edge_triggered;

	EventCallback callback;
	void *arg;

	int refcnt;

	struct list list;
};

EventObject event_object_create(int fd, bool edge_triggered,
				void *arg, EventCallback );
int event_object_get_fd(EventObject );
void *event_object_get_arg(EventObject );
int event_object_set_timer(EventObject , int timeout);
void event_object_destroy(EventObject );

void event_object_hold(EventObject object);

#endif
