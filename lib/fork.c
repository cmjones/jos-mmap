// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;

	// Round the address down to the nearest page
	addr = ROUNDDOWN(addr, PGSIZE);

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).
	//
	// If the second bit of err is not set, the fault is a read
	if((err&2) == 0)
		panic("fault was not caused by a write\n");
	if((uvpt[PGNUM(addr)]&PTE_COW) == 0)
		panic("faulting page was not copy-on-write");

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
	//   No need to explicitly delete the old page's mapping.
	//
	// First attempt to allocate a new page at the temporary address
	if(sys_page_alloc(0, PFTEMP, PTE_U|PTE_W) != 0)
		panic("couldn't allocate a new page for copy-on-write");

	// Next, copy the memory from the old page to the new
	memcpy(PFTEMP, addr, PGSIZE);

	// Finally, remap the new page to the old address.  Don't bother
	//  cleaning up PFTEMP.
	if(sys_page_map(0, PFTEMP, 0, addr, PTE_U|PTE_W) != 0)
		panic("couldn't remap the temporary page for copy-on-write");
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int perm = uvpt[pn]&PTE_SYSCALL;

	// If the old permissions were write and the page isn't being shared,
	//  the new permissions should include COW and not include PTE_W.
	if((uvpt[pn]&(PTE_W|PTE_COW)) != 0 && (uvpt[pn]&PTE_SHARE) == 0) {
		perm |= PTE_COW;
		perm &= ~PTE_W;
	}

	// Now map the page to the new environment
	if(sys_page_map(0, (void *)(pn*PGSIZE), envid, (void *)(pn*PGSIZE), perm) != 0)
		panic("duppage: unable to map page 0x%x to child", pn*PGSIZE);

	// The old mapping must be converted to COW as well. This must be done
	//  after the child mapping because of mapping the user stack.  A fault
	//  happens immediately, switching that mapping to a writable page.  That
	//  mapping is then copied over to the child incorrectly.
	if(perm&PTE_COW) {
		if(sys_page_map(0, (void *)(pn*PGSIZE), 0, (void *)(pn*PGSIZE), perm) != 0)
			panic("duppage: unable to set permissions for own page");
	}

	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	envid_t child;
	unsigned pagenum;
	int i;

	// First set up the page fault handler
	set_pgfault_handler(pgfault);

	// Create a new child environment and store its id. Return the error
	//  on failure
	child = sys_exofork();
	if(child < 0) {
		// There was some error, return the error
		return child;
	} else if(child == 0) {
		// This is the child, thisenv needs to be fixed
		thisenv = &envs[ENVX(sys_getenvid())];
		return child;
	}

	// Map every page under UTOP using duppage.  Writable pages will
	//  be mapped to a COW page, and non-writable pages will be mapped
	//  to read-only pages.  Very simple.
	//
	// The only exception is the exception stack (located at UXSTACKTOP-
	//  PGSIZE).  A new page should be allocated in the child for this.
	for(pagenum = 0; pagenum < PGNUM(UTOP); pagenum++) {
		// Check to see if the page directory entry and page table
		//  entry for this page exist.
		if((uvpd[PDX(pagenum*PGSIZE)]&PTE_P) == 0 || (uvpt[pagenum]&PTE_P) == 0)
			continue;

		if(pagenum == PGNUM(UXSTACKTOP-PGSIZE))
			// Found the exception stack page
			sys_page_alloc(child, (void *)(pagenum*PGSIZE), PTE_U|PTE_W);
		else
			// Duplicate the page
			duppage(child, pagenum);
	}

	// Get the child environment struct and set its pagefault handlers
	//  by copying the ones from the parent.
	if(sys_env_set_pgfault_upcall(child, thisenv->env_pgfault_upcall) != 0)
		panic("unable to set child page fault upcall");
	if(sys_env_set_global_pgfault(child, thisenv->env_pgfault_global) != 0)
		panic("unable to set child page fault handler");
	for(i = 0; i < MAXHANDLERS; i++) {
		// Don't add a handler that doesn't exist
		if(thisenv->env_pgfault_handlers[i].erh_maxaddr == 0) continue;

		// Attempt to set the handler
		if(sys_env_set_region_pgfault(child,
					      thisenv->env_pgfault_handlers[i].erh_handler,
					      (void *)thisenv->env_pgfault_handlers[i].erh_minaddr,
					      (void *)thisenv->env_pgfault_handlers[i].erh_maxaddr) != 0);
			panic("unable to set child page fault handler");
	}

	// Finally, mark the child as runnable.
	if(sys_env_set_status(child, ENV_RUNNABLE) != 0)
		panic("can't set child status to runnable");

	// Return the child id
	return child;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
