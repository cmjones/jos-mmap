#include <inc/lib.h>

#define TESTNUM 0
#define DEBUG 1

void
umain(int argc, char **argv)
{
	// mmap(void *addr, size_t len, int prot, int flags, int fd_num, off_t off)
	int fileid;
	struct Fd *fd;
	int r_open;

	size_t length;
	void *mmaped_addr;

	cprintf("\nRunning testmmap...\n");
	// First, open file 'lorem' and get the file id.
	if ((r_open = open("/lorem", O_RDONLY)) < 0)
		panic("mmap(): opening file failed, ERROR CODE: %d \n", r_open);
	fd = INDEX2FD(r_open);
	fileid = fd->fd_file.id;
	
	// Start testing.
	switch (TESTNUM) {
	case 0:
		cprintf("Test directly mmaping a file via fs ipc request.\n");

		extern union Fsipc fsipcbuf;
		envid_t fsenv;
		int r_ipc;
		envid_t envid_store;
		int perm_store;

		// set up the fsipc request
		fsipcbuf.breq.req_fileid = fileid;
		fsipcbuf.breq.req_offset = 0;
		fsipcbuf.breq.req_perm = PTE_U | PTE_W | PTE_SHARE;

		fsenv = ipc_find_env(ENV_TYPE_FS);
		if (DEBUG)
			cprintf("fsenv found: %p \n", fsenv);

		ipc_send(fsenv, FSREQ_BREQ, &fsipcbuf, PTE_P | PTE_U);

		// receive address
		char *content = (char *)0x20005000;
		cprintf("before uvpd: %p\n", uvpd[PDX(content)]);
		if (uvpd[PDX(content)] & PTE_P)
			cprintf("before uvpt: %p\n", uvpt[PGNUM(content)]);

		r_ipc = ipc_recv_src(fsenv, &envid_store, content, &perm_store);

		if (DEBUG) {
			cprintf("testmmap - returned from breq, in hex: %p, in int: %d \n", r_ipc, r_ipc);
			cprintf("testmmap - from returned ipc, content: %p, envid: %p, perm: %p \n", content, envid_store, perm_store);
		}

		cprintf("after uvpd: %p\n", uvpd[PDX(content)]);
		if (uvpd[PDX(content)] & PTE_P) {
			cprintf("after uvpt: %p\n", uvpt[PGNUM(content)] & PTE_SYSCALL);
		}

		cprintf("Read from file:\n\t%30s\n", content);
		break;
	case 1:
		cprintf("Test mmaping file as SHARED, read from it, and print out the content.\n");
		length = PGSIZE;
		mmaped_addr = mmap((void *) NULL, length, PROT_READ, MAP_SHARED, fileid, (off_t) 0);
		char *file_content = (char *) mmaped_addr;
		int counter = 0;
		cprintf(file_content);
		break;
	case 2:
		cprintf("Test mmaping file as SHARED, read from it, print out the content.\n \
		Change some content, read the file again and check the content.\n");
		break;
	case 3:
		cprintf("Test mmaping file as PRIVATE, read from it, and print out the content.\n");
		break;
	case 4:
		cprintf("Test mmaping file as PRIVATE, read from it, print out the content.\n \
		Change some content, read the file again and check the content.\n");
		break;
	default:
		cprintf("No valid test num was specified. Do nothing. \n");
		break;
	}
}
