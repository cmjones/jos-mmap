/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	struct Env *e;
	envid_t retval;

	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.
	retval = env_alloc(&e, curenv->env_id);

	// Set the new environment to be not runnable, and to return 0 if
	//  the environment allocation was successful.
	if(retval == 0) {
		e->env_status = ENV_NOT_RUNNABLE;
		e->env_tf = curenv->env_tf;
		e->env_tf.tf_regs.reg_eax = 0;
		retval = e->env_id;
	}

	return retval;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	struct Env *e;

	// Hint: Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.
	//
	// First try to find the correct environment
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Next check that the status is valid
	if(!(status == ENV_RUNNABLE || status == ENV_NOT_RUNNABLE)) return -E_INVAL;

	// Finally, make the change
	e->env_status = status;
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3) with interrupts enabled.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//
// The Trapframe pointer is checked in the dispatch function
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	struct Env *e;

	// Grab the target environment
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// If the environment is running, don't replace the
	// trap frame!!!
	if(e->env_status == ENV_RUNNING) return -E_BAD_ENV;

	// Copy the trap frame to the target environment to avoid the
	//  caller being able to change the environment's trap frame
	//  after setting it.
	memcpy((void *)&e->env_tf, (void *)tf, sizeof(struct Trapframe));

	// Set values in the new trap frame to ensure that the
	//  environment runs at CPL 3 and has interrupts enabled.
	//  The former is done by setting the low bits on all the
	//  segment registers.
	e->env_tf.tf_cs |= 3;
	e->env_tf.tf_ds |= 3;
	e->env_tf.tf_es |= 3;
	e->env_tf.tf_ss |= 3;
	e->env_tf.tf_eflags |= FL_IF;

	// All set
	return 0;
}

// Set the page fault upcall for the given environment.  This upcall is run in user
//  mode, and passed a handler function as an argument.  The idea is to allow this
//  function to return directly to the faulting instruction as opposed to returning
//  through the kernel.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env *e;

	// Grab the environment
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Now set the handler
	e->env_pgfault_upcall = func;
	return 0;
}

// Set the global page fault handler for 'envid' by modifying the corresponding
// struct Env's 'env_pgfault_global' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_global_pgfault(envid_t envid, void *func)
{
	struct Env *e;

	// Grab the environment
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Now set the handler
	e->env_pgfault_global = func;
	return 0;
}

// Sets a page fault handler for a specific range of addresses for 'envid.'  If
//  a page fault occurs within the given range, the installed handler will be
//  called.  Otherwise, the Env's 'env_pgfault_global' will be called if it exists.
//
// If func is NULL, any page fault handlers in the passed region will be removed.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_NO_MEM if adding this handler would bring the number of handlers
//		above MAXHANDLERS
//	-E_INVAL if either address is not page aligned, or the specified
//		range includes part of the range of a previously
//		installed handler.
static int
sys_env_set_region_pgfault(envid_t envid, void *func, uint32_t minaddr, uint32_t maxaddr)
{
	struct Env *e;
	int i, j;
	int dst = -1;

	// Sanity-check the addresses
	if(minaddr%PGSIZE != 0 || maxaddr%PGSIZE != 0) return -E_INVAL;

	// Grab the environment
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	if(func != NULL) {
		// Find an empty slot to put the new handler, or find a previous
		//  handler that will be deleted upon the installation of the new
		//  one.  If no such slot exists, this call should fail.
		for(i = 0; i < MAXHANDLERS; i++) {
			if(e->env_pgfault_handlers[i].erh_handler == 0 ||
			   (e->env_pgfault_handlers[i].erh_minaddr >= minaddr &&
			    e->env_pgfault_handlers[i].erh_maxaddr < maxaddr)) {
				dst = i;
				break;
			}
		}
		if(i == MAXHANDLERS) return -E_NO_MEM;
	}

	// Step through the currently installed handlers and make room for
	//  the new handler.
	//
	// Because of the invariant that no ranges overlap, the only ways
	//  adding a new handler can fail is if there are already MAXHANDLERS
	//  handlers installed, or the new range lies completely within a
	//  previous range (in which case the old handler will have to be
	//  split into two new ones.
	for(i = 0; i < MAXHANDLERS; i++) {
		// Check if the handler exists
		if(e->env_pgfault_handlers[i].erh_handler == NULL)
			continue;

		// If the new range is a subset of the old range, we may
		//  have to ensure there is empty space in the array
		if(e->env_pgfault_handlers[i].erh_minaddr < minaddr &&
		   e->env_pgfault_handlers[i].erh_maxaddr > maxaddr) {
			// Since the old range will have to be split into
			//  two, find a location to put the second piece.
			for(j = dst+1; j < MAXHANDLERS; j++) {
				if(e->env_pgfault_handlers[j].erh_handler == NULL) {
					// Split the handler into two pieces, with the old slot
					//  contianing the bottom half.
					e->env_pgfault_handlers[j].erh_handler = e->env_pgfault_handlers[i].erh_handler;
					e->env_pgfault_handlers[j].erh_minaddr = maxaddr;
					e->env_pgfault_handlers[j].erh_maxaddr = e->env_pgfault_handlers[i].erh_maxaddr;
					e->env_pgfault_handlers[i].erh_maxaddr = minaddr;

					// Because no ranges overlap, the new range will
					//  not overlap any other handler's ranges.
					goto success;
				}
			}

			// If we make it here, there wasn't room to split
			//  the old handler.
			return -E_NO_MEM;
		}

		// If the new range is a superset of the old range, the old
		//  handler should be deleted.
		if(e->env_pgfault_handlers[i].erh_minaddr >= minaddr &&
		   e->env_pgfault_handlers[i].erh_maxaddr <= maxaddr)
			e->env_pgfault_handlers[i].erh_handler = NULL;

		// If the new and old range overlap, adjust the old range.
		if(e->env_pgfault_handlers[i].erh_minaddr < maxaddr &&
		   e->env_pgfault_handlers[i].erh_maxaddr > maxaddr)
			e->env_pgfault_handlers[i].erh_minaddr = maxaddr;
		if(e->env_pgfault_handlers[i].erh_minaddr < minaddr &&
		   e->env_pgfault_handlers[i].erh_maxaddr > minaddr)
			e->env_pgfault_handlers[i].erh_maxaddr = minaddr;
	}

	success:
	if(func != NULL) {
		// Once we've reached here, we can install the new handler at slot dst
		e->env_pgfault_handlers[dst].erh_handler = func;
		e->env_pgfault_handlers[dst].erh_minaddr = minaddr;
		e->env_pgfault_handlers[dst].erh_maxaddr = maxaddr;
	}

	// All done
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_SYSCALL in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	struct Env *e;
	struct PageInfo *pi;

	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!
	//
	// Sanity check the permissions bits: PTE_P is automatically
	//  set, so that bit won't be necessary to provide unless
	//  a test case fails. ;)
	if((perm|PTE_U) == 0 || (perm&(!(PTE_U|PTE_W|PTE_AVAIL))) != 0) return -E_INVAL;

	// Sanity check the virtual address
	if((int)va >= UTOP || (int)va%PGSIZE != 0) return -E_INVAL;

	// Check that the environment id is correct
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Finally, attempt to allocate the new page using a nice
	//  little gotOMG IT'S A RAPTOR!
	if((pi = page_alloc(ALLOC_ZERO)) != NULL) {
		if(page_insert(e->env_pgdir, pi, va, perm) == 0) {
			goto success;
		}
		page_free(pi);
	}

	// Allocation failed... :(
	return -E_NO_MEM;

	// Allocation and insertion succeeded!
	success:
	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	struct Env *src, *dst;
	struct PageInfo *pi;
	pte_t *pte;

	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.
	//
	// Sanity check the virtual addresses
	if((int)srcva >= UTOP || (int)srcva%PGSIZE != 0 ||
	   (int)dstva >= UTOP || (int)dstva%PGSIZE != 0)
		return -E_INVAL;

	// Now grab each of the environments
	if(envid2env(srcenvid, &src, 1) != 0 || envid2env(dstenvid, &dst, 1) != 0)
		return -E_BAD_ENV;

	// Grab the permissions of the source page
	if((pi = page_lookup(src->env_pgdir, srcva, &pte)) == NULL) return -E_INVAL;

	// Sanity check the permission bits.  The last boolean is a complicated
	//  lookup to check if the mapped page is writable.  If it isn't, and the
	//  new permissions are writable, the permissions are invalid.
	if((perm|PTE_U) == 0 ||
	   (perm&(~PTE_SYSCALL)) != 0 ||
	   ((perm&PTE_W) != 0 && (*pte&PTE_W) == 0))
		return -E_INVAL;

	// Now attempt to insert the src page into the destination.  Returns 0 on
	//  success and -E_NO_MEM on failure.
	return page_insert(dst->env_pgdir, pi, dstva, perm);
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	struct Env *e;

	// Hint: This function is a wrapper around page_remove().
	//
	// Sanity check the virtual address
	if((int)va >= UTOP || (int)va%PGSIZE != 0) return -E_INVAL;

	// Check that the environment id is correct
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Now remove the page
	page_remove(e->env_pgdir, va);
	return 0;
}

// Reserves a contiguous block of 'pgnum' free pages of address space starting from
//  the page containing 'va'.  No physical memory is allocated; rather, each page
//  table entry is marked with 'perm', which must not have PTE_P set.
//
// The function allocates address space as best it can, so if there is not enough
//  free space at 'va,' it will scan forward through memory until a suitable space
//  is found.  Other reserved pages will be counted as taken.
//
// perm -- PTE_P must not be set.  Any other data in the other 31 bits are fine.
//
// Return va pointer to the first available page on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new block of pages.
static int
sys_page_reserve(envid_t envid, void *va, int pgnum, int perm)
{
	void *tmpva;
	void *retva;
	int counter;
	struct Env *e;
	struct PageInfo *pi;
	pte_t *pte;

	// Check that perm does not have PTE_P set
	if(perm&PTE_P) return -E_INVAL;

	// Check that the environment id is correct
	if(envid2env(envid, &e, 1) != 0) return -E_BAD_ENV;

	// Sanity check the virtual address
	if(va) {
		if((uint32_t)va >= UTOP || (uint32_t)va%PGSIZE != 0) {
			cprintf("sys_page_block_alloc() - invalid va. \n");
			return -E_INVAL;
		}
		tmpva = ROUNDDOWN(va, PGSIZE);
	} else {
		// If va is not provided, give an initial value
		// tmpva = (void *) page2kva(get_free_page()); // TODO: check correctness
		tmpva = (void *) UTEXT;
	}

	// Scan the memory for free pages
	counter = 0;
	cprintf("Scanning memory for %d free page(s)...\n", pgnum);
	while (counter < pgnum) {
		// cprintf("tmpva: %p \t counter: %d\n", (uint32_t) tmpva, counter);
		// If 'tmpva' is out of bound, fail
		if((uint32_t)tmpva >= UTOP) {
			cprintf("tmpva out of bound. fail.\n");
			return -E_INVAL;
		}

		// Test free pages
		if((pte = pgdir_walk(e->env_pgdir, tmpva, 0)) == NULL || *pte == 0) {
			counter++;
		} else {
			counter = 0;
		}
		tmpva += PGSIZE;
	}

	// If reached here, proper-sized block of free pages is found.
	// Now reserve.
	tmpva -= pgnum * PGSIZE; // back to the start of the block
	retva = tmpva;
	for (counter = 0; counter < pgnum; counter++, tmpva += PGSIZE) {
		pte = pgdir_walk(e->env_pgdir, tmpva, 1);
		*pte = perm;
	}

	// Success on all pages, return the start of the block
	return (int)retva; // need casting when used
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// CHALLENGE: The send may also fail if the target is blocked, but
//  waiting for a message from a specific environment that is not
//  this one.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first, or envid
//              has specified a source other than the current environment
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *target;
	struct PageInfo *pi;
	pte_t *pte;
	int retval = 0;

	// Grab the target environment without checking permissions
	if((retval = envid2env(envid, &target, 0)) != 0) return retval;

	// Ensure that the target is recieving ipc messages
	if(!target->env_ipc_recving) return -E_IPC_NOT_RECV;

	// Ensure that the target wants to hear a message from us
	if(target->env_ipc_from != 0 && target->env_ipc_from != curenv->env_id)
		return -E_IPC_NOT_RECV;

	// If the target is recieving environment wants a page and we're
	//  sending one, try to send the page.
	if((int)target->env_ipc_dstva < UTOP && (int)srcva < UTOP) {
		// Sanity check the src address and permissions
		if((unsigned int)srcva%PGSIZE != 0) return -E_INVAL;

		// Grab the permissions of the source page
		if((pi = page_lookup(curenv->env_pgdir, srcva, &pte)) == NULL)
			return -E_INVAL;

		// Check that we aren't mapping a read-only page
		//  to be writable
		if((perm&PTE_W) && ((*pte)&PTE_W) == 0) return -E_INVAL;

		// Finally, attempt to install the new mapping to the target
		//  environment.
		if((retval = page_insert(target->env_pgdir, pi, target->env_ipc_dstva, perm)) != 0)
			return retval;

		// Success! Signal that a page was transferred
		target->env_ipc_perm = perm;
	}

	// From here on, we can't fail, so set all the receiving data
	target->env_ipc_value = value;
	target->env_ipc_from = curenv->env_id;
	target->env_ipc_recving = 0;

	// Set the target to be runnable again, then return
	target->env_status = ENV_RUNNABLE;
	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
//
// CHALLENGE: You may also specify a source environment.  If this is not
//  0, then only messages from that environment will be recieved. Passing
//  0 for the source allows this environment to receive messages from
//  anybody as per the old behavior.
//
// New possible error:
//      -E_BAD_ENV if the source environment doesn't exist
static int
sys_ipc_recv(envid_t source, void *dstva)
{
	struct Env *env;

	// Ensure the source environment exists
	if(source != 0 && envid2env(source, &env, 0) != 0) return -E_BAD_ENV;

	// Sanity-check dstva
	if((unsigned int)dstva < UTOP && (int)dstva%PGSIZE != 0)
		return -E_INVAL;

	// Set up this environment to recieve ipcs
	curenv->env_ipc_recving = true;
	curenv->env_ipc_dstva = dstva;
	curenv->env_ipc_value = 0;
	curenv->env_ipc_from = source;
	curenv->env_ipc_perm = 0;

	// Now set this environment to be not runnable, and
	//  schedule a new environment to run on this cpu.
	//  This way, the environment won't run again until
	//  it receives an ipc.
	curenv->env_status = ENV_NOT_RUNNABLE;
	sched_yield();

	// Technically this is never run
	return 0;
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	int i;

	// The default return value is 0 for success
	int retval = 0;

	// Switch on the system call
	switch(syscallno) {
	case SYS_cputs:
		// a1 contains a pointer to the string, a2 contains the length
		user_mem_assert(curenv, (void *)a1, a2, PTE_U);
		sys_cputs((char *)a1, a2);
		break;
	case SYS_cgetc:
		retval = sys_cgetc();
		break;
	case SYS_getenvid:
		retval = sys_getenvid();
		break;
	case SYS_env_destroy:
		// a1 contains the id to destroy
		retval = sys_env_destroy(a1);
		break;
	case SYS_yield:
		// sys_yield doesn't return until this env is run again
		sys_yield();
		break;
	case SYS_exofork:
		retval = sys_exofork();
		break;
	case SYS_env_set_status:
		retval = sys_env_set_status(a1, a2);
		break;
	case SYS_page_alloc:
		retval = sys_page_alloc(a1, (void *)a2, a3);
		break;
	case SYS_page_map:
		retval = sys_page_map(a1, (void *)a2, a3, (void *)a4, a5);
		break;
	case SYS_page_unmap:
		retval = sys_page_unmap(a1, (void *)a2);
		break;
	case SYS_page_reserve:
		retval = sys_page_reserve(a1, (void *)a2, a3, a4);
		break;
	case SYS_env_set_trapframe:
		// Double-check the Trapframe pointer
		user_mem_assert(curenv, (void *)a2, sizeof(struct Trapframe), PTE_U);
		retval = sys_env_set_trapframe(a1, (void *)a2);
		break;
	case SYS_env_set_pgfault_upcall:
		retval = sys_env_set_pgfault_upcall(a1, (void *)a2);
		break;
	case SYS_env_set_global_pgfault:
		retval = sys_env_set_global_pgfault(a1, (void *)a2);
		break;
	case SYS_env_set_region_pgfault:
		retval = sys_env_set_region_pgfault(a1, (void *)a2, a3, a4);
		break;
	case SYS_ipc_try_send:
		retval = sys_ipc_try_send(a1, a2, (void *)a3, a4);
		break;
	case SYS_ipc_recv:
		retval = sys_ipc_recv(a1, (void *)a2);
		break;
	default:
		// Unknown/unimplemented system call number
		retval = -E_INVAL;
	}

	return retval;
}
