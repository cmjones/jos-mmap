#include <inc/lib.h>

#define debug	0

// Maximum number of mmapped regions. Default is 204, as the required metadata
// fill fit on a single page.
#define MAXMMAP	204

// Struct for storing the metadata about each mmapped region.
struct mmap_metadata {
	int mmmd_fileid;
	uint32_t mmmd_fileoffset;
	uint32_t mmmd_perm;
	uint32_t mmmd_startaddr;
	uint32_t mmmd_endaddr;
};

// Returns the metadata struct for index i.
#define INDEX2MMAP(i)	((struct mmap_metadata*) (MMAPTABLE + (i) * \
			sizeof(struct mmap_metadata)))

// Unmaps pages from the given address range.
static inline void
page_unmap(uint32_t start, uint32_t end)
{
	int i;

	// Loop through the region and unmap each memory page.
	for(i = start; i < end; i += PGSIZE)
		sys_page_unmap(0, (void *)i);

	// Remove any handlers from the unmapped region.
	set_pgfault_region_handler(NULL, (void *) start, (void *) end);
}

// Implementation of mmap(), which maps address space to a memory object.
// Sets up a mapping between a section of a process' virtual address space,
// starting at addr, and some memory object represented by fd with offset off,
// continuing for len bytes. Returns a pointer to the mapped address if
// successful; otherwise, returns a negative pointer.
void *
mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	struct mmap_metadata *mmmd;
	uint32_t retva, i, fileid;
	int r;

	// Sanity check for offset, which must be a multiple of PGSIZE.
	if ((off % PGSIZE) != 0) return (void *)-E_INVAL;

	// Sanity check for prot bit.
	if ((prot & ~PTE_W) != 0) return (void *) -E_INVAL;

	// Round 'len' up to the nearest page size, since mappings are
	// done with page granularity.
	len = ROUNDUP(len, PGSIZE);

	// Get fileid from fd number.
	fileid = fgetid(fd);

	// Attempt to find a contiguous region of memeory of size len.
	retva = sys_page_reserve(0, addr, len/PGSIZE, PTE_RSV);
	if (retva < 0) {
		cprintf("mmap() - failure from sys_page_block_alloc: "
			"%d \n", retva);
		return (void *)retva;
	}
	if (debug)
		cprintf("mmap() - start memory address: %p, UTOP: %p \n",
			(uint32_t)retva, UTOP);

	// Allocates a page to hold mmaped_region structs if one hasn't
	// been allocated yet.
	if((!(uvpd[PDX(MMAPTABLE)]&PTE_P) || !(uvpt[PGNUM(MMAPTABLE)]&PTE_P)) &&
	   (r = sys_page_alloc(0, (void *) MMAPTABLE, PTE_P|PTE_W|PTE_U)) < 0)
		panic("sys_page_alloc: %e", r);

	// Finds the smallest i that isn't already used by a mmap_metadata
	// struct, fills in its values, and returns retva. Returns negative on
	// failure.  Unallocated meta-data slots have mmmd_endaddr = NULL
	for (i = 0; i < MAXMMAP; i++) {
		if((mmmd = INDEX2MMAP(i))->mmmd_endaddr == 0) {
			mmmd->mmmd_fileid = fileid;
			mmmd->mmmd_fileoffset = off;
			// Adds appropriate prot flags, setting PTE_U for all
			// pages, PTE_COW for MAP_PRIVATE pages, and PTE_SHARE
			// for MAP_SHARED pages.
			mmmd->mmmd_perm = prot | PTE_U |
				((flags & MAP_SHARED) ? PTE_SHARE : PTE_COW);
			mmmd->mmmd_startaddr = retva;
			mmmd->mmmd_endaddr = retva+len;
			break;
		}
	}

	// If we didn't find a slot, we've reached the limit on mmap regions
	if (i == MAXMMAP)
		return (void *) -E_NO_MEM;

	if (debug)
		cprintf("mmap() - found slot %d for meta-data\n", i);

	// Install the correct handler for the type of mapping created.
	if ((flags & MAP_SHARED) != 0)
		set_pgfault_region_handler(mmap_shared_handler, (void *) retva,
					   (void * )(retva + len));

	else
		set_pgfault_region_handler(mmap_private_handler, (void *) retva,
					   (void * )(retva + len));

	if (debug)
		cprintf("mmap() - finished, region starts at %08x\n", retva);
	
	return (void *) retva;
}

// Implementation of munmap(), which unmaps any mmapped regions, and sets
// the appropriate handlers. Can unmap and entire mmapped region, partial
// region, or multiple mmapped regions, depending on the len given.
int
munmap(void *addr, size_t len)
{
	struct mmap_metadata *mmmd, *temp;
	int i, j;

	// Ensure addr is page-aligned, and pre-calculate an address range
	uint32_t minaddr = (uint32_t)addr;
	uint32_t maxaddr = (uint32_t)addr + ROUNDUP(len, PGSIZE);
	if (minaddr % PGSIZE != 0) return -E_INVAL;

	// Step through the meta-data array and remove any mmapped regions
	//  that lie within the address range.  Every time a region is
	//  removed, the corresponding virtual addresses must be unmapped.
	//
	// An error may be thrown here if the range lies in the middle of
	//  an mmapped region, and there are MAXMMAP regions already.  There
	//  will not be room for the two mmap regions that will be the result.
	for (i = 0; i < MAXMMAP; i++) {
		// Check if this slot has been allocated
		if ((mmmd = INDEX2MMAP(i))->mmmd_endaddr == 0)
			continue;

		// If the new range is a subset of the old range, we may
		//  have to ensure there is empty space in the array
		if (mmmd->mmmd_startaddr < minaddr && mmmd->mmmd_endaddr >
			maxaddr) {
			// Since the old range will have to be split into
			//  two, find a location to put the second piece.
			for (j = 0; j < MAXMMAP; j++) {
				if ((temp = INDEX2MMAP(j))->mmmd_endaddr == 0) {
					// Split the handler into two pieces,
					// with the old slot containing the
					// bottom half.
					mmmd->mmmd_fileid = mmmd->mmmd_fileid;

			    		// Calculate the file offset of the new
					// mmap region.
					mmmd->mmmd_fileoffset = 
						mmmd->mmmd_fileoffset + 
						maxaddr-mmmd->mmmd_startaddr;

					temp->mmmd_startaddr = maxaddr;
					temp->mmmd_endaddr = mmmd->mmmd_endaddr;
					mmmd->mmmd_endaddr = minaddr;

					// Because no ranges overlap, the new
					// range will not overlap any other mmap
					// regions.  Unmap all of the pages in
					// the address range.
					page_unmap(minaddr, maxaddr);
					goto success;
				}
			}

			// If we make it here, there wasn't room to split the
			// region.
			return -E_NO_MEM;
		}

		// If the new range is a superset of the old range, the old
		// region should be removed.
		if (mmmd->mmmd_startaddr >= minaddr && mmmd->mmmd_endaddr <=
		    maxaddr) {
			page_unmap(mmmd->mmmd_startaddr, mmmd->mmmd_endaddr);
			mmmd->mmmd_endaddr = 0;
		}

		// If the new and old range overlap, adjust the old range.
		if (mmmd->mmmd_startaddr < maxaddr && mmmd->mmmd_endaddr >
		    maxaddr) {
			page_unmap(mmmd->mmmd_startaddr, maxaddr);
			mmmd->mmmd_fileoffset += maxaddr-mmmd->mmmd_startaddr;
			mmmd->mmmd_startaddr = maxaddr;
		} else if (mmmd->mmmd_startaddr < minaddr &&
			   mmmd->mmmd_endaddr > minaddr) {
			page_unmap(minaddr, mmmd->mmmd_endaddr);
			mmmd->mmmd_endaddr = minaddr;
		}
	}

	// Once we've reached here, we're done
	success:
	return 0;
}

// Handler for pages mmapped with the MAP_SHARED flag.
static void
mmap_shared_handler(struct UTrapframe *utf)
{
	struct mmap_metadata *mmmd;
	uint32_t err, i;
	void *addr;

	addr = (void *) utf->utf_fault_va;
	err = utf->utf_err;

	// Iterates through the mmap handlers, setting mmmd to the mmap
	// metdata struct that contains the mapped region.
	for (i = 0; i < MAXMMAP; i++) {
		mmmd = INDEX2MMAP(i);
		if ((uint32_t) addr < mmmd->mmmd_endaddr &&
		    (uint32_t) addr >= mmmd->mmmd_startaddr)
			break;
	}

	// Page aligning addr for filesystem request.
	addr = ROUNDDOWN(addr, PGSIZE);

	// If it is a write fault and the page isn't supposed to be writeable,
	// throw an error.
	if ((err & 2) && !(mmmd->mmmd_perm & PTE_W))
		panic("tried to write in a non-writeable mmapped region.\n");

	// So, it's either a read or write fault with appropriate perms, so
	// make the request from the filesystem and panic if that fails.
	if (request_block(mmmd->mmmd_fileid, mmmd->mmmd_fileoffset, addr,
			  mmmd->mmmd_perm) < 0)
		panic("request block failed in mmap handler.\n");
}

// Handler for pages mmapped with the MAP_PRIVATE flag.
static void
mmap_private_handler(struct UTrapframe *utf)
{
	struct mmap_metadata *mmmd;
	uint32_t err, i;
	void *addr;

	addr = (void *) utf->utf_fault_va;
	err = utf->utf_err;

	cprintf("Starting page fault handler for private mappings, fault "
		"address %p\n", addr);

	// Iterates through the mmap handlers, setting mmmd to the mmap
	// metdata struct that contains the mapped region.
	for (i = 0; i < MAXMMAP; i++) {
		mmmd = INDEX2MMAP(i);
		if ((uint32_t) addr < mmmd->mmmd_endaddr &&
		    (uint32_t) addr >= mmmd->mmmd_startaddr)
			break;
	}

	cprintf("Found metadata at index %d\n", i);

	// Page aligning addr for filesystem request.
	addr = ROUNDDOWN(addr, PGSIZE);

	// Request the file block only if we don't have it yet.
	if ((!(uvpd[PDX(addr)]&PTE_P) || !(uvpt[PGNUM(addr)]&PTE_P)) &&
	   request_block(mmmd->mmmd_fileid, mmmd->mmmd_fileoffset, addr,
		   	 mmmd->mmmd_perm) < 0)
		panic("request block failed in mmap handler.\n");


	// If it is a write fault.
	if (err & 2) {
		// If the mmapped page isn't supposed to be writeable, throw an
		// error.
		if (!(mmmd->mmmd_perm & PTE_W)) {
			panic("tried to write in a non-writeable mmapped "
				"region.\n");
		} else {
			// The fault is writeable and we have write permissions
			// so we allocate a temp page, copy memory, and map the
			// new page to the fault address.
			if (sys_page_alloc(0, PFTEMP, PTE_U|PTE_W) != 0)
				panic("couldn't allocate a new page for "
					"copy-on-write.\n");

			memcpy(PFTEMP, addr, PGSIZE);

			if (sys_page_map(0, PFTEMP, 0, addr, PTE_U|PTE_W) != 0)
				panic("couldn't remap temp page for COW.\n");
		}
	}
}
