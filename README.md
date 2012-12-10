JOS mmap() Library
==================

This is a POSIX-compliant implementation of the
[mmap()](http://en.wikipedia.org/wiki/Mmap) library, built on top
of JOS, a exokernel-style operating system.

mmap is a method of memory-mapped I/O, which allows user programs to map a
memory object, such a partial or complete file, to their environment's virtual
address space. Pages are mapped lazily and are retrieved on-demand.

***

```C
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
```

```C
int munmap(void *addr, size_t len);
```
