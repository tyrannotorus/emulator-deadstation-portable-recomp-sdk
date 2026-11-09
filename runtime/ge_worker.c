/* The PSP's GE beside its CPU, natively: display lists run on a host thread of their own, as the GE ran them beside the Allegrex,
 * while the game's code goes on. The guest side (hle_ge.c) keeps every list's bookkeeping and posts work here
 * in order: a list to run (to its END or its stall address), a frame to present, VRAM the CPU wrote (the textures and targets there
 * dropped before the next list reads them). The worker hands back each run's end (the next address, 0 at END, and the FINISH id),
 * which the guest side takes at its next GE or display call: the finish callback then runs, as the PSP's finish interrupt would.
 *
 * A game that enqueues a frame's list after the flip and syncs at the start of the next frame has the whole GE (decoding, transform
 * setup, the Vulkan recording and submission) run while it computes the next frame.
 *
 * DSP_GE_SYNC=1 (dev): the lists run on the caller's thread (the runtime's own path). */
#include "recomp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
extern unsigned long g_ge_list_commands;
void gegpu_dev_counts(uint64_t out[5]) __attribute__((weak));
void gegpu_dev_phase_line(void) __attribute__((weak));
void ge_dev_drawn_fbs(char *out, size_t n) __attribute__((weak));   /* (dev) DSP_PRESENTLOG=1 */   /* (dev) SR_GPU_CPU_PROFILE=1: the renderer's phases */   /* the GPU backend's (ge_gpu.c): passes, batches, submits, texture
								   updates, new textures */
static uint64_t s_dirty_bytes;   /* the CPU's VRAM writes posted (dev) */
/* the GE in two stages: the renderer on a thread of its own after this one, fed in order (ge.c) */
int ge_split_on(void) __attribute__((weak));
void ge_split_post(void (*fn)(const void *), const void *arg, uint32_t bytes, int frame_end) __attribute__((weak));
void ge_split_drain(void) __attribute__((weak));
void ge_split_stop(void) __attribute__((weak));
void ge_split_dev(uint64_t *busy, uint64_t *longest) __attribute__((weak));
static int split(void) { return ge_split_on && ge_split_on(); }

enum { W_RUN, W_PRESENT, W_DIRTY, W_QUIT };
typedef struct {
	int kind, slot;
	uint32_t a, b, c, d;
} WCmd;
typedef struct {
	int slot;
	uint32_t next_pc, finish_id, stall;
} WDone;

#define WQ 1024
#define WD 256
#define SLOTS 64
#define DIRTY_RANGES 64	/* the CPU's VRAM writes between two posts: precise enough that scattered small writes don't drop
				 * unrelated textures (each dropped texture is compared byte for byte at its next use) */
#define PRESENTS_AHEAD 2	/* frames the game may run ahead of the worker's presents */

static WCmd s_q[WQ];
static unsigned s_qh, s_qt;
static WDone s_d[WD];
static unsigned s_dh, s_dt;
static int s_inflight[SLOTS];
static int s_presents;
static int s_busy;
static int s_owed;   /* runs' ends and presents posted to the renderer, not yet reached */
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_work = PTHREAD_COND_INITIALIZER, s_done = PTHREAD_COND_INITIALIZER;
static pthread_t s_thread;
static int s_started, s_on = -1;
static __thread int s_is_worker;
static void (*s_present)(uint32_t fb, int fmt, uint32_t stride);

/* the CPU's VRAM writes since the last post (the guest side only) */
static uint32_t s_dlo[DIRTY_RANGES], s_dhi[DIRTY_RANGES];
static int s_ndirty;

int ge_worker_on(void)
{
	if (s_on < 0) {
		const char *e = getenv("DSP_GE_SYNC");
		s_on = !(e && e[0] == '1');
		fprintf(stderr, "ge worker: %s\n", s_on ? "display lists on their own thread, beside the game" : "off (DSP_GE_SYNC=1)");
	}
	return s_on;
}

int ge_worker_is_self(void) { return s_is_worker; }

void ge_worker_set_presenter(void (*present)(uint32_t fb, int fmt, uint32_t stride)) { s_present = present; }

static void push_locked(WCmd c)
{
	while ((s_qt + 1u) % WQ == s_qh)
		pthread_cond_wait(&s_done, &s_m);
	s_q[s_qt] = c;
	s_qt = (s_qt + 1u) % WQ;
	pthread_cond_signal(&s_work);
}

/* fastmem's VRAM pages the game stored to since the last post: closed again and reported here */
void sr_fastmem_vram_flush(void) __attribute__((weak));

static void flush_dirty_locked(void)
{
	if (sr_fastmem_vram_flush)
		sr_fastmem_vram_flush();
	for (int i = 0; i < s_ndirty; i++) {
		WCmd c = { W_DIRTY, -1, s_dlo[i], s_dhi[i] - s_dlo[i], 0, 0 };
		push_locked(c);
	}
	s_ndirty = 0;
}

/* (dev) DSP_HLETIME=1: the worker's busy share, said once a second ("ge worker: busy N%, waited M% for work") */
static uint64_t wnow(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* on the renderer's thread, after the run's drawing: the run's end handed back */
static void done_reached(const void *arg)
{
	pthread_mutex_lock(&s_m);
	s_d[s_dt] = *(const WDone *)arg;
	s_dt = (s_dt + 1u) % WD;
	s_owed--;
	pthread_cond_broadcast(&s_done);
	pthread_mutex_unlock(&s_m);
}

typedef struct {
	uint32_t fb, fmt, stride;
} WPresent;
static void present_reached(const void *arg)
{
	const WPresent *p = arg;
	if (s_present)
		s_present(p->fb, (int)p->fmt, p->stride);
	pthread_mutex_lock(&s_m);
	s_presents--;
	s_owed--;
	pthread_cond_broadcast(&s_done);
	pthread_mutex_unlock(&s_m);
}

static void owe(void)
{
	pthread_mutex_lock(&s_m);
	s_owed++;
	pthread_mutex_unlock(&s_m);
}

void brick_affinity(int core) __attribute__((weak));
static void *worker(void *arg)
{
	(void)arg;
	s_is_worker = 1;
	if (brick_affinity)
		brick_affinity(1);	/* the GE's display-list thread: core 1 (brick/affinity.c) */
	const char *tm = getenv("DSP_HLETIME");
	const int timing = tm && tm[0] == '1';
	uint64_t t_last = wnow(), busy = 0, idle = 0, presents = 0, runs = 0, commands = 0, prims = 0, dev0[5] = { 0, 0, 0, 0, 0 };
	uint64_t frame_busy = 0, frame_longest = 0;
	pthread_mutex_lock(&s_m);
	for (;;) {
		const uint64_t w0 = timing ? wnow() : 0;
		while (s_qh == s_qt)
			pthread_cond_wait(&s_work, &s_m);
		const uint64_t w1 = timing ? wnow() : 0;
		idle += w1 - w0;
		const WCmd c = s_q[s_qh];
		s_qh = (s_qh + 1u) % WQ;
		s_busy = 1;
		pthread_cond_broadcast(&s_done);
		pthread_mutex_unlock(&s_m);
		WDone d = { -1, 0, 0, 0 };
		if (c.kind == W_RUN) {
			g_ge_stall_addr = c.b;
			g_ge_signal_stack_limit = c.d;
			int stopped = 0;
			const uint32_t prims0 = g_frame_prims;
			const uint32_t next = ge_run_list_until(c.a, (int)c.c, NULL, &stopped);
			commands += g_ge_list_commands;
			prims += g_frame_prims >= prims0 ? g_frame_prims - prims0 : g_frame_prims;
			g_ge_stall_addr = 0;
			char fault[160];
			if (ge_take_signal_fault(fault, sizeof(fault)))
				fprintf(stderr, "ge worker: a SIGNAL the GE driver refuses, the list stops there: %s\n", fault);
			d.slot = c.slot;
			d.next_pc = next;
			d.finish_id = ge_last_finish_id();
			d.stall = c.b;
			if (split() && d.slot >= 0) {
				owe();
				ge_split_post(done_reached, &d, sizeof d, 0);
				d.slot = -1;
			}
		} else if (c.kind == W_PRESENT) {
			{	/* (dev) DSP_PRESENTLOG=1: each present's framebuffer and what was drawn since, the first 600 */
				static int plog = -1, said;
				if (plog < 0) plog = getenv("DSP_PRESENTLOG") != NULL;
				if (plog && ge_dev_drawn_fbs && said < 4000) {
					static char last[256];
					char drawn[256];
					ge_dev_drawn_fbs(drawn, sizeof drawn);
					if (strcmp(drawn, last)) {   /* only when what was drawn changed */
						fprintf(stderr, "PRESENT %llu fb 0x%08x; drawn since:%s\n", (unsigned long long)wnow() / 1000000ull, c.a, drawn);
						said++;
						strcpy(last, drawn);
					}
				}
			}
			if (split()) {
				const WPresent p = { c.a, c.b, c.c };
				owe();
				ge_split_post(present_reached, &p, sizeof p, 1);
			} else if (s_present)
				s_present(c.a, (int)c.b, c.c);
		} else if (c.kind == W_QUIT) {
			if (ge_split_stop)
				ge_split_stop();
		} else if (c.kind == W_DIRTY) {
			sr_gpu_vram_dirty(c.a, c.b);
			s_dirty_bytes += c.b;
			static int said;
			if (timing && said < 40) {
				said++;
				fprintf(stderr, "ge worker: VRAM written by the CPU: 0x%08x, %u bytes\n", c.a, c.b);
			}
		}
		if (timing) {
			const uint64_t w2 = wnow();
			busy += w2 - w1;
			presents += c.kind == W_PRESENT;
			runs += c.kind == W_RUN;
			/* (dev) the longest frame of work: the busy time from one present to the next */
			frame_busy += w2 - w1;
			if (c.kind == W_PRESENT) {
				if (frame_busy > frame_longest) frame_longest = frame_busy;
				frame_busy = 0;
			}
			if (w2 - t_last >= 1000000000ull) {
				const double span = (double)(w2 - t_last);
				uint64_t dev[5] = { 0, 0, 0, 0, 0 };
				if (gegpu_dev_counts)
					gegpu_dev_counts(dev);
				const uint64_t pf = presents ? presents : 1u;
				fprintf(stderr, "ge worker: busy %.0f%%, waiting for work %.0f%%, %llu presents; a frame: %llu commands, %llu PRIMs, "
						"%llu render passes, %llu batches, %llu submits, %llu textures updated, %llu new; %llu KB of VRAM the CPU wrote; %llu runs; "
						"longest frame of work %.1f ms\n",
						100.0 * (double)busy / span, 100.0 * (double)idle / span, (unsigned long long)presents,
						(unsigned long long)(commands / pf), (unsigned long long)(prims / pf), (unsigned long long)((dev[0] - dev0[0]) / pf),
						(unsigned long long)((dev[1] - dev0[1]) / pf), (unsigned long long)((dev[2] - dev0[2]) / pf),
						(unsigned long long)((dev[3] - dev0[3]) / pf), (unsigned long long)((dev[4] - dev0[4]) / pf),
						(unsigned long long)(s_dirty_bytes / pf / 1024u), (unsigned long long)(runs / pf), (double)frame_longest * 1e-6);
				if (split() && ge_split_dev) {
					uint64_t rb = 0, rl = 0;
					ge_split_dev(&rb, &rl);
					fprintf(stderr, "ge renderer: busy %.0f%%, longest frame of work %.1f ms\n", 100.0 * (double)rb / span, (double)rl * 1e-6);
				}
				if (gegpu_dev_phase_line)
					gegpu_dev_phase_line();
				frame_longest = 0;
				s_dirty_bytes = 0;
				runs = 0;
				busy = idle = presents = commands = prims = 0;
				memcpy(dev0, dev, sizeof(dev0));
				t_last = w2;
			}
		}
		pthread_mutex_lock(&s_m);
		if (d.slot >= 0) {
			s_d[s_dt] = d;
			s_dt = (s_dt + 1u) % WD;
		}
		if (c.kind == W_PRESENT && !split())
			s_presents--;
		s_busy = 0;
		pthread_cond_broadcast(&s_done);
		if (c.kind == W_QUIT)
			break;
	}
	pthread_mutex_unlock(&s_m);
	return NULL;
}

static void start_locked(void)
{
	if (s_started)
		return;
	s_started = 1;
	pthread_create(&s_thread, NULL, worker, NULL);
}

/* run the list in `slot` from `pc` (resume: from where it stalled) to its END or `stall` */
void ge_worker_run(int slot, uint32_t pc, int resume, uint32_t stall, uint32_t stacks)
{
	pthread_mutex_lock(&s_m);
	start_locked();
	flush_dirty_locked();
	WCmd c = { W_RUN, slot, pc, stall, (uint32_t)resume, stacks };
	push_locked(c);
	if (slot >= 0 && slot < SLOTS)
		s_inflight[slot]++;
	pthread_mutex_unlock(&s_m);
}

/* the next finished run: 1 and its slot, next address (0: END), FINISH id and the stall it ran to; 0 when none */
int ge_worker_take(int *slot, uint32_t *next_pc, uint32_t *finish_id, uint32_t *stall)
{
	pthread_mutex_lock(&s_m);
	if (s_dh == s_dt) {
		pthread_mutex_unlock(&s_m);
		return 0;
	}
	const WDone d = s_d[s_dh];
	s_dh = (s_dh + 1u) % WD;
	if (d.slot >= 0 && d.slot < SLOTS)
		s_inflight[d.slot]--;
	pthread_mutex_unlock(&s_m);
	*slot = d.slot;
	*next_pc = d.next_pc;
	*finish_id = d.finish_id;
	*stall = d.stall;
	return 1;
}

/* runs posted for `slot` (< 0: any) whose end hasn't been taken */
int ge_worker_inflight(int slot)
{
	int n = 0;
	pthread_mutex_lock(&s_m);
	if (slot >= 0 && slot < SLOTS)
		n = s_inflight[slot];
	else
		for (int i = 0; i < SLOTS; i++)
			n += s_inflight[i];
	pthread_mutex_unlock(&s_m);
	return n;
}

/* (dev, DSP_HLETIME=1) the game's time waiting for the GE (a run's end, a drain, a present slot) since the last ask */
static uint64_t s_game_waited;
uint64_t ge_worker_waited_ns(void)
{
	pthread_mutex_lock(&s_m);
	const uint64_t w = s_game_waited;
	s_game_waited = 0;
	pthread_mutex_unlock(&s_m);
	return w;
}

/* until a run ends (or 2 ms: the caller looks again) */
void ge_worker_wait(void)
{
	const uint64_t w0 = wnow();
	pthread_mutex_lock(&s_m);
	if (s_dh == s_dt && (s_qh != s_qt || s_busy || s_owed)) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 2000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&s_done, &s_m, &ts);
	}
	s_game_waited += wnow() - w0;
	pthread_mutex_unlock(&s_m);
}

/* everything posted done (the runs' ends still to be taken) */
void ge_worker_drain(void)
{
	const uint64_t w0 = wnow();
	pthread_mutex_lock(&s_m);
	flush_dirty_locked();
	while (s_qh != s_qt || s_busy || s_owed)
		pthread_cond_wait(&s_done, &s_m);
	s_game_waited += wnow() - w0;
	pthread_mutex_unlock(&s_m);
	if (ge_split_drain)
		ge_split_drain();   /* the CPU's VRAM writes reached too */
}

/* a frame to show, after the lists posted before it; a present not yet started is replaced by a newer one, and the game waits
 * when it is PRESENTS_AHEAD frames ahead of the screen */
void ge_worker_present(uint32_t fb, int fmt, uint32_t stride)
{
	pthread_mutex_lock(&s_m);
	start_locked();
	flush_dirty_locked();
	const unsigned last = (s_qt + WQ - 1u) % WQ;
	if (s_qh != s_qt && s_q[last].kind == W_PRESENT) {
		s_q[last].a = fb;
		s_q[last].b = (uint32_t)fmt;
		s_q[last].c = stride;
	} else {
		const uint64_t w0 = s_presents >= PRESENTS_AHEAD ? wnow() : 0;
		while (s_presents >= PRESENTS_AHEAD)
			pthread_cond_wait(&s_done, &s_m);
		if (w0)
			s_game_waited += wnow() - w0;
		WCmd c = { W_PRESENT, -1, fb, (uint32_t)fmt, stride, 0 };
		push_locked(c);
		s_presents++;
	}
	pthread_mutex_unlock(&s_m);
}

/* the CPU wrote VRAM: kept as ranges (touching ones merged) until the next post */
void ge_worker_vram_dirty(uint32_t addr, uint32_t bytes)
{
	const uint32_t lo = addr, hi = addr + bytes;
	for (int i = 0; i < s_ndirty; i++)
		if (lo <= s_dhi[i] + 64u && s_dlo[i] <= hi + 64u) {
			if (lo < s_dlo[i]) s_dlo[i] = lo;
			if (hi > s_dhi[i]) s_dhi[i] = hi;
			return;
		}
	if (s_ndirty < DIRTY_RANGES) {
		s_dlo[s_ndirty] = lo;
		s_dhi[s_ndirty] = hi;
		s_ndirty++;
		return;
	}
	/* full: into the nearest */
	int best = 0;
	uint64_t bestgap = UINT64_MAX;
	for (int i = 0; i < s_ndirty; i++) {
		const uint64_t gap = lo > s_dhi[i] ? lo - s_dhi[i] : s_dlo[i] > hi ? s_dlo[i] - hi : 0;
		if (gap < bestgap) bestgap = gap, best = i;
	}
	if (lo < s_dlo[best]) s_dlo[best] = lo;
	if (hi > s_dhi[best]) s_dhi[best] = hi;
}

/* at exit, before the GE's renderer goes */
void ge_worker_stop(void)
{
	if (!s_started)
		return;
	pthread_mutex_lock(&s_m);
	WCmd c = { W_QUIT, -1, 0, 0, 0, 0 };
	push_locked(c);
	pthread_mutex_unlock(&s_m);
	pthread_join(s_thread, NULL);
	s_started = 0;
}
