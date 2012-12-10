#include <inc/string.h>

#include "fs.h"

#define debug 0

// --------------------------------------------------------------
// Super block
// --------------------------------------------------------------

// Validate the file system super-block.
void
check_super(void)
{
	if (super->s_magic != FS_MAGIC)
		panic("bad file system magic number");

	if (super->s_nblocks > DISKSIZE/BLKSIZE)
		panic("file system is too large");

	cprintf("superblock is good\n");
}

////////////////////////////////////////////////////////////////////
// CHALLENGE: Block bitmap functions, lifted from last year's lab //
////////////////////////////////////////////////////////////////////

// Returns true if the block 'blockno' is free by checking
//  the block bitmap, false otherwise.
bool
block_is_free(uint32_t blockno)
{
	if (super == 0 || blockno >= super->s_nblocks)
		return false;
	if (bitmap[blockno / 32] & (1 << (blockno % 32)))
		return true;
	return false;
}

// Mark a block free in the bitmap
void
free_block(uint32_t blockno)
{
	if (blockno == 0)
		panic("attempt to free block 0");
	bitmap[blockno/32] |= 1<<(blockno%32);
}

// Search the bitmap for a free block and allocate it.  When you
// allocate a block, inmmediately flush the changed bitmap block
// to disk.
//
// Return block number allocated on success,
// -E_NO_DISK if we are out of blocks.
//
// Hint: use free_block as an example for manipulating the bitmap
int
alloc_block(void)
{
	int i, j;
	// Search through the bitmap array for a value that is not
	// 0 (all blocks allocated).  Each value in the array deals
	// with 32 blocks.
	for(i = 0; i < (super->s_nblocks+31)/32; i++) {
		if(bitmap[i] != 0) {
			// The first free bit should be the least
			// significant one bit, loop to find it.
			for(j = 0; j < 32; ((bitmap[i] >> j)&1) == 0);

			// Check if we're out of blocks
			if(i*32+j >= super->s_nblocks) break;

			// Quick way to set the correct bit
			// eg: 0x01010000 & 0x01001111 = 0x01000000
			bitmap[i] &= bitmap[i]-1;

			// Flush the change in the bitmap to disk
			flush_block(&bitmap[i], false);

			// Finally, return the block number that
			// was allocated
			return i*32+j;
		}
	}

	// No free blocks are available
	return -E_NO_DISK;
}


// --------------------------------------------------------------
// File system structures
// --------------------------------------------------------------

// Initialize the file system
void
fs_init(void)
{
	static_assert(sizeof(struct File) == 256);

	// Find a JOS disk.  Use the second IDE disk (number 1) if available.
	if (ide_probe_disk1())
		ide_set_disk(1);
	else
		ide_set_disk(0);

	bc_init();

	// Set "super" to point to the super block.
	super = diskaddr(1);
	check_super();
}

// Find the disk block number slot for the 'filebno'th block in file 'f'.
// Set '*ppdiskbno' to point to that slot.
// The slot will be one of the f->f_direct[] entries,
// or an entry in the indirect block.
// When 'alloc' is set, this function will allocate an indirect block
// if necessary.
//
// Returns:
//	0 on success (but note that *ppdiskbno might equal 0).
//	-E_NOT_FOUND if the function needed to allocate an indirect block, but
//		alloc was 0.
//	-E_NO_DISK if there's no space on the disk for an indirect block.
//	-E_INVAL if filebno is out of range (it's >= NDIRECT + NINDIRECT).
//
// Analogy: This is like pgdir_walk for files.
// Hint: Don't forget to clear any block you allocate.
static int
file_block_walk(struct File *f, uint32_t filebno, uint32_t **ppdiskbno, bool alloc)
{
	int r;
	uint32_t *ptr;
	char *blk;

	if (filebno < NDIRECT)
		ptr = &f->f_direct[filebno];
	else if (filebno < NDIRECT + NINDIRECT) {
		if (f->f_indirect == 0) {
			// CHALLENGE:
			// If we're not allocating, return failure
			if(!alloc) return -E_NOT_FOUND;

			// Attempt to allocate an indirect block
			if((r = alloc_block()) < 0) return r;

			// Zero out the newly allocated block and map
			// it to the file.
			memset(diskaddr(r), 0, BLKSIZE);
			f->f_indirect = r;
		}
		ptr = (uint32_t*)diskaddr(f->f_indirect) + filebno - NDIRECT;
	} else
		return -E_INVAL;

	*ppdiskbno = ptr;
	return 0;
}

// Set *blk to the address in memory where the filebno'th
// block of file 'f' would be mapped.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_NO_DISK if a block needed to be allocated but the disk is full.
//	-E_INVAL if filebno is out of range.
//
int
file_get_block(struct File *f, uint32_t filebno, char **blk)
{
	int r;
	uint32_t *ptr;

	if ((r = file_block_walk(f, filebno, &ptr, 1)) < 0)
		return r;
	if (*ptr == 0) {
		if (debug)
			cprintf("A new block is being allocated for file %8s, file block number %d\n", f->f_name, filebno);
		// CHALLENGE: allocate new block for the file at this
		//  location.
		if((r = alloc_block()) < 0) return -E_NO_DISK;
		*ptr = r;
	}
	*blk = diskaddr(*ptr);
	if (debug)
		cprintf("Found block %d for file %8s at 0x%x (disk block %d)\n", filebno, f->f_name, *blk, *ptr);
	return 0;
}

// Try to find a file named "name" in dir.  If so, set *file to it.
//
// Returns 0 and sets *file on success, < 0 on error.  Errors are:
//	-E_NOT_FOUND if the file is not found
static int
dir_lookup(struct File *dir, const char *name, struct File **file)
{
	int r;
	uint32_t i, j, nblock;
	char *blk;
	struct File *f;

	// Search dir for name.
	// We maintain the invariant that the size of a directory-file
	// is always a multiple of the file system's block size.
	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (strcmp(f[j].f_name, name) == 0) {
				*file = &f[j];
				return 0;
			}
	}
	return -E_NOT_FOUND;
}

// Set *file to point at a free File structure in dir.  The caller is
// responsible for filling in the File fields.
//
// Lifted from last year's lab
static int
dir_alloc_file(struct File *dir, struct File **file)
{
	int r;
	uint32_t nblock, i, j;
	char *blk;
	struct File *f;

	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++) {
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File*) blk;
		for (j = 0; j < BLKFILES; j++)
			if (f[j].f_name[0] == '\0') {
				*file = &f[j];
				return 0;
			}
	}
	dir->f_size += BLKSIZE;
	if ((r = file_get_block(dir, i, &blk)) < 0)
		return r;
	f = (struct File*) blk;
	*file = &f[0];
	return 0;
}

// Skip over slashes.
static const char*
skip_slash(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

// Evaluate a path name, starting at the root.
// On success, set *pf to the file we found
// and set *pdir to the directory the file is in.
// If we cannot find the file but find the directory
// it should be in, set *pdir and copy the final path
// element into lastelem.
static int
walk_path(const char *path, struct File **pdir, struct File **pf, char *lastelem)
{
	const char *p;
	char name[MAXNAMELEN];
	struct File *dir, *f;
	int r;

	// if (*path != '/')
	//	return -E_BAD_PATH;
	path = skip_slash(path);
	f = &super->s_root;
	dir = 0;
	name[0] = 0;

	if (pdir)
		*pdir = 0;
	*pf = 0;
	while (*path != '\0') {
		dir = f;
		p = path;
		while (*path != '/' && *path != '\0')
			path++;
		if (path - p >= MAXNAMELEN)
			return -E_BAD_PATH;
		memmove(name, p, path - p);
		name[path - p] = '\0';
		path = skip_slash(path);

		if (dir->f_type != FTYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(dir, name, &f)) < 0) {
			if (r == -E_NOT_FOUND && *path == '\0') {
				if (pdir)
					*pdir = dir;
				if (lastelem)
					strcpy(lastelem, name);
				*pf = 0;
			}
			return r;
		}
	}

	if (pdir)
		*pdir = dir;
	*pf = f;
	return 0;
}

// --------------------------------------------------------------
// File operations
// --------------------------------------------------------------

// CHALLENGE: Create "path".  On success set *pf to point at the file
// and return 0.  On error return < 0;
//
// Lifted from last year's lab.
int
file_create(const char *path, struct File **pf)
{
	char name[MAXNAMELEN];
	int r;
	struct File *dir, *f;

	if ((r = walk_path(path, &dir, &f, name)) == 0)
		return -E_FILE_EXISTS;
	if (r != -E_NOT_FOUND || dir == 0)
		return r;
	if ((r = dir_alloc_file(dir, &f)) < 0)
		return r;
	strcpy(f->f_name, name);
	*pf = f;
	file_flush(dir, 0, 0, false);
	return 0;
}

// Open "path".  On success set *pf to point at the file and return 0.
// On error return < 0.
int
file_open(const char *path, struct File **pf)
{
	return walk_path(path, 0, pf, 0);
}

// Read count bytes from f into buf, starting from seek position
// offset.  This meant to mimic the standard pread function.
// Returns the number of bytes read, < 0 on error.
ssize_t
file_read(struct File *f, void *buf, size_t count, off_t offset)
{
	int r, bn;
	off_t pos;
	char *blk;

	if (offset >= f->f_size)
		return 0;

	count = MIN(count, f->f_size - offset);

	for (pos = offset; pos < offset + count; ) {
		if ((r = file_get_block(f, pos / BLKSIZE, &blk)) < 0)
			return r;
		bn = MIN(BLKSIZE - pos % BLKSIZE, offset + count - pos);
		memmove(buf, blk + pos % BLKSIZE, bn);
		pos += bn;
		buf += bn;
	}

	return count;
}

// CHALLENGE: function descriptions from last year's lab, tried
//  not to look at the implementations while coding.
//
// Write count bytes from buf into f, starting at seek position
// offset.  This is meant to mimic the standard pwrite function.
// Extends the file if necessary.
// Returns the number of bytes written, < 0 on error.
int
file_write(struct File *f, const void *buf, size_t count, off_t offset)
{
	int r, bn;
	off_t pos;
	char *blk;

	// If the last write location is greater than the file size,
	//  attempt to increase the file size.
	if(offset+count >= f->f_size && (r = file_set_size(f, offset+count)) != 0)
		return r;

	// Begin copying bytes from the buffer to the file
	for(pos = offset; pos < offset+count; ) {
		// Grab the block containing pos
		if ((r = file_get_block(f, offset/BLKSIZE, &blk)) < 0)
			return r;

		// Calculate how many bytes can be written
		bn = MIN(BLKSIZE - pos % BLKSIZE, offset + count - pos);

		// Write the bytes to the block
		memmove(blk + pos % BLKSIZE, buf, bn);
		pos += bn;
		buf += bn;
	}

	// By now we should have written 'count' bytes
	return count;
}

// Remove a block from file f.  If it's not there, just silently succeed.
// Returns 0 on success, < 0 on error
static int
file_free_block(struct File *f, uint32_t filebno)
{
	int r;
	uint32_t *blkno;

	// Attempt to grab the correct block
	if((r = file_block_walk(f, filebno, &blkno, false)) != 0)
		return r;

	// Free the block by zeroing the file's block slot
	//  and marking that disk block as free
	if(*blkno != 0) {
		 free_block(*blkno);
		*blkno = 0;
	}

	// Return success
	return 0;
}

// Remove any blocks currently used by file 'f',
// but not necessary for a file of size 'newsize'.
// For both the old and new sizes, figure out the number of blocks required,
// and then clear the blocks from new_nblocks to old_nblocks.
// If the new_nblocks is no more than NDIRECT, and the indirect block has
// been allocated (f->f_indirect != 0), then free the indirect block too.
// (Remember to clear the f->f_indirect pointer so you'll know
// whether it's valid!)
// Do not change f->f_size.
static void
file_truncate_blocks(struct File *f, off_t newsize)
{
	int newblks, oldblks;
	uint32_t i;

	// Calculate blocks needed for the old and new size
	newblks = ROUNDUP(newsize, BLKSIZE)/BLKSIZE;
	oldblks = ROUNDUP(f->f_size, BLKSIZE)/BLKSIZE;

	// Free the newly unused blocks
	for(i = newblks; i < oldblks; i++)
		file_free_block(f, i);

	// If newblks is less than NDIRECT and the indirect
	//  block is allocated, free it as well
	if(newblks <= NDIRECT && f->f_indirect != 0) {
		free_block(f->f_indirect);
		f->f_indirect = 0;
	}
}

// Set the size of file f, truncating or extending as necessary
int
file_set_size(struct File *f, off_t newsize)
{
	if(f->f_size > newsize)
		file_truncate_blocks(f, newsize);
	f->f_size = newsize;
	flush_block(f, false);
	return 0;
}

// Flush the contents and metadata of file f out to disk.
// Loop over all the blocks in range, or over the whole file
// if length is 0.
// Translate the file block number into a disk block number
// and then check whether that disk block is dirty. If so,
// write it out.
// If 'force' is true, skip checking the dirty bit
void
file_flush(struct File *f, size_t length, off_t offset, bool force)
{
	int r, i, min, max;
	uint32_t *blk;

	// Flush the file meta-data block and the indirect block
	flush_block(f, false);
	if(f->f_indirect != 0)
		flush_block(diskaddr(f->f_indirect), force);

	// Figure out the number of blocks in the file,
	//  then loop through them.
	if(length <= 0) {
		min = offset/BLKSIZE;
		max = ROUNDUP(offset+length, BLKSIZE)/BLKSIZE;
	} else {
		min = 0;
		max = ROUNDUP(f->f_size, BLKSIZE)/BLKSIZE;
	}
	for(i = min; i < max; i++) {
		// Calculate the address of the file block,
		//  then flush that address.
		if ((r = file_block_walk(f, i, &blk, 0)) < 0 ||
		    *blk == 0)
			continue;
		flush_block(diskaddr(*blk), force);
	}
}

// Remove a file by truncating it and then zeroing the name
int
file_remove(const char *path)
{
	int r;
	struct File *f;

	if ((r = walk_path(path, 0, &f, 0)) < 0)
		return r;

	file_truncate_blocks(f, 0);
	f->f_name[0] = '\0';
	f->f_size = 0;
	flush_block(f, false);

	return 0;
}

// Sync the entire file system.  A big hammer.
void
fs_sync(void)
{
	int i;
	for (i = 1; i < super->s_nblocks; i++)
		flush_block(diskaddr(i), false);
}
