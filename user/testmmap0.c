#include <inc/lib.h>

#define TESTNUM 4
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

	char *content;
	char fread_buf[512];

	// First, open file 'lorem' and get the file id.
	if ((r_open = open("/lorem", O_RDONLY)) < 0)
		panic("mmap(): opening file failed, ERROR CODE: %d \n", r_open);
	fileid = fgetid(r_open);

	cprintf("Test directly mmaping a file via fs ipc request.\n");
	content = (char *)0x20005000;
	uint32_t perm = PTE_U | PTE_W | PTE_SHARE;
	int ret_rb = request_block(fileid, 0, content, perm);
	if (ret_rb < 0)
		panic("REQUEST FIIALED! : %d\n", ret_rb);

	cprintf("=> Read from mmapped region:\n\t%30s\n", content);
}
