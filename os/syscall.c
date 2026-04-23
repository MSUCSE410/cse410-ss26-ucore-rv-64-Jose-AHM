#include "console.h"
#include "defs.h"
#include "loader.h"
#include "sync.h"
#include "syscall.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

// Special return value for "deadlock detected".
#define DEADLOCK_DETECTED (-0xdead)

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_PIPE:
		return pipewrite(f->pipe, va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_PIPE:
		return piperead(f->pipe, va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_pipe(uint64 fdarray)
{
	struct proc *p = curr_proc();
	uint64 fd0, fd1;
	struct file *f0, *f1;
	if (f0 < 0 || f1 < 0) {
		return -1;
	}
	f0 = filealloc();
	f1 = filealloc();
	if (pipealloc(f0, f1) < 0)
		goto err0;
	fd0 = fdalloc(f0);
	fd1 = fdalloc(f1);
	if (fd0 < 0 || fd1 < 0)
		goto err0;
	if (copyout(p->pagetable, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
	    copyout(p->pagetable, fdarray + sizeof(uint64), (char *)&fd1,
		    sizeof(fd1)) < 0) {
		goto err1;
	}
	return 0;

err1:
	p->files[fd0] = 0;
	p->files[fd1] = 0;
err0:
	fileclose(f0);
	fileclose(f1);
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_thread_create(uint64 entry, uint64 arg)
{
	struct proc *p = curr_proc();
	int tid = allocthread(p, entry, 1);
	if (tid < 0) {
		errorf("fail to create thread");
		return -1;
	}
	struct thread *t = &p->threads[tid];
	t->trapframe->a0 = arg;
	t->state = RUNNABLE;
	add_task(t);
	return tid;
}

int sys_gettid()
{
	return curr_thread()->tid;
}

int sys_waittid(int tid)
{
	if (tid < 0 || tid >= NTHREAD) {
		errorf("unexpected tid %d", tid);
		return -1;
	}
	struct thread *t = &curr_proc()->threads[tid];
	if (t->state == T_UNUSED || tid == curr_thread()->tid) {
		return -1;
	}
	if (t->state != EXITED) {
		return -2;
	}
	//I clear the deadlock bookkeeping when a thread exits
	// so the detector does not think a dead thread still owns or wants resources.
	// Overwrite the old kernel stack so stale thread data is not left behind.
	memset((void *)t->kstack, 7, KSTACK_SIZE);
	// This thread is gone now, so remove any old deadlock records for it.
	// Remove any old "holds" / "waiting for" records for this thread.
	// Clear mutex ownership records for this thread.
	memset(curr_proc()->mutex_allocation[tid], 0,
	       sizeof(curr_proc()->mutex_allocation[tid]));
	// Clear mutex waiting records for this thread.
	memset(curr_proc()->mutex_request[tid], 0,
	       sizeof(curr_proc()->mutex_request[tid]));
	// Clear semaphore ownership counts for this thread.
	memset(curr_proc()->semaphore_allocation[tid], 0,
	       sizeof(curr_proc()->semaphore_allocation[tid]));

	// Clear semaphore waiting records for this thread.
	memset(curr_proc()->semaphore_request[tid], 0,
	       sizeof(curr_proc()->semaphore_request[tid]));
	t->tid = -1;
	t->state = T_UNUSED;
	return t->exit_code;
}

// added deadlock-detection support.
// The helper functions build the current free-resource picture and
// run the deadlock check. Then the mutex and semaphore syscalls update
// bookkeeping for what each thread holds and what it is waiting for.
// If detection is enabled and a new wait would cause deadlock,
// the syscall returns DEADLOCK_DETECTED instead of sleeping.
// Also, when a thread fully exits in sys_waittid, I clear its old
// bookkeeping so dead threads do not appear in the deadlock graph.




// Build a tiny array that says which mutexes are free right now.
//
// available[mid] = 1 means mutex mid is free
// available[mid] = 0 means mutex mid is busy
//
// The deadlock checker uses this as the "what resources can we use right now?" picture.
static void mutex_available_snapshot(const struct proc *p,
				     int available[LOCK_POOL_SIZE])
{
	// Start with everything set to 0.
	memset(available, 0, sizeof(int) * LOCK_POOL_SIZE);

	for (int i = 0; i < p->next_mutex_id; i++) {
		// If the mutex is locked, it is not available.
		// If it is unlocked, it is available.
		available[i] = p->mutex_pool[i].locked ? 0 : 1;
	}
}


// Build a tiny array that says how many free semaphore units exist right now.
//
// available[sid] = current free count of semaphore sid
//
// The deadlock checker uses this as the "free resources" picture for semaphores.
static void semaphore_available_snapshot(const struct proc *p,
					 int available[LOCK_POOL_SIZE])
{
	// Start with everything set to 0.
	memset(available, 0, sizeof(int) * LOCK_POOL_SIZE);

	for (int i = 0; i < p->next_semaphore_id; i++) {
		// Copy the semaphore's current free count.
		available[i] = p->semaphore_pool[i].count;

		// Negative count here only means "threads are waiting".
		// For deadlock checking, treat that as 0 free units.
		if (available[i] < 0) {
			available[i] = 0;
		}
	}
}


// Check whether this thread is part of the deadlock picture at all.
//
// If a thread owns nothing and is waiting for nothing,
// then it does not matter for deadlock checking.
static int row_has_resource(const int allocation[LOCK_POOL_SIZE],
			    const int request[LOCK_POOL_SIZE])
{
	for (int i = 0; i < LOCK_POOL_SIZE; i++) {
		if (allocation[i] != 0 || request[i] != 0) {
			return 1;
		}
	}
	return 0;
}


// Deadlock detection algorithm.
//
// Super simple idea:
// pretend threads finish one by one.
// If a thread can finish, it would give its resources back.
// Keep doing that until no more progress is possible.
//
// If some thread is still stuck at the end,
// then the system is in deadlock.
static int deadlock_detect(const int available[LOCK_POOL_SIZE],
			   const int allocation[NTHREAD][LOCK_POOL_SIZE],
			   const int request[NTHREAD][LOCK_POOL_SIZE])
{
	int work[LOCK_POOL_SIZE];
	int finish[NTHREAD];

	// work = resources that are free right now.
	memmove(work, available, sizeof(work));

	// finish[tid] = 1 means we proved thread tid could finish.
	memset(finish, 0, sizeof(finish));

	for (;;) {
		// found = did we make progress in this round?
		int found = 0;

		for (int tid = 0; tid < NTHREAD; tid++) {
			// Start by assuming this unfinished thread might be able to finish.
			int can_finish = !finish[tid];

			for (int rid = 0; can_finish && rid < LOCK_POOL_SIZE; rid++) {
				// If this thread wants more than we currently have free,
				// then it cannot finish yet.
				if (request[tid][rid] > work[rid]) {
					can_finish = 0;
				}
			}

			if (!can_finish) {
				continue;
			}

			// If this thread can finish, pretend it gives its resources back.
			for (int rid = 0; rid < LOCK_POOL_SIZE; rid++) {
				work[rid] += allocation[tid][rid];
			}

			// Mark this thread as solvable.
			finish[tid] = 1;
			found = 1;
		}

		// Stop when no more threads can be finished.
		if (!found) {
			break;
		}
	}

	// If a thread still cannot finish and it is actually involved,
	// then we found deadlock.
	for (int tid = 0; tid < NTHREAD; tid++) {
		if (!finish[tid] &&
		    row_has_resource(allocation[tid], request[tid])) {
			return 1;
		}
	}

	return 0;
}


// Look at the first waiting thread in a queue without removing it.
//
// This helps before unlock/up, so we can see who will likely wake up next.
static int queue_front_tid(const struct queue *q)
{
	if (q->empty) {
		return -1;
	}

	struct thread *t = id_to_task(q->data[q->front]);
	return t == NULL ? -1 : t->tid;
}


// Create a new mutex for the current process.
//
// Return a small integer id that user space will use later.
int sys_mutex_create(int blocking)
{
	struct mutex *m = mutex_create(blocking);
	if (m == NULL) {
		errorf("fail to create mutex: out of resource");
		return -1;
	}

	// Turn the pointer into a user-visible mutex id.
	int mutex_id = m - curr_proc()->mutex_pool;
	debugf("create mutex %d", mutex_id);
	return mutex_id;
}


// Try to lock a mutex.
//
// If the mutex is free, take it now.
// If the mutex is busy:
// 1. record that this thread is waiting
// 2. if deadlock detection is on, check for deadlock
// 3. if deadlock would happen, fail instead of sleeping
// 4. otherwise do the real lock and sleep if needed
int sys_mutex_lock(int mutex_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();

	if (mutex_id < 0 || mutex_id >= p->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}

	struct mutex *m = &p->mutex_pool[mutex_id];

	if (!m->locked) {
		// The mutex is free, so lock it immediately.
		mutex_lock(m);

		// Record that this thread now owns this mutex.
		p->mutex_allocation[t->tid][mutex_id] = 1;
		return 0;
	}

	// The mutex is busy, so record that this thread is waiting for it.
	p->mutex_request[t->tid][mutex_id] = 1;

	if (p->deadlock_detect_enabled) {
		int available[LOCK_POOL_SIZE];

		// Build the current "free mutex" picture.
		mutex_available_snapshot(p, available);

		// If waiting now would cause deadlock, cancel the wait and fail.
		if (deadlock_detect(available, p->mutex_allocation,
				    p->mutex_request)) {
			p->mutex_request[t->tid][mutex_id] = 0;
			return DEADLOCK_DETECTED;
		}
	}

	// No deadlock found, so do the real lock operation.
	mutex_lock(m);

	// This thread is no longer waiting after the lock succeeds.
	p->mutex_request[t->tid][mutex_id] = 0;

	// Now the thread owns the mutex.
	p->mutex_allocation[t->tid][mutex_id] = 1;
	return 0;
}


// Unlock a mutex.
//
// The current thread gives the mutex up.
// If another waiting thread is about to get it next,
// move the bookkeeping to that thread too.
int sys_mutex_unlock(int mutex_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();

	if (mutex_id < 0 || mutex_id >= p->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}

	struct mutex *m = &p->mutex_pool[mutex_id];
	int next_tid = -1;

	if (m->blocking) {
		// Peek at who will likely wake up next before unlock changes the queue.
		next_tid = queue_front_tid(&m->wait_queue);
	}

	// Do the real unlock.
	mutex_unlock(m);

	// Current thread no longer owns this mutex.
	p->mutex_allocation[t->tid][mutex_id] = 0;

	if (next_tid >= 0) {
		// If a waiting thread got the mutex next, move ownership bookkeeping to it.
		p->mutex_request[next_tid][mutex_id] = 0;
		p->mutex_allocation[next_tid][mutex_id] = 1;
	}

	return 0;
}

// Create a new semaphore for the current process.
//
// Return a small integer id for user space.
int sys_semaphore_create(int res_count)
{
	struct semaphore *s = semaphore_create(res_count);
	if (s == NULL) {
		errorf("fail to create semaphore: out of resource");
		return -1;
	}

	// Turn the pointer into a user-visible semaphore id.
	int sem_id = s - curr_proc()->semaphore_pool;
	debugf("create semaphore %d", sem_id);
	return sem_id;
}


// Give one semaphore unit back.
//
// If another waiting thread wakes up and gets that unit,
// move the bookkeeping to that thread.
int sys_semaphore_up(int semaphore_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();

	if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}

	struct semaphore *s = &p->semaphore_pool[semaphore_id];
	int next_tid = queue_front_tid(&s->wait_queue);

	// Current thread is giving one unit back.
	if (p->semaphore_allocation[t->tid][semaphore_id] > 0) {
		p->semaphore_allocation[t->tid][semaphore_id]--;
	}

	// Do the real semaphore up.
	semaphore_up(s);

	if (next_tid >= 0) {
		// If a waiting thread got the released unit, record that ownership moved.
		p->semaphore_request[next_tid][semaphore_id] = 0;
		p->semaphore_allocation[next_tid][semaphore_id]++;
	}

	return 0;
}


// Try to take one semaphore unit.
//
// If a free unit exists, take it now.
// If no unit is free:
// 1. record that this thread is waiting
// 2. if deadlock detection is on, check for deadlock
// 3. if deadlock would happen, fail instead of sleeping
// 4. otherwise do the real down and possibly sleep
int sys_semaphore_down(int semaphore_id)
{
	struct proc *p = curr_proc();
	struct thread *t = curr_thread();

	if (semaphore_id < 0 || semaphore_id >= p->next_semaphore_id) {
		errorf("Unexpected semaphore id %d", semaphore_id);
		return -1;
	}

	struct semaphore *s = &p->semaphore_pool[semaphore_id];
	int old_count = s->count;

	if (old_count > 0) {
		// A free unit exists, so take it now.
		semaphore_down(s);

		// Record that this thread now owns one unit.
		p->semaphore_allocation[t->tid][semaphore_id]++;
		return 0;
	}

	// No free unit exists, so record that this thread is waiting.
	p->semaphore_request[t->tid][semaphore_id] = 1;

	if (p->deadlock_detect_enabled) {
		int available[LOCK_POOL_SIZE];

		// Build the current "free semaphore units" picture.
		semaphore_available_snapshot(p, available);

		// If waiting now would cause deadlock, cancel the wait and fail.
		if (deadlock_detect(available, p->semaphore_allocation,
			
				    p->semaphore_request)) {
			p->semaphore_request[t->tid][semaphore_id] = 0;
			return DEADLOCK_DETECTED;
		}
	}

	// No deadlock found, so do the real down operation.
	semaphore_down(s);

	// After wakeup, this thread is no longer waiting.
	p->semaphore_request[t->tid][semaphore_id] = 0;

	return 0;
}


// Create a new condition variable for the current process.
int sys_condvar_create()
{
	struct condvar *c = condvar_create();
	if (c == NULL) {
		errorf("fail to create condvar: out of resource");
		return -1;
	}

	// Turn the pointer into a user-visible condition variable id.
	int cond_id = c - curr_proc()->condvar_pool;
	debugf("create condvar %d", cond_id);
	return cond_id;
}


// Wake one thread waiting on this condition variable.
int sys_condvar_signal(int cond_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}

	// Do the real signal.
	cond_signal(&curr_proc()->condvar_pool[cond_id]);
	return 0;
}


// Wait on a condition variable using the given mutex.
//
// This does the normal condvar behavior:
// unlock mutex -> sleep -> wake up -> lock mutex again
int sys_condvar_wait(int cond_id, int mutex_id)
{
	if (cond_id < 0 || cond_id >= curr_proc()->next_condvar_id) {
		errorf("Unexpected condvar id %d", cond_id);
		return -1;
	}

	if (mutex_id < 0 || mutex_id >= curr_proc()->next_mutex_id) {
		errorf("Unexpected mutex id %d", mutex_id);
		return -1;
	}

	// Do the real condvar wait logic.
	cond_wait(&curr_proc()->condvar_pool[cond_id],
		  &curr_proc()->mutex_pool[mutex_id]);
	return 0;
}


// Turn deadlock detection on or off for this process.
//
// 0 = off
// non-zero = on
int sys_enable_deadlock_detect(int enabled)
{
	// Treat any non-zero value as "enabled".
	curr_proc()->deadlock_detect_enabled = enabled != 0;
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_thread()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d args = [%x, %x, %x, %x, %x, %x]", id,
		       args[0], args[1], args[2], args[3], args[4], args[5]);
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	// case SYS_nanosleep:
	// 	ret = sys_nanosleep(args[0]);
	// 	break;
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_pipe2:
		ret = sys_pipe(args[0]);
	case SYS_thread_create:
		ret = sys_thread_create(args[0], args[1]);
		break;
	case SYS_gettid:
		ret = sys_gettid();
		break;
	case SYS_waittid:
		ret = sys_waittid(args[0]);
		break;
	case SYS_mutex_create:
		ret = sys_mutex_create(args[0]);
		break;
	case SYS_mutex_lock:
		ret = sys_mutex_lock(args[0]);
		break;
	case SYS_mutex_unlock:
		ret = sys_mutex_unlock(args[0]);
		break;
	case SYS_semaphore_create:
		ret = sys_semaphore_create(args[0]);
		break;
	case SYS_semaphore_up:
		ret = sys_semaphore_up(args[0]);
		break;
	case SYS_enable_deadlock_detect:
		ret = sys_enable_deadlock_detect(args[0]);
		break;
	case SYS_semaphore_down:
		ret = sys_semaphore_down(args[0]);
		break;
	case SYS_condvar_create:
		ret = sys_condvar_create();
		break;
	case SYS_condvar_signal:
		ret = sys_condvar_signal(args[0]);
		break;
	case SYS_condvar_wait:
		ret = sys_condvar_wait(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	curr_thread()->trapframe->a0 = ret;
	if (id != SYS_write && id != SYS_read && id != SYS_sched_yield) {
		debugf("syscall %d ret %d", id, ret);
	}
}
