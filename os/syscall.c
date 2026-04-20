#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "file.h"
#include "fs.h"
#include "loader.h"
#include "proc.h"
#include "string.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

#define BIG_STRIDE 0x7fffffffULL

#define DIR  0x040000
#define FILE 0x100000

typedef struct {
	uint64 dev;
	uint64 ino;
	uint32 mode;
	uint32 nlink;
	uint64 pad[7];
} Stat;

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);

uint64 sys_openat(uint64 path, int flags, int mode);
uint64 sys_close(int fd);
uint64 sys_fstat(int fd, uint64 st);
uint64 sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint flags);
uint64 sys_unlinkat(int dirfd, uint64 path, uint flags);

void freeproc(struct proc *p);

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
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
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
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
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd >= FD_BUFFER_SIZE)
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

uint64 sys_openat(uint64 path, int flags, int mode)
{
        (void)mode;
        struct proc *p = curr_proc();
        char name[MAXPATH];

        if (copyinstr(p->pagetable, name, path, sizeof(name)) < 0)
                return -1;

        return fileopen(name, flags);
}

uint64 sys_close(int fd)
{
	struct proc *p = curr_proc();

	if (fd < 0 || fd >= FD_BUFFER_SIZE)
		return -1;
	if (p->files[fd] == NULL)
		return -1;

	fileclose(p->files[fd]);
	p->files[fd] = NULL;
	return 0;
}

uint64 sys_spawn(uint64 path)
{
	struct proc *p = curr_proc();
	struct proc *np;
	struct inode *ip;
	char name[MAX_STR_LEN];
	char *argv[] = { 0 };

	if (copyinstr(p->pagetable, name, path, sizeof(name)) < 0)
		return -1;

	np = allocproc();
	if (np == 0)
		return -1;

	np->parent = p;

	if (init_stdio(np) < 0) {
		freeproc(np);
		return -1;
	}

	ip = namei(name);
	if (ip == 0) {
		freeproc(np);
		return -1;
	}

	bin_loader(ip, np);
	iput(ip);
	np->trapframe->a0 = push_argv(np, argv);
	np->state = RUNNABLE;
	return np->pid;
}

uint64 sys_set_priority(long long prio)
{
	struct proc *p = curr_proc();

	if (prio < 2)
		return -1;

	p->priority = (uint64)prio;
	p->pass = BIG_STRIDE / p->priority;
	return p->priority;
}

/* compile-safe placeholders for Project 4; replace next */
uint64 sys_fstat(int fd, uint64 st)
{
        struct proc *p = curr_proc();
        struct file *f;
        Stat kst;

        if (fd < 0 || fd >= FD_BUFFER_SIZE)
                return -1;
        if ((f = p->files[fd]) == NULL)
                return -1;
        if (f->type != FD_INODE)
                return -1;

        ivalid(f->ip);

        memset(&kst, 0, sizeof(kst));
        kst.dev = 0;
        kst.ino = f->ip->inum;
        kst.mode = (f->ip->type == T_DIR) ? DIR : FILE;
        kst.nlink = f->ip->nlink;

        if (copyout(p->pagetable, st, (char *)&kst, sizeof(kst)) < 0)
                return -1;

        return 0;
}

uint64 sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint flags)
{
        (void)olddirfd;
        (void)newdirfd;
        (void)flags;

        struct proc *p = curr_proc();
        struct inode *ip, *dp;
        char oldname[MAXPATH], newname[MAXPATH];

        if (copyinstr(p->pagetable, oldname, oldpath, sizeof(oldname)) < 0)
                return -1;
        if (copyinstr(p->pagetable, newname, newpath, sizeof(newname)) < 0)
                return -1;

        if (strncmp(oldname, newname, DIRSIZ) == 0)
                return -1;

        if ((ip = namei(oldname)) == 0)
                return -1;

        ivalid(ip);
        if (ip->type != T_FILE) {
                iput(ip);
                return -1;
        }

        dp = root_dir();
        ivalid(dp);

        if (dirlink(dp, newname, ip->inum) < 0) {
                iput(dp);
                iput(ip);
                return -1;
        }

        ip->nlink++;
        iupdate(ip);

        iput(dp);
        iput(ip);
        return 0;
}

uint64 sys_unlinkat(int dirfd, uint64 path, uint flags)
{
        (void)dirfd;
        (void)flags;

        struct proc *p = curr_proc();
        struct inode *ip, *dp;
        struct dirent de;
        uint off;
        char name[MAXPATH];

        if (copyinstr(p->pagetable, name, path, sizeof(name)) < 0)
                return -1;

        dp = root_dir();
        ivalid(dp);

        if ((ip = dirlookup(dp, name, &off)) == 0) {
                iput(dp);
                return -1;
        }

        ivalid(ip);

        memset(&de, 0, sizeof(de));
        if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de)) {
                iput(ip);
                iput(dp);
                return -1;
        }

        ip->nlink--;
        iupdate(ip);

        iput(dp);
        iput(ip);
        return 0;
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };

	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]",
	       id, args[0], args[1], args[2], args[3], args[4], args[5]);

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
	case SYS_clone:
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
		ret = sys_fstat(args[0], args[1]);
		break;
	case SYS_linkat:
		ret = sys_linkat(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_unlinkat:
		ret = sys_unlinkat(args[0], args[1], args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	case SYS_setpriority:
		ret = sys_set_priority((long long)args[0]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}

	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	(void)flag;
	(void)fd;

	struct proc *p = curr_proc();
	uint64 a;
	int perm = PTE_U;

	if (len == 0) return 0;
	if (start % PGSIZE != 0) return -1;
	if (len > (1ULL << 30)) return -1;
	if ((port & ~0x7) != 0) return -1;
	if ((port & 0x7) == 0) return -1;

	if (port & 0x1) perm |= PTE_R;
	if (port & 0x2) perm |= PTE_W;
	if (port & 0x4) perm |= PTE_X;

	len = PGROUNDUP(len);

	for (a = start; a < start + len; a += PGSIZE) {
		if (walkaddr(p->pagetable, a) != 0) return -1;
	}

	for (a = start; a < start + len; a += PGSIZE) {
		void *mem = kalloc();
		if (mem == 0) return -1;
		memset(mem, 0, PGSIZE);
		if (mappages(p->pagetable, a, PGSIZE, (uint64)mem, perm) < 0) {
			return -1;
		}
	}

	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();
	uint64 a;

	if (len == 0) return 0;
	if (start % PGSIZE != 0) return -1;

	len = PGROUNDUP(len);

	for (a = start; a < start + len; a += PGSIZE) {
		if (walkaddr(p->pagetable, a) == 0) return -1;
	}

	uvmunmap(p->pagetable, start, len / PGSIZE, 1);
	return 0;
}