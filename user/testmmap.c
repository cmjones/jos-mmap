#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	cprintf("\nRunning testmmap...\n");
	mmap((void *) NULL, (size_t) 10, 1, 0, 3, (off_t) 0);
	//munmap((void *) -1, (size_t) 1);
}
