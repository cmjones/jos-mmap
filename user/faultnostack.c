// test user fault handler being called with no exception stack mapped

#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	sys_env_set_global_pgfault(0, (void*) thisenv->env_pgfault_global);
	*(int*)0 = 0;
}
