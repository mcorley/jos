#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/monitor.h>


// Choose a user environment to run and run it.
void
sched_yield(void)
{
	// Implement simple round-robin scheduling.
	// Search through 'envs' for a runnable environment,
	// in circular fashion starting after the previously running env,
	// and switch to the first such environment found.
	// It's OK to choose the previously running env if no other env
	// is runnable.
	// But never choose envs[0], the idle environment,
	// unless NOTHING else is runnable.

	int i, k;
	bool search = 1;

	// Get the offset into envs of the currently running environment.
	k = curenv ? ENVX(curenv->env_id) : 0;	
	i = k+1;

	while (search) {
		if (envs[i].env_status == ENV_RUNNABLE) {
			env_run(&envs[i]);
			search = 0;
		}
		++i;
		if (i % NENV == 0) i = 1;
		if (i == k+1) search = 0;
	}

	// If we went through all the envs and found none runnable, we proceed
	// and run the special idle environment, otherwise we must have started
	// a runnable environment and can just return.
	if (i != k+1) return; 

	// Run the special idle environment when nothing else is runnable.
	if (envs[0].env_status == ENV_RUNNABLE)
		env_run(&envs[0]);
	else {
		cprintf("Destroyed all environments - nothing more to do!\n");
		while (1)
			monitor(NULL);
	}
}
