/*
 * File system server main loop -
 * serves IPC requests from other environments.
 */

#include <inc/x86.h>
#include <inc/string.h>

#include "fs.h"


#define debug 0

// The file system server maintains three structures
// for each open file.
//
// 1. The on-disk 'struct File' is mapped into the part of memory
//    that maps the disk.  This memory is kept private to the file
//    server.
// 2. Each open file has a 'struct Fd' as well, which sort of
//    corresponds to a Unix file descriptor.  This 'struct Fd' is kept
//    on *its own page* in memory, and it is shared with any
//    environments that have the file open.
// 3. 'struct OpenFile' links these other two structures, and is kept
//    private to the file server.  The server maintains an array of
//    all open files, indexed by "file ID".  (There can be at most
//    MAXOPEN files open concurrently.)  The client uses file IDs to
//    communicate with the server.  File IDs are a lot like
//    environment IDs in the kernel.  Use openfile_lookup to translate
//    file IDs to struct OpenFile.

struct OpenFile {
	uint32_t o_fileid;	// file id
	struct File *o_file;	// mapped descriptor for open file
	int o_mode;		// open mode
	struct Fd *o_fd;	// Fd page
};

// Max number of open files in the file system at once
#define MAXOPEN		1024
#define FILEVA		0xD0000000

// initialize to force into data section
struct OpenFile opentab[MAXOPEN] = {
	{ 0, 0, 1, 0 }
};

// Virtual address at which to receive page mappings containing client requests.
union Fsipc *fsreq = (union Fsipc *)0x0ffff000;

void
serve_init(void)
{
	int i;
	uintptr_t va = FILEVA;
	for (i = 0; i < MAXOPEN; i++) {
		opentab[i].o_fileid = i;
		opentab[i].o_fd = (struct Fd*) va;
		va += PGSIZE;
	}
}

// Allocate an open file.
int
openfile_alloc(struct OpenFile **o)
{
	int i, r;

	// Find an available open-file table entry
	for (i = 0; i < MAXOPEN; i++) {
		switch (pageref(opentab[i].o_fd)) {
		case 0:
			if ((r = sys_page_alloc(0, opentab[i].o_fd, PTE_P|PTE_U|PTE_W)) < 0)
				return r;
			/* fall through */
		case 1:
			opentab[i].o_fileid += MAXOPEN;
			*o = &opentab[i];
			memset(opentab[i].o_fd, 0, PGSIZE);
			return (*o)->o_fileid;
		}
	}
	return -E_MAX_OPEN;
}

// Look up an open file for envid.
int
openfile_lookup(envid_t envid, uint32_t fileid, struct OpenFile **po)
{
	struct OpenFile *o;

	o = &opentab[fileid % MAXOPEN];
	if (pageref(o->o_fd) == 1 || o->o_fileid != fileid)
		return -E_INVAL;
	*po = o;
	return 0;
}

// Open req->req_path in mode req->req_omode, storing the Fd page and
// permissions to return to the calling environment in *pg_store and
// *perm_store respectively.
int
serve_open(envid_t envid, struct Fsreq_open *req,
	   void **pg_store, int *perm_store)
{
	char path[MAXPATHLEN];
	struct File *f;
	int fileid;
	int r;
	struct OpenFile *o;

	if (debug)
		cprintf("serve_open %08x %s 0x%x\n", envid, req->req_path, req->req_omode);

	// Copy in the path, making sure it's null-terminated
	memmove(path, req->req_path, MAXPATHLEN);
	path[MAXPATHLEN-1] = 0;

	// Find an open file ID
	if ((r = openfile_alloc(&o)) < 0) {
		if (debug)
			cprintf("openfile_alloc failed: %e", r);
		return r;
	}
	fileid = r;

	if ((req->req_omode&O_MKDIR) != 0) {
		if (debug)
			cprintf("file_open omode O_MKDIR unsupported");
		return -E_INVAL;
	}

	// Attempt to open the file.  If opening fails because the file
	// wasn't found, and the mode is O_CREAT, try to create the file.
	// If that fails as well, the open fails.
	if ((r = file_open(path, &f)) < 0 &&
	    !(r == -E_NOT_FOUND && (req->req_omode&O_CREAT) != 0 && file_create(path, &f))) {
		if (debug)
			cprintf("file_open failed: %e", r);
		return r;
	}

	// If the file mode is O_EXCL and not O_CREAT and we've gotten here,
	// the file was found, so an error should be thrown.
	if((req->req_omode&O_EXCL) != 0 && (req->req_omode&O_CREAT) == 0) {
		if (debug)
			cprintf("file_open failed because file already exists");
		return -E_FILE_EXISTS;
	}

	// If the file mode is O_TRUNC, truncate the file to 0 here
	if((req->req_omode&O_TRUNC) != 0 && (r = file_set_size(f, 0)) < 0) {
		if (debug)
			cprintf("file_open failed: %e", r);
		return r;
	}

	// Save the file pointer
	o->o_file = f;

	// Fill out the Fd structure
	o->o_fd->fd_file.id = o->o_fileid;
	o->o_fd->fd_omode = req->req_omode & O_ACCMODE;
	o->o_fd->fd_dev_id = devfile.dev_id;
	o->o_mode = req->req_omode;

	if (debug)
		cprintf("sending success, page %08x\n", (uintptr_t) o->o_fd);

	// Share the FD page with the caller by setting *pg_store,
	// store its permission in *perm_store
	*pg_store = o->o_fd;
	*perm_store = PTE_P|PTE_U|PTE_W|PTE_SHARE;

	return 0;
}

// For the file req->req_fileid, find the block of memory that
//  holds req->req_offset and stores the address and permissions
//  in *pg_store and *perm_store.
int
serve_mmap(envid_t envid, struct Fsreq_mmap *req,
	   void **pg_store, int *perm_store)
{
	int r;
	struct OpenFile *o;

	if(debug)
		cprintf("serve_mmap %08x %08x %08x %08x\n", envid, req->req_fileid, req->req_flags, req->req_offset);

	// Find the relevant open file to map
	if ((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;

	// If the file is opened in a read-only mode, then the
	//  mmapped block must be opened with MAP_PRIVATE, since
	//  no changes would be written to disk.  Otherwise,
	//  files must be opened with write permissions to be able
	//  to be mmapped.
	//
	// All mmapped files must have read access
	if((o->o_mode&O_ACCMODE) == O_WRONLY ||
	   ((o->o_mode&O_ACCMODE) == O_RDONLY && (req->req_flags&MAP_PRIVATE) == 0))
		return -E_MODE_ERR;

	// Ensure that req->offset is contained within the file, and
	//  grab the page that contains it.
	r = -E_INVAL;
	if(req->req_offset >= 0 ||
	   (r = file_get_block(o->o_file, req->req_offset/BLKSIZE, (char **)pg_store)) != 0)
		return r;

	// Mapped pages will always have read access.  If the requested
	//  mode is MAP_PRIVATE, then permissions for both the new page
	//  and the page in our memory should be PTE_COW
	*perm_store = PTE_P|PTE_U;
	if((req->req_flags&MAP_PRIVATE) == 0) {
		*perm_store |= PTE_W;
	} else {
		*perm_store |= PTE_COW;

		// Store our own mapping as PTE_COW
		// TODO: make sure a page handler is registered for this
		//  environment/this address.
		if(sys_page_map(0, *pg_store, 0, *pg_store, *perm_store) != 0)
			panic("file system unable to map own page as copy-on-write");
	}

	// All set, page should be mapped appropriately
	return 0;
}

// Set the size of req->req_fileid to req->req_size bytes,
//  truncating or extending the file as necessary
int
serve_set_size(envid_t envid, union Fsipc *ipc)
{
	int r;
	struct OpenFile *o;
	struct Fsreq_set_size *req;

	req = &ipc->set_size;

	if(debug)
		cprintf("serve_set_size %08x %08x %08x\n", envid, req->req_fileid, req->req_size);

	// Every file system IPC call has the same general structure.
	// Here's how it goes.

	// First, use openfile_lookup to find the relevant open file.
	// On failure, return the error code to the client with ipc_send.
	if ((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;

	// Second, call the relevant file system function (from fs/fs.c).
	// On failure, return the error code to the client.
	return file_set_size(o->o_file, req->req_size);
}


// Read at most ipc->read.req_n bytes from the current seek position
// in ipc->read.req_fileid.  Return the bytes read from the file to
// the caller in ipc->readRet, then update the seek position.  Returns
// the number of bytes successfully read, or < 0 on error.
int
serve_read(envid_t envid, union Fsipc *ipc)
{
	struct Fsreq_read *req = &ipc->read;
	struct Fsret_read *ret = &ipc->readRet;

	if (debug)
		cprintf("serve_read %08x %08x %08x\n", envid, req->req_fileid, req->req_n);

	// Look up the file id, read the bytes into 'ret', and update
	// the seek position.  Be careful if req->req_n > PGSIZE
	// (remember that read is always allowed to return fewer bytes
	// than requested).  Also, be careful because ipc is a union,
	// so filling in ret will overwrite req.
	//
	struct OpenFile *o;
	int r;

	if ((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;

	// Check for read access to the file
	if((o->o_mode&O_ACCMODE) != O_RDONLY && (o->o_mode&O_ACCMODE) != O_RDWR)
		return -E_MODE_ERR;

	if ((r = file_read(o->o_file, ret->ret_buf,
			   MIN(req->req_n, sizeof ret->ret_buf),
			   o->o_fd->fd_offset)) < 0)
		return r;

	o->o_fd->fd_offset += r;
	return r;
}

// Write req->req_n bytes from req->req_buf to req_fileid, starting at
// the current seek position, and update the seek position
// accordingly.  Extend the file if necessary.  Returns the number of
// bytes written, or < 0 on error.
int
serve_write(envid_t envid, union Fsipc *ipc)
{
	int r;
	struct OpenFile *o;
	struct Fsreq_write *req;

	req = &ipc->write;

	if(debug)
		cprintf("serve_set_size %08x %08x %08x\n", envid, req->req_fileid, req->req_n);

	// Open the file
	if ((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;

	// Check for write access to the file
	if((o->o_mode&O_ACCMODE) != O_WRONLY && (o->o_mode&O_ACCMODE) != O_RDWR)
		return -E_MODE_ERR;

	// Now try to write req->req_n bytes, up to the size of the buffer.
	//  Store how many bytes were written, and start writing from
	//  the offset.
	if((r = file_write(o->o_file, req->req_buf,
			   MIN(req->req_n, sizeof req->req_buf),
			   o->o_fd->fd_offset)) < 0)
		return r;

	// Increment the file offset and return the number of bytes written.
	o->o_fd->fd_offset += r;
	return r;
}

// Stat ipc->stat.req_fileid.  Return the file's struct Stat to the
// caller in ipc->statRet.
int
serve_stat(envid_t envid, union Fsipc *ipc)
{
	struct Fsreq_stat *req = &ipc->stat;
	struct Fsret_stat *ret = &ipc->statRet;
	struct OpenFile *o;
	int r;

	if (debug)
		cprintf("serve_stat %08x %08x\n", envid, req->req_fileid);

	if ((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;

	strcpy(ret->ret_name, o->o_file->f_name);
	ret->ret_size = o->o_file->f_size;
	ret->ret_isdir = (o->o_file->f_type == FTYPE_DIR);
	return 0;
}


// Flush all data and metadata of req->req_fileid to disk.
int
serve_flush(envid_t envid, union Fsipc *ipc)
{
	int r;
	struct OpenFile *o;
	struct Fsreq_flush *req;

	req = &ipc->flush;

	if(debug)
		cprintf("serve_flush %08x %08x\n", envid, req->req_fileid);

	if((r = openfile_lookup(envid, req->req_fileid, &o)) < 0)
		return r;
	file_flush(o->o_file);
	return 0;
}

// Remove the file req->req_path.
int
serve_remove(envid_t envid, union Fsipc *ipc)
{
	struct Fsreq_remove *req;
	char path[MAXPATHLEN];
	int r;

	req = &ipc->remove;

	if (debug)
		cprintf("serve_remove %08x %s\n", envid, req->req_path);

	// Delete the named file.
	// Note: This request doesn't refer to an open file.

	// Copy in the path, making sure it's null-terminated
	memmove(path, req->req_path, MAXPATHLEN);
	path[MAXPATHLEN-1] = 0;

	// Delete the specified file
	return file_remove(path);
}

// Sync the file system.
int
serve_sync(envid_t envid, union Fsipc *req)
{
	fs_sync();
	return 0;
}

typedef int (*fshandler)(envid_t envid, union Fsipc *req);

fshandler handlers[] = {
	// Open and mmap are handled specially because
	// they pass pages
	/* [FSREQ_OPEN] =	(fshandler)serve_open, */
	/* [FSREQ_MMAP] =	(fshandler)serve_mmap, */
	[FSREQ_READ] =		serve_read,
	[FSREQ_WRITE] =		serve_write,
	[FSREQ_STAT] =		serve_stat,
	[FSREQ_FLUSH] =		(fshandler)serve_flush,
	[FSREQ_REMOVE] =	serve_remove,
	[FSREQ_SYNC] =		serve_sync,
	[FSREQ_SET_SIZE] =	serve_set_size,
};
#define NHANDLERS (sizeof(handlers)/sizeof(handlers[0]))

void
serve(void)
{
	uint32_t req, whom;
	int perm, r;
	void *pg;

	while (1) {
		perm = 0;
		req = ipc_recv((int32_t *) &whom, fsreq, &perm);
		if (debug)
			cprintf("fs req %d from %08x [page %08x: %s]\n",
				req, whom, uvpt[PGNUM(fsreq)], fsreq);

		// All requests must contain an argument page
		if (!(perm & PTE_P)) {
			cprintf("Invalid request from %08x: no argument page\n",
				whom);
			continue; // just leave it hanging...
		}

		pg = NULL;
		if (req == FSREQ_OPEN) {
			r = serve_open(whom, (struct Fsreq_open*)fsreq, &pg, &perm);
		} else if (req == FSREQ_MMAP) {
			r = serve_mmap(whom, (struct Fsreq_mmap*)fsreq, &pg, &perm);
		} else if (req < NHANDLERS && handlers[req]) {
			r = handlers[req](whom, fsreq);
		} else {
			cprintf("Invalid request code %d from %08x\n", req, whom);
			r = -E_INVAL;
		}
		ipc_send(whom, r, pg, perm);
		sys_page_unmap(0, fsreq);
	}
}

void
umain(int argc, char **argv)
{
	static_assert(sizeof(struct File) == 256);
	binaryname = "fs";
	cprintf("FS is running\n");

	// Check that we are able to do I/O
	outw(0x8A00, 0x8A00);
	cprintf("FS can do I/O\n");

	serve_init();
	fs_init();
	serve();
}

