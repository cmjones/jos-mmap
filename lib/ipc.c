// User-level IPC library routines

#include <inc/lib.h>

// Receive a value via IPC and return it.
// If 'pg' is nonnull, then any page sent by the sender will be mapped at
//	that address.
// If 'from_env_store' is nonnull, then store the IPC sender's envid in
//	*from_env_store.
// If 'perm_store' is nonnull, then store the IPC sender's page permission
//	in *perm_store (this is nonzero iff a page was successfully
//	transferred to 'pg').
// If the system call fails, then store 0 in *fromenv and *perm (if
//	they're nonnull) and return the error.
// Otherwise, return the value sent by the sender
//
// Hint:
//   Use 'thisenv' to discover the value and who sent it.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value, since that's
//   a perfectly valid place to map a page.)
//
// CHALLENGE:
//  For backwards compatability, this function now emulates the expected
//  behavior of recieving a message from any environment.
int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
	return ipc_recv_src(0, from_env_store, pg, perm_store);
}


// CHALLENGE:  This new function allows a reciever to dictate which environmnet
//  is allowed to send it a message.  A 0 means that any environment is allowed.
//
// Errors and parameters are otherwise as in ipc_recv
int32_t
ipc_recv_src(envid_t source, envid_t *from_env_store, void *pg, int *perm_store)
{
	int32_t retval;

	// If pg is null, pass 0xFFFFFFFF as a pointer, which is above UTOP :)
	if(pg == NULL) pg = (void *)(-1);

	// Attempt the system call
	if((retval = sys_ipc_recv(source, pg)) != 0) {
		// We failed, set values for non null pointers
		if(from_env_store != 0) *from_env_store = 0;
		if(perm_store != 0) *perm_store = 0;

		// return the error
		return retval;
	}

	// Success!  Let's see what we got
	if(from_env_store != NULL) *from_env_store = thisenv->env_ipc_from;
	if(perm_store != NULL) *perm_store = thisenv->env_ipc_perm;

	// Return the return value!
	return thisenv->env_ipc_value;
}

// Send 'val' (and 'pg' with 'perm', if 'pg' is nonnull) to 'toenv'.
// This function keeps trying until it succeeds.
// It should panic() on any error other than -E_IPC_NOT_RECV.
//
// Hint:
//   Use sys_yield() to be CPU-friendly.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value.)
void
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
	int retval;

	// If pg is NULL, set it to 0xFFFFFFFF, which is above UTOP
	if(pg == NULL) pg = (void *)(-1);

	// Begin the loop!
	while((retval = sys_ipc_try_send(to_env, val, pg, perm)) == -E_IPC_NOT_RECV)
		// Use sys_yield() to pause between each try
		sys_yield();

	// If the return value was an error, panic!
	if(retval == -E_BAD_ENV) panic("ipc_send called with a bad envid");
	if(retval == -E_INVAL) panic("ipc_send called with invalid parameters");
	if(retval == -E_NO_MEM) panic("ipc_send ran out of memory");
	if(retval != 0) panic("ipc_send failed with an unknown error");
}

// Find the first environment of the given type.  We'll use this to
// find special environments.
// Returns 0 if no such environment exists.
envid_t
ipc_find_env(enum EnvType type)
{
	int i;
	for (i = 0; i < NENV; i++)
		if (envs[i].env_type == type)
			return envs[i].env_id;
	return 0;
}
