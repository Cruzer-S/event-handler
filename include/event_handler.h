#ifndef EVENT_HANDLER_H__
#define EVENT_HANDLER_H__

#include "event.h"

typedef struct event_handler *EventHandler;

EventHandler event_handler_create(void);
void event_handler_destroy(EventHandler );

int event_handler_add(EventHandler , Event );
int event_handler_del(EventHandler , Event );

int event_handler_start(EventHandler );
int event_handler_stop(EventHandler );

struct list *event_handler_get_events(EventHandler );

#endif
