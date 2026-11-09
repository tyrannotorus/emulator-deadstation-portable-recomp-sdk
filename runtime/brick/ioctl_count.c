/* (dev) DSP_IOCTLCOUNT=1: every ioctl the process makes (the GPU driver's kernel calls among them) counted by request and, for the
 * PowerVR services bridge (request 0xc0206440), by bridge and function id, with the time spent; the 12 heaviest said every 4 s.
 * The executable exports ioctl (cmake: -Wl,--export-dynamic-symbol=ioctl), so the driver's calls come here; the real one is the
 * next in the lookup order. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SLOTS 256
static struct {
	uint64_t key, calls, ns;
	uint32_t tid;
} s_slot[SLOTS];
int ge_worker_is_self(void) __attribute__((weak));
static int s_on = -1;
static uint64_t s_since;

static uint64_t now_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void note(uint64_t key, uint32_t tid, uint64_t ns)
{
	uint32_t h = (uint32_t)(((key ^ tid) * 0x9E3779B97F4A7C15ull) >> 56);
	for (int probe = 0; probe < SLOTS; probe++, h = (h + 1u) & (SLOTS - 1u)) {
		if ((s_slot[h].key == key && s_slot[h].tid == tid) || s_slot[h].calls == 0) {
			s_slot[h].key = key;
			s_slot[h].tid = tid;
			s_slot[h].calls++;
			s_slot[h].ns += ns;
			return;
		}
	}
}

static void say(uint64_t t)
{
	if (!s_since) {
		s_since = t;
		return;
	}
	if (t - s_since < 4000000000ull)
		return;
	const double secs = (double)(t - s_since) * 1e-9;
	char line[2048];
	int o = snprintf(line, sizeof(line), "IOCTLCOUNT a second:");
	for (int r = 0; r < 12; r++) {
		int best = -1;
		for (int i = 0; i < SLOTS; i++)
			if (s_slot[i].calls && (best < 0 || s_slot[i].ns > s_slot[best].ns))
				best = i;
		if (best < 0)
			break;
		o += snprintf(line + o, sizeof(line) - (size_t)o, " [%s %08llx %llx: %.0f calls, %.1f ms]",
					  s_slot[best].tid == 1u ? "ge worker" : "other",
					  (unsigned long long)(s_slot[best].key >> 32), (unsigned long long)(s_slot[best].key & 0xFFFFFFFFu),
					  (double)s_slot[best].calls / secs, (double)s_slot[best].ns * 1e-6 / secs);
		s_slot[best].calls = 0;
	}
	fprintf(stderr, "%s\n", line);
	memset(s_slot, 0, sizeof(s_slot));
	s_since = t;
}

int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	static int (*real)(int, unsigned long, void *);
	if (!real)
		real = (int (*)(int, unsigned long, void *))dlsym(RTLD_NEXT, "ioctl");
	if (s_on < 0) {
		const char *e = getenv("DSP_IOCTLCOUNT");
		s_on = e && e[0] == '1';
	}
	if (!s_on)
		return real(fd, request, arg);
	const uint64_t t0 = now_ns();
	const int r = real(fd, request, arg);
	const uint64_t t1 = now_ns();
	uint64_t key = (uint64_t)(uint32_t)request << 32;
	if ((uint32_t)request == 0xc0206440u && arg) {
		const uint32_t *b = (const uint32_t *)arg;   /* the bridge package: its bridge id, its function id */
		key |= ((uint64_t)(b[0] & 0xFFFFu) << 16) | (b[1] & 0xFFFFu);
	}
	/* the first return address up the frame chain that is the executable's own (non-PIE: below 0x10000000): which of our
	 * calls made the driver call the kernel (the driver keeps frame pointers) */
	uintptr_t site = 0;
	void **fp = (void **)__builtin_frame_address(0);
	for (int d = 0; d < 16 && fp; d++) {
		void **next = (void **)fp[0];
		const uintptr_t ret = (uintptr_t)fp[1];
		if (d > 0 && ret >= 0x400000u && ret < 0x10000000u) {
			site = ret;
			break;
		}
		if (next <= fp || (uintptr_t)next - (uintptr_t)fp > (1u << 20))
			break;
		fp = next;
	}
	note(key ^ ((uint64_t)site << 8), ge_worker_is_self && ge_worker_is_self() ? 1u : 0u, t1 - t0);
	if (site) {
		static uintptr_t sites[64];
		static uint64_t keys[64];
		static int nsites;
		int known = 0;
		for (int k = 0; k < nsites; k++)
			known |= sites[k] == site && keys[k] == key;
		if (!known && nsites < 64) {
			sites[nsites] = site;
			keys[nsites++] = key;
			fprintf(stderr, "IOCTLSITE %llx from 0x%llx\n", (unsigned long long)key, (unsigned long long)site);
		}
	}
	say(t1);
	return r;
}
