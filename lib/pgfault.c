// User-level page fault handler support.
// Rather than register the C page fault handler directly with the
// kernel as the page fault handler, we register the assembly language
// wrapper in pfentry.S, which in turns calls the registered C
// function.

#include <inc/lib.h>

// Assembly language pgfault entrypoint defined in lib/pfentry.S.
extern void _pgfault_upcall(void (*handler)(struct UTrapframe *utf));

// Allocat a page for the exception stack if one doesn't already exist
static void
allocate_exception_stack()
{
	if(thisenv->env_pgfault_upcall == NULL) {
		// First time through! 0 is the id for the
		//  current environment.
		if(sys_page_alloc(0, (void *)(UXSTACKTOP-PGSIZE), PTE_U|PTE_W) != 0)
			panic("could not allocate room for exception stack\n");

		sys_env_set_pgfault_upcall(0, _pgfault_upcall);
	}
}

//
// Set a global page fault handler function.  Allocate an exception
// stack if necessary (the first time a handler is registered).
//
// The first time we register a handler, we need to
// allocate an exception stack (one page of memory with its top
// at UXSTACKTOP).  The kernel will call our handler with an
// assembly routine that returns back to user code directly rather
// than going back through the kernel.
//
void
set_pgfault_handler(void (*handler)(struct UTrapframe *utf))
{
	// Allocate an exception stack if necessary
	allocate_exception_stack();

	// Tell the kernel about the page fault handler
	sys_env_set_global_pgfault(0, handler);
}

//
// Set a region page fault handler function.  This is basically
// a page fault handler that is associated with a given address
// range, and is called only when an address in that range faults.
//
// The address range is:
//	 minaddr <= addr < maxaddr
//
void
set_pgfault_region_handler(void (*handler)(struct UTrapframe *utf), void *minaddr, void *maxaddr)
{
	// Allocate an exception stack if necessary
	allocate_exception_stack();

	// Tell the kernel about the page fault handler
	sys_env_set_region_pgfault(0, handler, minaddr, maxaddr);
}
