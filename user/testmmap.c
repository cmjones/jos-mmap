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

		char *content = (char *)0x20005000;
		uint32_t perm = PTE_U | PTE_W | PTE_SHARE;
		int ret_rb = request_block(fd, 0, content, perm);
		if (ret_rb < 0)
			panic("FIALEDDDD! : %d\n", ret_rb);

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
