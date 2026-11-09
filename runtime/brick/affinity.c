/* (dev) DSP_AFFINITY=1: the module's threads pinned to the Brick's four cores (the game's thread on core 2, the GE's list thread on
 * core 1, its renderer on core 3, the rest on core 0). Off by default: the kernel places them better. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

void brick_affinity(int core)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("DSP_AFFINITY");
		on = e && e[0] == '1';
	}
	if (!on || core < 0 || core > 3)
		return;
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(core, &set);
	const int r = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
	if (r != 0)
		fprintf(stderr, "brick: a thread could not be put on core %d (%d)\n", core, r);
}
