// Main public header file for our user-land support library,
// whose code lives in the lib directory.
// This library is roughly our OS's version of a standard C library,
// and is intended to be linked into all user-mode applications
// (NOT the kernel or boot loader).

#ifndef JOS_INC_LIB_H
#define JOS_INC_LIB_H 1

#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/stdarg.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/memlayout.h>
#include <inc/syscall.h>
#include <inc/trap.h>
#include <inc/fs.h>
#include <inc/fd.h>
#include <inc/args.h>

#define USED(x)		(void)(x)

// main user program
void	umain(int argc, char **argv);

// libmain.c or entry.S
extern const char *binaryname;
extern const volatile struct Env *thisenv;
extern const volatile struct Env envs[NENV];
extern const volatile struct PageInfo pages[];

// exit.c
void	exit(void);

// pgfault.c
void	set_pgfault_handler(void (*handler)(struct UTrapframe *utf));
void	set_pgfault_region_handler(void (*handler)(struct UTrapframe *utf), void *minaddr, void *maxaddr);

// readline.c
char*	readline(const char *buf);

// syscall.c
void	sys_cputs(const char *string, size_t len);
int	sys_cgetc(void);
envid_t	sys_getenvid(void);
int	sys_env_destroy(envid_t);
void	sys_yield(void);
static envid_t sys_exofork(void);
int	sys_env_set_status(envid_t env, int status);
int	sys_env_set_trapframe(envid_t env, struct Trapframe *tf);
int	sys_env_set_pgfault_upcall(envid_t env, void *upcall);
int	sys_env_set_global_pgfault(envid_t env, void *handler);
int	sys_env_set_region_pgfault(envid_t env, void *func, void *minaddr, void *maxaddr);
int	sys_page_alloc(envid_t env, void *pg, int perm);
int	sys_page_map(envid_t src_env, void *src_pg,
		     envid_t dst_env, void *dst_pg, int perm);
int	sys_page_unmap(envid_t env, void *pg);
int	sys_page_reserve(envid_t envid, void *va, int pgnum, int perm);
int	sys_ipc_try_send(envid_t to_env, uint32_t value, void *pg, int perm);
int	sys_ipc_recv(envid_t from_env, void *rcv_pg);

// This must be inlined.  Exercise for reader: why?
static __inline envid_t __attribute__((always_inline))
sys_exofork(void)
{
	envid_t ret;
	__asm __volatile("int %2"
		: "=a" (ret)
		: "a" (SYS_exofork),
		  "i" (T_SYSCALL)
	);
	return ret;
}

// ipc.c
void	ipc_send(envid_t to_env, uint32_t value, void *pg, int perm);
int32_t ipc_recv(envid_t *from_env_store, void *pg, int *perm_store);
envid_t	ipc_find_env(enum EnvType type);

// CHALLENGE: receive message only from the given environment
int32_t ipc_recv_src(envid_t from_env, envid_t *from_env_store, void *pg, int *perm_store);

// fork.c
envid_t	fork(void);
envid_t	sfork(void);	// Challenge!

// fd.c
int	ftruncate(int fd, off_t size);
int	close(int fd);
ssize_t	read(int fd, void *buf, size_t nbytes);
ssize_t	write(int fd, const void *buf, size_t nbytes);
int	seek(int fd, off_t offset);
void	close_all(void);
ssize_t	readn(int fd, void *buf, size_t nbytes);
int	dup(int oldfd, int newfd);
int	fstat(int fd, struct Stat *statbuf);
int	stat(const char *path, struct Stat *statbuf);
int	fgetid(int fd);

// file.c
int	open(const char *path, int mode);
int	remove(const char *path);
int	sync(void);
int     request_block(int fileid, off_t offset, void * dstva, uint32_t perm);

// mmap.c
void *	mmap(void *addr, size_t len, int prot, int flags, int fd_num, off_t off);
int	munmap(void *addr, size_t len);
static void	mmap_shared_handler(struct UTrapframe *utf);
static void	mmap_private_handler(struct UTrapframe *utf);

// pageref.c
int	pageref(void *addr);


// spawn.c
envid_t	spawn(const char *program, const char **argv);
envid_t	spawnl(const char *program, const char *arg0, ...);

// console.c
void	cputchar(int c);
int	getchar(void);
int	iscons(int fd);
int	opencons(void);

// pipe.c
int	pipe(int pipefds[2]);
int	pipeisclosed(int pipefd);

// wait.c
void	wait(envid_t env);

/* PTE bit definitions */
#define	PTE_SHARE	0x400
#define PTE_COW		0x800		/* Copy-on-write page table permissions */

/* File open modes */
#define	O_RDONLY	0x0000		/* open for reading only */
#define	O_WRONLY	0x0001		/* open for writing only */
#define	O_RDWR		0x0002		/* open for reading and writing */
#define	O_ACCMODE	0x0003		/* mask for above modes */

#define	O_CREAT		0x0100		/* create if nonexistent */
#define	O_TRUNC		0x0200		/* truncate to zero length */
#define	O_EXCL		0x0400		/* error if already exists */
#define O_MKDIR		0x0800		/* create directory, not regular file */

/* MMap flags */
#define	MMAPTABLE	0xCFFFF000	/* Bottom of the mmap metadata table. */
#define MAP_PRIVATE	0x0000		/* If set, changes are not written to disk. */
#define MAP_SHARED	0x0001	        /* Updates are visiable and carried through */

/* INDEX2FD definitions */
// Bottom of file descriptor area
#define FDTABLE		0xD0000000
// Return the 'struct Fd*' for file descriptor index i
#define INDEX2FD(i)	((struct Fd*) (FDTABLE + (i)*PGSIZE))

#endif	// !JOS_INC_LIB_H
