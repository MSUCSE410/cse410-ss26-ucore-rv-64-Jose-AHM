#ifndef SYNC_H
#define SYNC_H
#include "queue.h"
#include "types.h"

#define WAIT_QUEUE_MAX_LENGTH 16

// A mutex lets only one thread enter a protected section at a time.
struct mutex {
	// 0 means spin mutex: keep trying in a loop.
	// non-zero means blocking mutex: sleep if it is busy.
	uint blocking;
	// 0 means free.
	// non-zero means some thread has the mutex.
	uint locked;
	// Blocking mutex waiters sleep in this queue.
	struct queue wait_queue;
	// "alloc" data for wait queue
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

// A semaphore is a counter for "how many units are free".
struct semaphore {
	// How many resources are free right now.
	// Positive: some are free.
	// Zero or negative: nobody is free, so some thread may have to wait.
	int count;
	// Threads waiting in semaphore_down() sleep here.
	struct queue wait_queue;
	// "alloc" data for wait queue
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

// A condition variable lets one thread sleep until another thread signals it.
struct condvar {
	// Threads waiting for cond_signal() sleep here.
	struct queue wait_queue;
	// "alloc" data for wait queue
	int _wait_queue_data[WAIT_QUEUE_MAX_LENGTH];
};

struct mutex *mutex_create(int blocking);
void mutex_lock(struct mutex *);
void mutex_unlock(struct mutex *);
struct semaphore *semaphore_create(int count);
void semaphore_up(struct semaphore *);
void semaphore_down(struct semaphore *);
struct condvar *condvar_create();
void cond_signal(struct condvar *);
void cond_wait(struct condvar *, struct mutex *);
#endif
