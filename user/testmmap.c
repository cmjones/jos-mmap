#include <inc/lib.h>

#define DEBUG 1

// 
// mmap the file at the give path to memory.
// returns the address it's mapped to, and the size of memory used (should be the same as the size of file).
//
void
mmap(const char *path, int req_flags, uint32_t req_offset)
{
	if (DEBUG)
		cprintf("mmap() called for path: \"%s\" \n", path);

	extern union Fsipc fsipcbuf;
	envid_t fsenv;
	int fileid;

	envid_t envid_store;
	int perm_store;

	int r_open, r_ipc;

	// open the file to get file descriptor
	if ((r_open = open(path, O_RDONLY)) < 0)
		panic("mmap(): opening file failed, ERROR CODE: %d \n", r_open);
	fileid = r_open;

	// set up the fsipc request
	fsipcbuf.mmap.req_fileid = fileid;
	fsipcbuf.mmap.req_flags = req_flags;
	fsipcbuf.mmap.req_offset = req_offset;

	fsenv = ipc_find_env(ENV_TYPE_FS);

	if (DEBUG)
		cprintf("fsipc request ready with arguments:\n\t req_fileid: %d, req_flags: %x, req_offset: %d, fsenv: %x \n", 
			fsipcbuf.mmap.req_fileid, fsipcbuf.mmap.req_flags, fsipcbuf.mmap.req_offset, fsenv);

	ipc_send(fsenv, FSREQ_MMAP, &fsipcbuf, PTE_P | PTE_U);

	// receive address
	r_ipc = ipc_recv(&envid_store, NULL, &perm_store);

	if (DEBUG) {
		cprintf("mmap(): returned from mmap, in hex: %p, in int: %d \n", r_ipc, r_ipc);
		cprintf("mmap(): from returned ipc, envid: %x, perm: %x \n", envid_store, perm_store);
	}
}


void
umain(int argc, char **argv)
{
	cprintf("--\n");

	cprintf("Running testmmap: mapping files to memory. \n");
	cprintf("mmaping /lorem...\n");
	mmap("/lorem", MAP_PRIVATE, 0);

	cprintf("--\n");
}
