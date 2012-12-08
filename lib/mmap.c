#include <inc/lib.h>

// Desired memory protection for the mapping. Should not conflict with the open
// mode of the file. prot is defined as the bitwise OR of the following:
#define PROT_READ	0x01	// Pages may be written
#define	PROT_WRITE	0x02	// Pages may be read

// Determines whether updates to the mapping can by seen by other mappings of
// the same file and if any changes are carried through to the underlying
// filesystem. Include one of the following in the flags argument:
//#define MAP_PRIVATE	0x00	// Create private COW mapping
//#define MAP_SHARED	0x01	// Updates are visiable and carried through
//
// *** Moved definition to inc/lib.h so fs/serv.c knows what it is.
// *** (Subject to change)
//

// Implementation of mmap(), which maps address space to a memory object.
// Sets up a mapping between a section of a process' virtual address space,
// starting at addr, and some memory object represented by fd with offset off,
// continuing for len bytes. Returns a pointer to the mapped address if
// successful; otherwise, returns a value of MAP_FAILED.
void *
mmap(void *addr, size_t len, int prot, int flags, int fd_num, off_t off)
{
	int retva;

	// off must be a multiple of PGSIZE
	if ((off % PGSIZE) != 0) return (void *)-E_INVAL;

	// Round the address down to the nearest page
	//addr = ROUNDDOWN(addr, PGSIZE);

	// Attempt to find a contiguous region of memeory of size len and
	cprintf("mmap() - find free memory \n");
	retva = sys_page_block_alloc(0, addr, len / PGSIZE, PTE_U|flags);
	if (retva == E_NO_MEM || retva == E_INVAL || retva == E_BAD_ENV) {
		cprintf("mmap() - failure from sys_page_block_alloc: %d \n", retva);
		return (void *)retva;
	}
	cprintf("mmap() - start memory address: %p, UTOP: %p \n", (uint32_t)retva, UTOP); // va>UTOP??

	// TODO: Do necessary set-up for two types of page fault region
	// handlers for MAP_SHARE and MAP_PRIVATE. The handlers have all
	// the logic for sending IPC calls to the filesystem of facilitating
	// copy on write.

	return (void *) 0;
}

int
munmap(void *addr, size_t len)
{
	return 0;
}

// env_set_region_handler(envid_t env, void *func, uint32_t minaddr, uint32_t maxaddr)
