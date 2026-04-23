#include "defs.h"
#include "proc.h"
#include "sync.h"

// Make one mutex for the current process.
struct mutex *mutex_create(int blocking)
{
	struct proc *p = curr_proc();
	if (p->next_mutex_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	// Use the next free slot in this process's mutex array.
	struct mutex *m = &p->mutex_pool[p->next_mutex_id];
	// Reserve that id so the next create gets the next slot.
	p->next_mutex_id++;
	// Remember whether this mutex should spin or sleep.
	m->blocking = blocking;
	// New mutex starts out unlocked.
	m->locked = 0;
	if (blocking) {
		// Blocking mutexes need a wait queue for sleeping threads.
		init_queue(&m->wait_queue, WAIT_QUEUE_MAX_LENGTH,
			   m->_wait_queue_data);
	}
	return m;
}

// Lock a mutex.
// If it is free, take it now.
// If it is busy, either spin or sleep depending on the mutex type.
void mutex_lock(struct mutex *m)
{
	if (!m->locked) {
		// Mark the mutex as taken.
		m->locked = 1;
		debugf("lock a free mutex");
		return;
	}
	if (!m->blocking) {
		// Spin mutex: keep checking until it becomes free.
		debugf("try to lock spin mutex");
		while (m->locked) {
			yield();
		}
		// When the loop ends, the lock became free.
		debugf("lock spin mutex after some trials");
		return;
	}
	// Blocking mutex: sleep instead of wasting CPU time.
	struct thread *t = curr_thread();
	// Put this thread into the mutex wait queue.
	push_queue(&m->wait_queue, task_to_id(t));
	// Mark the thread as sleeping so the scheduler will skip it.
	t->state = SLEEPING;
	debugf("block to wait for mutex");
	// Give up the CPU until someone wakes this thread.
	sched();
	debugf("blocking mutex passed to me");
	// After wakeup, the unlock path already handed the mutex to this thread.
}

// Unlock a mutex.
// For blocking mutexes, wake one waiting thread if there is one.
void mutex_unlock(struct mutex *m)
{
	if (m->blocking) {
		// Take the next waiting thread out of the queue.
		struct thread *t = id_to_task(pop_queue(&m->wait_queue));
		if (t == NULL) {
			// No waiting thread: just mark the mutex free.
			m->locked = 0;
			debugf("blocking mutex released");
		} else {
			// Wake the next thread and let it run later.
			t->state = RUNNABLE;
			add_task(t);
			debugf("blocking mutex passed to thread %d", t->tid);
		}
	} else {
		// Spin mutex unlock is only "make it free again".
		m->locked = 0;
		debugf("spin mutex unlocked");
	}
}

// Make one semaphore for the current process.
struct semaphore *semaphore_create(int count)
{
	struct proc *p = curr_proc();
	if (p->next_semaphore_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	// Use the next free slot in this process's semaphore array.
	struct semaphore *s = &p->semaphore_pool[p->next_semaphore_id];
	// Reserve that id.
	p->next_semaphore_id++;
	// Save the starting resource count.
	s->count = count;
	// Threads that cannot continue after down() wait here.
	init_queue(&s->wait_queue, WAIT_QUEUE_MAX_LENGTH, s->_wait_queue_data);
	return s;
}

// semaphore_up():
// give one resource back.
// If a thread is waiting, wake one up.
void semaphore_up(struct semaphore *s)
{
	// One more unit is available now.
	s->count++;
	if (s->count <= 0) {
		// count <= 0 means there was at least one waiting thread.
		struct thread *t = id_to_task(pop_queue(&s->wait_queue));
		if (t == NULL) {
			panic("count <= 0 after up but wait queue is empty?");
		}
		// Wake that thread so it can continue.
		t->state = RUNNABLE;
		add_task(t);
		debugf("semaphore up and notify another task");
	}
	debugf("semaphore up from %d to %d", s->count - 1, s->count);
}

// semaphore_down():
// try to take one resource.
// If none are free, go to sleep.
void semaphore_down(struct semaphore *s)
{
	// Try to take one unit.
	s->count--;
	if (s->count < 0) {
		// Negative means there was not a free unit for this thread.
		struct thread *t = curr_thread();
		// Put this thread into the semaphore wait queue.
		push_queue(&s->wait_queue, task_to_id(t));
		// Mark it sleeping.
		t->state = SLEEPING;
		debugf("semaphore down to %d and wait...", s->count);
		// Give up the CPU until semaphore_up() wakes it.
		sched();
		debugf("semaphore up to %d and wake up", s->count);
	}
	debugf("finish semaphore_down with count = %d", s->count);
}

// Make one condition variable for the current process.
struct condvar *condvar_create()
{
	struct proc *p = curr_proc();
	if (p->next_condvar_id >= LOCK_POOL_SIZE) {
		return NULL;
	}
	// Use the next free slot in this process's condvar array.
	struct condvar *c = &p->condvar_pool[p->next_condvar_id];
	// Reserve that id.
	p->next_condvar_id++;
	init_queue(&c->wait_queue, WAIT_QUEUE_MAX_LENGTH, c->_wait_queue_data);
	return c;
}

// Wake one thread waiting on this condition variable.
void cond_signal(struct condvar *cond)
{
	// Take one thread out of the wait queue.
	struct thread *t = id_to_task(pop_queue(&cond->wait_queue));
	if (t) {
		// Wake it up.
		t->state = RUNNABLE;
		add_task(t);
		debugf("signal wake up thread %d", t->tid);
	} else {
		debugf("dummpy signal");
	}
}

// Wait on a condition variable.
// Usual rule:
// 1. unlock the mutex
// 2. sleep
// 3. lock the mutex again after wakeup
void cond_wait(struct condvar *cond, struct mutex *m)
{
	// Let go of the mutex before sleeping.
	mutex_unlock(m);
	struct thread *t = curr_thread();
	// Join the condvar wait queue.
	push_queue(&cond->wait_queue, task_to_id(t));
	// Mark this thread as sleeping.
	t->state = SLEEPING;
	debugf("wait for cond");
	// Give up the CPU until cond_signal() wakes us.
	sched();
	debugf("wake up from cond");
	// Lock the mutex again before returning to the caller.
	mutex_lock(m);
}
