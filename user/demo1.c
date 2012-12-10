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

	char *string = "MMAP IS COOL ";

	// First, open file 'lorem' and get the file id.
	if ((r_open = open("/lorem", O_RDONLY)) < 0)
		panic("mmap(): opening file failed, ERROR CODE: %d \n", r_open);
	fileid = fgetid(r_open);

	// Start testing.
	cprintf("\nTest mmaping file as PRIVATE, read from it, print out the content.\n"
		"Change some content, read the file again and check the content.\n"
                "Then munmap the region and try to read again.\n");
	length = 500;
	mmaped_addr = mmap(NULL, length, PTE_W, MAP_PRIVATE, r_open, (off_t) 0);
	content = (char *) mmaped_addr;
	cprintf("=> Read from mmaped region:\n\t%.30s\n", content);

	cprintf("=> Make some changes to file...\n");
	for (change_i = 0; change_i < 13; change_i += 1) {
		content[change_i] = string[change_i];
	}

	cprintf("=> Read from the mmaped region...\n");
	cprintf("\t%.30s\n", content);

	cprintf("=> Read directly from the FS...\n");
	cprintf("=> Correct behavior shows different contents b/c of COW\n");
	read(r_open, fread_buf, length);
	cprintf("\t%.30s\n", (char *) fread_buf);

	msync(content, length, 0);

	cprintf("=> Unmap the region.\n");
	r_munmap = munmap(mmaped_addr, length);
	cprintf("=> munmap() - return %d \n", r_munmap);
	cprintf("=> Try to read again (PGFLT expected).\n");
	cprintf("=> Read from mmapped region after munmap:\n\t%.30s\n", content);

}
