#ifndef EVENT_HANDLER_H__
#define EVENT_HANDLER_H__

#include <stdbool.h>

typedef struct event_handler *EventHandler;
typedef void (*EventCallback)(int fd, void *arg);

EventHandler event_handler_create(void);
void event_handler_destroy(EventHandler handler);

// thread-safe
int event_handler_add(EventHandler handler, bool edge_trigger,
		      int fd, void *arg, EventCallback callback);
int event_handler_del(EventHandler handler, int fd);

int event_handler_start(EventHandler handler);
int event_handler_stop(EventHandler handler);

int event_handler_set_callback(EventHandler handler, EventCallback callback);
int event_handler_set_timer(EventHandler handler,
			    int fd, void *arg, EventCallback callback);

#endif
