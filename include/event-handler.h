#ifndef EVENT_HANDLER_H__
#define EVENT_HANDLER_H__

#include <stdbool.h>

#include "event-object.h"

typedef struct event_handler *EventHandler;

EventHandler event_handler_create(void);
void event_handler_destroy(EventHandler );

int event_handler_add(EventHandler , EventObject );
int event_handler_del(EventHandler , EventObject );

int event_handler_start(EventHandler );
int event_handler_stop(EventHandler );

int event_handler_set_callback(EventHandler , EventCallback );

#endif
