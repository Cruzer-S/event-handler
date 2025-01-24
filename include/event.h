#ifndef EVENT_H__
#define EVENT_H__

#include "Cruzer-S/list/list.h"

typedef void (*EventCallback)(int fd, void *arg);

typedef struct event *Event;

struct event {
	int fd;

	EventCallback callback;
	void *arg;

	struct list list;
};

Event event_create(int fd, EventCallback ,void *arg);
void event_destroy(Event );

#endif
