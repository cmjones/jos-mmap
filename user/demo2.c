#include <inc/lib.h>

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

	int change_i;

	char *content;
	char fread_buf[512];

	int r_munmap;

	// First, open file 'lorem' and get the file id.
	if ((r_open = open("/lorem", O_RDONLY)) < 0)
		panic("mmap(): opening file failed, ERROR CODE: %d \n", r_open);
	fileid = fgetid(r_open);

	// Start testing.
	cprintf("\nTest mmaping file as SHARED, print out the content. Unmap the region. try read again.\n");
	length = PGSIZE;
	mmaped_addr = mmap(NULL, length, PTE_W, MAP_SHARED, r_open, (off_t) 0);
	content = (char *) mmaped_addr;
	cprintf("=> Read from mmapped region:\n\t%30s\n", content);

	cprintf("=> Now make some changes to file...\n");
	for (change_i = 0; change_i < length; change_i += 4) {
		content[change_i] = 'J';
	}

	cprintf("=> Now read from the mmaped region...\n");
	cprintf("\t%30s\n", content);

	cprintf("=> Now read directly from the FS...\n");
	cprintf("=> Correct behavior shows same content b/c of SHARED\n");
	read(r_open, fread_buf, length);
	cprintf("\t%30s\n", (char *) fread_buf);

	cprintf("=> Now unmap the region.\n");
	r_munmap = munmap(mmaped_addr, length);
	cprintf("=> munmap() - return %d \n", r_munmap);
	cprintf("=> Now try to read again (PGFLT expected).\n");
	cprintf("=> Read from mmapped region after munmap:\n\t%30s\n", content);
}
