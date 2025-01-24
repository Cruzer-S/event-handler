#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

#include <fcntl.h>
#include <unistd.h>

#include "event_handler.h"

#define MAX_PIPES 256
#define MAX_HANDLER 8
#define MAX_WORKER 8
#define MAX_TESTING 100000

#define print(FMT, ...) 				\
		fprintf(stderr,				\
			"[%5.5ld] " FMT "\n",		\
			pthread_self() % 10000		\
			__VA_OPT__(, __VA_ARGS__)	\
		)

typedef struct handler {
	EventHandler handler;

	atomic_int alloc;
	atomic_int free;
} *Handler;

typedef struct pipe {
	int pfd[2];
	bool closed;

	Handler handler;
	Event event;
} *Pipe;

struct handler handlers[MAX_HANDLER];
pthread_t worker[MAX_WORKER];
struct pipe pipes[MAX_PIPES];
const char *random_data[5] = {
	"ssklahflkhwehrvaksdjf;lvajsdkl;fja;svkodjfk;lawherwehakljrhfaldhfv",
	"vvvvksjf81923jahsdlkfhalshi2h1j2kh4h8s9zxhiab3asdfiuhjh",
	"asdioufhaisdbfk18237819273981273981729837981az98sda98fy98987899vbasd",
	"89123shdfhdskfadskh11bskdjfhkzjhkzjxhc",
	"1023891237ahfszbcvlhvjzcxklhvsdfjlhalksjdfhuqerioyqweouiryl"
};

atomic_int total_pipes = MAX_PIPES * 2;
atomic_int total_read_errors = 0;
atomic_int total_write_errors = 0;
atomic_int total_writes = 0;
atomic_int total_reads = 0;

void callback(int fd, void *args)
{
	Pipe pipe = args;
	Handler handler = pipe->handler;
	char buffer[BUFSIZ];
	int readlen = 0, retval;

	while (readlen < BUFSIZ) {
		retval = read(fd, &buffer[readlen], BUFSIZ - readlen);
		if (retval == -1) {
			if (errno == EAGAIN)
				break;

			print("\tfailed to read(): %s", strerror(errno));

			total_read_errors++;

			return ;
		}

		if (retval == 0) {
			print("\tread close (EOF) signal from %3d", fd);

			event_handler_del(handler->handler, pipe->event);
			close(fd);

			handler->free++;
			total_pipes--;
			
			return ;
		}

		readlen += retval;
		total_reads += retval;
       	}

	print("\tread data %.5s from %3d", buffer, fd);
}

void *worker_thread(void *_)
{
	const char *data;
	Pipe pipe;

	print("start worker thread");

	for (int i = 0; i < MAX_TESTING; i++) {
		pipe = &pipes[rand() % MAX_PIPES];
		data = random_data[rand() % 5];

		if (pipe->closed != 0)
			continue;

		int retval = write(pipe->pfd[1], data, strlen(data) + 1);
		if (retval == -1) {
			print("\tfailed to write(): %s", strerror(errno));
			total_write_errors++;
			continue;
		}

		print("write data %.5s to %3d", data, pipe->pfd[1]);

		total_writes += retval;

		if ((rand() % (MAX_TESTING / 100)) < 10) {
			if (pipe->closed)
				continue;

			pipe->closed = true;
			close(pipe->pfd[1]);

			total_pipes--;
		}
	}

	print("done!");

	return NULL;
}

int main(int argc, char *argv[])
{
	Event event;

	for (int i = 0; i < MAX_HANDLER; i++) {
		Handler hdr = &handlers[i];

		hdr->handler = event_handler_create();
		hdr->free = hdr->alloc = 0;
	}

	for (int i = 0; i < MAX_PIPES; i++) {
		Handler hdr = &handlers[rand() % MAX_HANDLER];
		Pipe pipe = &pipes[i];

		pipe2(pipe->pfd, O_NONBLOCK);
		pipe->closed = false;
		pipe->handler = hdr;

		hdr->alloc++;

		event = event_create(pipe->pfd[0], callback, hdr);
		if ( !event ) {
			close(pipe->pfd[0]);
			close(pipe->pfd[1]);
			continue;
		} else {
			pipe->event = event;
		}

		event_handler_add(hdr->handler, event);
	}

	for (int i = 0; i < MAX_HANDLER; i++)
		event_handler_start(handlers[i].handler);

	for (int i = 0; i < MAX_WORKER; i++)
		pthread_create(&worker[i], NULL, worker_thread, pipes);

	for (int i = 0; i < MAX_WORKER; i++) {
		void *retval;
		print("pthread_join(%ld)...", worker[i] % 10000);
		pthread_join(worker[i], &retval);
	}

	print("remain pipe: %d", total_pipes);

	for (int i = 0; i < MAX_PIPES; i++) {
		Pipe pipe = &pipes[i];

		if (pipe->closed)
			continue;

		event_handler_del(pipe->handler->handler, pipe->event);
		pipe->handler->free++;
		total_pipes--;

		print("close remain pipe %d:%d", pipe->pfd[0], pipe->pfd[1]);
		
		close(pipe->pfd[0]); close(pipe->pfd[1]);
	}

	print("handler's file descriptor: (alloc : free)");
	for (int i = 0; i < MAX_HANDLER; i++)
		print("\t[%d] handler %d : %d", i, handlers[i].alloc,
						   handlers[i].free);

	print("total read errors: %d", total_read_errors);
	print("total write errors: %d", total_write_errors);
	print("total reads: %d", total_reads);
	print("total writes: %d", total_writes);

	for (int i = 0; i < MAX_HANDLER; i++)
		event_handler_stop(handlers[i].handler);

	for (int i = 0; i < MAX_HANDLER; i++)
		event_handler_destroy(handlers[i].handler);

	return 0;
}
