#ifndef EVENT_OBJECT_H__
#define EVENT_OBJECT_H__

#include <stdbool.h>

typedef struct event_object *EventObject;
typedef void (*EventCallback)(EventObject );

EventObject event_object_create(int fd, bool edge_triggered,
				void *arg, EventCallback );
int event_object_get_fd(EventObject );
void *event_object_get_arg(EventObject );
int event_object_set_timer(EventObject , int timeout);
void event_object_destroy(EventObject );

#endif
