
#include "fs.h"

// Return the virtual address of this disk block.
void*
diskaddr(uint32_t blockno)
{
	if (blockno == 0 || (super && blockno >= super->s_nblocks))
		panic("bad block number %08x in diskaddr", blockno);
	return (char*) (DISKMAP + blockno * BLKSIZE);
}

// Is this virtual address mapped?
bool
va_is_mapped(void *va)
{
	return (uvpd[PDX(va)] & PTE_P) && (uvpt[PGNUM(va)] & PTE_P);
}

// Is this virtual address dirty?
bool
va_is_dirty(void *va)
{
	return (uvpt[PGNUM(va)] & PTE_D) != 0;
}

// CHALLENGE:
// Flush the block containing the given address to disk if necessary.
// Based off of last year's solution which used va_is_mapped and
// va_is_dirty.  If the block isn't mapped or isn't dirty, do nothing.
//
// If 'force' is true, don't check the dirty bit.
void
flush_block(void *addr, bool force)
{
	// Sanity-check the address
	if((int)addr < DISKMAP || (int)addr >= DISKMAP+DISKSIZE)
		panic("flush_block called on bad address 0x%08x", addr);

	// Round the address to the nearest block size
	addr = ROUNDDOWN(addr, BLKSIZE);

	// If the block at address addr exists and is dirty, flush
	// it to the disk.
	if(va_is_mapped(addr) && (force || va_is_dirty(addr))) {
		// Write the block to disk, converting addr to
		//  a block number.
		ide_write(((uint32_t)addr-DISKMAP)/BLKSECTS, addr, BLKSECTS);

		// Now we need to reset the PTE_D bit at addr
		if(sys_page_map(0, addr, 0, addr, uvpt[PGNUM(addr)]&PTE_SYSCALL) != 0)
			panic("Failed to reset dirty bit on block at address %x", addr);
	}
}

// Challenge:
//  Reads the block containing addr into memory from disk, replacing any
//  previous contents of that block in the buffer cache.
void
read_block(void *addr)
{
	uint32_t blockno = ((uint32_t)addr-DISKMAP)/BLKSIZE;

	// Sanity check the block number.
	if (super && blockno >= super->s_nblocks)
		panic("reading non-existent block %08x\n", blockno);

	// Allocate a page in the disk map region, read the contents
	// of the block from the disk into that page.
	// Hint: first round addr to page boundary.
	addr = ROUNDDOWN(addr, PGSIZE);

	// Allocate a new page for the address
	if(sys_page_alloc(0, addr, PTE_U|PTE_W) != 0)
		panic("couldn't allocate a new page for file system");

	// Call ide_read, which takes a sector number and a number of
	//  sectors.  We want to read a full page, or BLKSCTS sectors,
	//  starting at the first sector of the target block.
	if(ide_read(blockno*BLKSECTS, addr, BLKSECTS) != 0)
		panic("error reading block %d in FS", blockno);
}

// Fault any disk block that is read in to memory by
// loading it from disk.
static void
bc_pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;

	// Check that the fault was within the block cache region
	if (addr < (void*)DISKMAP || addr >= (void*)(DISKMAP + DISKSIZE))
		panic("page fault in FS: eip %08x, va %08x, err %04x",
		      utf->utf_eip, addr, utf->utf_err);

	// Read the missing block into memory
	read_block(addr);
}


void
bc_init(void)
{
	struct Super super;
	set_pgfault_handler(bc_pgfault);

	// cache the super block by reading it once
	memmove(&super, diskaddr(1), sizeof super);
}

