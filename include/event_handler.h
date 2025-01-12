#ifndef EVENT_HANDLER_H__
#define EVENT_HANDLER_H__

typedef void (*EventCallback)(int fd, void *arg);

typedef struct event_handler *EventHandler;

EventHandler event_handler_create(void);
void event_handler_destroy(EventHandler );

int event_handler_add(EventHandler , int fd,
		      EventCallback , void *arg);
int event_handler_del(EventHandler , int fd);

int event_handler_start(EventHandler );
int event_handler_stop(EventHandler );

#endif
