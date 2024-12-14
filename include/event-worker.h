#ifndef EVENT_WORKER_H__
#define EVENT_WORKER_H__

#include "event-handler.h"

#include "Cruzer-S/list/list.h"

#include <stdbool.h>

#include <pthread.h>

typedef enum event_worker_state {
	EVENT_WORKER_READY,
	EVENT_WORKER_RUNNING,
	EVENT_WORKER_STOP,
	EVENT_WORKER_ERROR,
} EventWorkerState;

typedef struct event_worker {
	struct list object_list;
	int nobject;

	struct list add_list;
	struct list del_list;
	pthread_mutex_t lock;

	int epfd;
	int evfd;

	pthread_t tid;

	EventWorkerState state;
} *EventWorker;

EventWorker event_worker_create(EventHandler );

int event_worker_start(EventWorker );
int event_worker_stop(EventWorker );

void event_worker_destroy(EventWorker );

#endif
