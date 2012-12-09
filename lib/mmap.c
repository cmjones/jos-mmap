#include <inc/lib.h>

#define debug	0

// Maximum number of mmapped regions. Default is 256, as the required metadata
// fill fit on a single page.
#define MAXMMAP	256

// Struct for storing the metadata about each mmapped region.
struct mmap_metadata {
	int mmmd_fileid;
	uint32_t mmmd_fileoffset;
	uint32_t mmmd_startaddr;
	uint32_t mmmd_endaddr;
};

// Returns the metadata struct for index i.
#define INDEX2MMAP(i)	((struct mmap_metadata*) (MMAPTABLE + (i)*0x1000))


// Unmaps pages from the given range
static inline void
page_unmap(uint32_t start, uint32_t end)
{
	int i;
	for(i = start; i < end; i += PGSIZE)
		sys_page_unmap(0, (void *)i);
}

// Implementation of mmap(), which maps address space to a memory object.
// Sets up a mapping between a section of a process' virtual address space,
// starting at addr, and some memory object represented by fd with offset off,
// continuing for len bytes. Returns a pointer to the mapped address if
// successful; otherwise, returns a negative pointer.
void *
mmap(void *addr, size_t len, int prot, int flags, int fd_num, off_t off)
{
	struct mmap_metadata *mmmd;
	uint32_t retva, i;
	int r;

	// Sanity check for offset, which must be a multiple of PGSIZE.
	if ((off % PGSIZE) != 0) return (void *)-E_INVAL;

	// Sanity check for prot bit.
	if ((prot & ~PTE_W) != 0) return (void *) -E_INVAL;

	// Round 'len' up to the nearest page size, since mappings are
	//  done with page granularity
	len = ROUNDUP(len, PGSIZE);

	// Attempt to find a contiguous region of memeory of size len
	cprintf("mmap() - find free memory \n");
	retva = sys_page_block_alloc(0, addr, len, PTE_U|prot);
	if (retva < 0) {
		cprintf("mmap() - failure from sys_page_block_alloc: %d \n", retva);
		return (void *)retva;
	}
	cprintf("mmap() - start memory address: %p, UTOP: %p \n", (uint32_t)retva, UTOP); // va>UTOP??

	// Allocates a page to hold mmaped_region structs if one hasn't
	//  been allocated yet.
	if((!(uvpd[PDX(MMAPTABLE)]&PTE_P) || !(uvpt[PGNUM(MMAPTABLE)]&PTE_P)) &&
	   (r = sys_page_alloc(0, (void *) MMAPTABLE, PTE_P|PTE_W|PTE_U)) < 0)
		panic("sys_page_alloc: %e", r);

	// Finds the smallest i that isn't already used by a mmap_metadata
	// struct, fills in its values, and returns retva. Returns negative on
	// failure.  Unallocated meta-data slots have mmmd_endaddr = NULL
	for (i = 0; i < MAXMMAP; i++) {
		if((mmmd = INDEX2MMAP(i))->mmmd_endaddr == 0) {
			mmmd->mmmd_fileid = fd_num;
			mmmd->mmmd_fileoffset = off;
			mmmd->mmmd_startaddr = retva;
			mmmd->mmmd_endaddr = retva+len;
		}
	}

	// If we didn't find a slot, we've reached the limit on mmap regions
	if(i == MAXMMAP)
		return (void *) -E_NO_MEM;

	// Install the correct handler for the type of mapping created
	if ((flags & MAP_SHARED) != 0)
		sys_env_set_region_handler(0, mmap_shared_handler, (void *) retva,
					   (void *)(retva + len));
	else
		sys_env_set_region_handler(0, mmap_private_handler, (void *) retva,
					   (void *)(retva + len));

	return (void *) retva;
}

int
munmap(void *addr, size_t len)
{
	struct mmap_metadata *mmmd, *temp;
	int i, j;

	// Ensure addr is page-aligned, and pre-calculate an address range
	uint32_t minaddr = (uint32_t)addr;
	uint32_t maxaddr = (uint32_t)addr + ROUNDUP(len, PGSIZE);
	if(minaddr%PGSIZE != 0) return -E_INVAL;

	// Step through the meta-data array and remove any mmapped regions
	//  that lie within the address range.  Every time a region is
	//  removed, the corresponding virtual addresses must be unmapped.
	//
	// An error may be thrown here if the range lies in the middle of
	//  an mmapped region, and there are MAXMMAP regions already.  There
	//  will not be room for the two mmap regions that will be the result.
	for(i = 0; i < MAXMMAP; i++) {
		// Check if this slot has been allocated
		if((mmmd = INDEX2MMAP(i))->mmmd_endaddr == 0)
			continue;

		// If the new range is a subset of the old range, we may
		//  have to ensure there is empty space in the array
		if(mmmd->mmmd_startaddr < minaddr && mmmd->mmmd_endaddr > maxaddr) {
			// Since the old range will have to be split into
			//  two, find a location to put the second piece.
			for(j = 0; j < MAXMMAP; j++) {
				if((temp = INDEX2MMAP(j))->mmmd_endaddr == 0) {
					// Split the handler into two pieces, with the old slot
					//  contianing the bottom half.
					mmmd->mmmd_fileid = mmmd->mmmd_fileid;

			    		// Calculate the file offset of the new mmap region
					mmmd->mmmd_fileoffset = mmmd->mmmd_fileoffset+maxaddr-mmmd->mmmd_startaddr;

					temp->mmmd_startaddr = maxaddr;
					temp->mmmd_endaddr = mmmd->mmmd_endaddr;
					mmmd->mmmd_endaddr = minaddr;

					// Because no ranges overlap, the new range will
					//  not overlap any other mmap regions.  Unmap all
					//  of the pages in the address range
					page_unmap(minaddr, maxaddr);
					goto success;
				}
			}

			// If we make it here, there wasn't room to split the region
			return -E_NO_MEM;
		}

		// If the new range is a superset of the old range, the old
		//  region should be removed.
		if(mmmd->mmmd_startaddr >= minaddr && mmmd->mmmd_endaddr <= maxaddr) {
			page_unmap(mmmd->mmmd_startaddr, mmmd->mmmd_endaddr);
			mmmd->mmmd_endaddr = 0;
		}

		// If the new and old range overlap, adjust the old range.
		if(mmmd->mmmd_startaddr < maxaddr && mmmd->mmmd_endaddr > maxaddr) {
			page_unmap(mmmd->mmmd_startaddr, maxaddr);
			mmmd->mmmd_fileoffset += maxaddr-mmmd->mmmd_startaddr;
			mmmd->mmmd_startaddr = maxaddr;
		} else if(mmmd->mmmd_startaddr < minaddr && mmmd->mmmd_endaddr > minaddr) {
			page_unmap(minaddr, mmmd->mmmd_endaddr);
			mmmd->mmmd_endaddr = minaddr;
		}
	}

	// Once we've reached here, we're done
	success:
	return 0;
}

// Handler for pages mmapped with the MAP_SHARED flag
static void
mmap_shared_handler(struct UTrapframe *utf)
{
	//union Fsipc fsipcbuf;

	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;

	//fsibcbuf.breq.req_fileid = 0x0;
	//fsibcbuf.breq.req_offset = 0x1;
	//fsibcbuf.breq.req_perm = 0x1;
	
	// TODO: Look up mmap region meta data using va
	// 	 Not sure the best way to do this, maybe redo the struct?
	
	// TODO: Determine fault type r/w and set appropriate perms.
	//	 If 0, then 0. If PTE_W, then PTE_W

	// TODO: Make the appropriate IPC call to the FS.
	 
}

// Handler for pages mmapped with the MAP_PRIVATE flag
static void
mmap_private_handler(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;

	// TODO: Look up mmap region meta data using va

	// TODO: Determine fault type r/w and set appropriate perms.
	//	 If 0, then 0. If PTE_W, then copy perms and PTE_W

	// TODO: Make the appropriate IPC call to the FS.
}
