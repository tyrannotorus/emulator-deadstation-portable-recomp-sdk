/* (dev) A sampling profiler for the Brick, which has no perf: DSP_PROF=FILE samples the program counter of whichever thread runs
 * on the process's CPU time every millisecond (ITIMER_PROF, SIGPROF), from the first frame to exit, and writes the samples there:
 * a line "base 0x..." (the executable's load address) and one line per sample "pc thread lr". dev/prof.py resolves them against the
 * executable's symbols. */
#define _GNU_SOURCE
#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROF_MAX (1u << 21)	/* samples kept: ~35 minutes of one busy core */

static uint64_t *s_pc, *s_lr;	/* the sampled pc and the link register (a leaf's caller: who called memcpy) */
/* DSP_PROF_STACK=1: four more frames a sample from the unwinder (glibc backtrace: .eh_frame, no frame pointers needed), for the
 * chain into a library's own copies */
#define PROF_FRAMES 4
static uint64_t *s_bt;
static int s_stack;
static uint32_t *s_tid;
static volatile uint32_t s_n;
static const char *s_path;

static void on_prof(int sig, siginfo_t *si, void *ctx)
{
	(void)sig;
	(void)si;
	const uint32_t i = __atomic_fetch_add(&s_n, 1u, __ATOMIC_RELAXED);
	if (i >= PROF_MAX)
		return;
	const ucontext_t *uc = (const ucontext_t *)ctx;
#if defined(__aarch64__)
	s_pc[i] = uc->uc_mcontext.pc;
	s_lr[i] = uc->uc_mcontext.regs[30];
#elif defined(__x86_64__)
	s_pc[i] = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
	s_lr[i] = 0;
#endif
	s_tid[i] = (uint32_t)syscall(SYS_gettid);
	if (s_stack && s_bt) {
		void *bt[3 + PROF_FRAMES];
		const int n = backtrace(bt, 3 + PROF_FRAMES);
		/* bt[0] this handler, bt[1] the signal trampoline, bt[2] the interrupted function: its callers from bt[3] */
		for (int k = 0; k < PROF_FRAMES; k++)
			s_bt[(size_t)i * PROF_FRAMES + (size_t)k] = 3 + k < n ? (uint64_t)(uintptr_t)bt[3 + k] : 0u;
	}
}

/* (dev) a frame handed out: a marker among the samples (pc 0, thread 0, the frame's length in ns where the link register goes), so
 * a late frame's samples can be told from the others (scratch: late-frame analysis) */
void brick_prof_frame(uint64_t frame_ns)
{
	if (!s_pc)
		return;
	const uint32_t i = __atomic_fetch_add(&s_n, 1u, __ATOMIC_RELAXED);
	if (i >= PROF_MAX)
		return;
	s_pc[i] = 0;
	s_lr[i] = frame_ns;
	s_tid[i] = 0;
}
static int base_of_exe(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	if (info->dlpi_name == NULL || info->dlpi_name[0] == '\0') {
		*(uint64_t *)data = (uint64_t)info->dlpi_addr;
		return 1;
	}
	return 0;
}

/* each thread's CPU time (utime + stime, clock ticks) from /proc/self/task: at the start of sampling and at the end, so the report
 * says how busy each thread was over the window (a thread at 100% is the limit; none at 100% means a wait, the GPU's or a lock) */
#include <dirent.h>
#define PROF_TASKS 64
static struct { unsigned tid; unsigned long long ticks; char name[20]; } s_task0[PROF_TASKS], s_task1[PROF_TASKS];
static int s_ntask0, s_ntask1;
static struct timespec s_t0, s_t1;
static int read_tasks(void *out_, int *n)
{
	typeof(s_task0[0]) *out = out_;
	*n = 0;
	DIR *d = opendir("/proc/self/task");
	if (d == NULL)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL && *n < PROF_TASKS) {
		if (e->d_name[0] == '.')
			continue;
		char path[64], buf[512];
		snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
		FILE *f = fopen(path, "r");
		if (f == NULL)
			continue;
		const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[len] = '\0';
		char *open = strchr(buf, '('), *close = strrchr(buf, ')');
		if (open == NULL || close == NULL)
			continue;
		unsigned long long ut = 0, st = 0;
		/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
		if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2)
			continue;
		out[*n].tid = (unsigned)atoi(e->d_name);
		out[*n].ticks = ut + st;
		const size_t nl = (size_t)(close - open - 1) < sizeof(out[*n].name) - 1 ? (size_t)(close - open - 1) : sizeof(out[*n].name) - 1;
		memcpy(out[*n].name, open + 1, nl);
		out[*n].name[nl] = '\0';
		(*n)++;
	}
	closedir(d);
	return 1;
}

static void prof_write(void)
{
	struct itimerval off;
	memset(&off, 0, sizeof(off));
	setitimer(ITIMER_PROF, &off, NULL);
	clock_gettime(CLOCK_MONOTONIC, &s_t1);
	read_tasks(s_task1, &s_ntask1);
	const double secs = (double)(s_t1.tv_sec - s_t0.tv_sec) + (double)(s_t1.tv_nsec - s_t0.tv_nsec) * 1e-9;
	const double hz = (double)sysconf(_SC_CLK_TCK);
	if (s_ntask0 > 0 && secs > 0.5)
		for (int i = 0; i < s_ntask1; i++)
			for (int j = 0; j < s_ntask0; j++)
				if (s_task0[j].tid == s_task1[i].tid && s_task1[i].ticks > s_task0[j].ticks)
					fprintf(stderr, "prof: thread %u (%s) busy %.0f%% of the %.1f s sampled\n", s_task1[i].tid, s_task1[i].name,
							100.0 * (double)(s_task1[i].ticks - s_task0[j].ticks) / hz / secs, secs);
	FILE *f = fopen(s_path, "w");
	if (f == NULL) {
		fprintf(stderr, "prof: %s can't be written\n", s_path);
		return;
	}
	uint64_t base = 0;
	dl_iterate_phdr(base_of_exe, &base);
	fprintf(f, "base 0x%llx\n", (unsigned long long)base);
	/* the libraries' mappings, for samples outside the executable: "map start end offset path" */
	FILE *m = fopen("/proc/self/maps", "r");
	if (m != NULL) {
		char line[512];
		while (fgets(line, sizeof(line), m) != NULL) {
			unsigned long long a, b, off;
			char perms[8], path[400] = "";
			if (sscanf(line, "%llx-%llx %7s %llx %*s %*s %399s", &a, &b, perms, &off, path) >= 4 && perms[2] == 'x' && path[0] == '/')
				fprintf(f, "map %llx %llx %llx %s\n", a, b, off, path);
		}
		fclose(m);
	}
	const uint32_t n = s_n < PROF_MAX ? s_n : PROF_MAX;
	for (uint32_t i = 0; i < n; i++)
	{
		fprintf(f, "%llx %u %llx", (unsigned long long)s_pc[i], s_tid[i], (unsigned long long)s_lr[i]);
		for (int k = 0; s_stack && s_bt && k < PROF_FRAMES; k++)
			fprintf(f, " %llx", (unsigned long long)s_bt[(size_t)i * PROF_FRAMES + (size_t)k]);
		fprintf(f, "\n");
	}
	fclose(f);
	fprintf(stderr, "prof: %u samples to %s\n", n, s_path);
}

#include <pthread.h>
#include <time.h>
static unsigned s_after;
void brick_affinity(int core);
static void *prof_later(void *arg)
{
	brick_affinity(0);
	(void)arg;
	sleep(s_after);
	struct itimerval it;
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 1000;
	it.it_value = it.it_interval;
	clock_gettime(CLOCK_MONOTONIC, &s_t0);
	read_tasks(s_task0, &s_ntask0);
	setitimer(ITIMER_PROF, &it, NULL);
	fprintf(stderr, "prof: sampling from now\n");
	return NULL;
}

/* DSP_PROF_AFTER=seconds: the sampling starts that long after the first frame */
void brick_prof_start(void)
{
	s_path = getenv("DSP_PROF");
	if (s_path == NULL || s_path[0] == '\0')
		return;
	s_pc = calloc(PROF_MAX, sizeof(*s_pc));
	s_lr = calloc(PROF_MAX, sizeof(*s_lr));
	s_stack = getenv("DSP_PROF_STACK") != NULL;
	if (s_stack) {
		s_bt = calloc((size_t)PROF_MAX * PROF_FRAMES, sizeof(*s_bt));
		void *warm[4];
		backtrace(warm, 4);   /* loads the unwinder now, not in the signal handler */
	}
	s_tid = calloc(PROF_MAX, sizeof(*s_tid));
	if (s_pc == NULL || s_lr == NULL || s_tid == NULL)
		return;
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigaction(SIGPROF, &sa, NULL);
	atexit(prof_write);
	const char *after = getenv("DSP_PROF_AFTER");
	s_after = after ? (unsigned)atoi(after) : 0u;
	pthread_t t;
	pthread_create(&t, NULL, prof_later, NULL);
	pthread_detach(t);
	fprintf(stderr, "prof: sampling every 1 ms of CPU time to %s\n", s_path);
}
