/* The SDK's own HLE handlers: PSP system calls Nakagawa's runtime doesn't have yet, registered after its own (sr_hle_init's hook,
 * patches/nakagawa/0001). Each follows PPSSPP's implementation (Deadstation Portable's core/Core/HLE), which is the reference. */
#include "recomp.h"
#include "hle_internal.h"
#include "vfs_path.h"

#include <stdio.h>
#include <time.h>
#include <stdlib.h>

/* sceHprmRegisterCallback(cbid): the headphone remote's callback. The Brick has no remote: accepted, never called (PPSSPP:
 * sceHprm.cpp, returns 0). */
static uint32_t h_HprmRegisterCallback(CpuState *s)
{
	(void)s;
	return 0;
}

static uint32_t h_HprmUnregisterCallback(CpuState *s)
{
	(void)s;
	return 0;
}

/* (dev) DSP_IOTRACE=1: each file call logged with its arguments and result, the runtime's own handler doing the work */
#define IOTRACE_CALLS(X) \
	X(0, 0x109f50bcu, "sceIoOpen") X(1, 0x89aa9906u, "sceIoOpenAsync") X(2, 0x6a638d83u, "sceIoRead") \
	X(3, 0xa0b5a7c2u, "sceIoReadAsync") X(4, 0x27eb27b8u, "sceIoLseek") X(5, 0x71b19e77u, "sceIoLseekAsync") \
	X(6, 0xe23eec33u, "sceIoWaitAsync") X(7, 0x35dbd746u, "sceIoWaitAsyncCB") X(8, 0x3251ea56u, "sceIoPollAsync") \
	X(9, 0xcb05f8d6u, "sceIoGetAsyncStat") X(10, 0x810c4bc3u, "sceIoClose") X(11, 0xff5940b6u, "sceIoCloseAsync") \
	X(12, 0x68963324u, "sceIoLseek32") X(13, 0x1b385d8fu, "sceIoLseek32Async") X(14, 0x54f5fb11u, "sceIoDevctl") \
	X(15, 0x63632449u, "sceIoIoctl") X(16, 0xe95a012bu, "sceIoIoctlAsync") X(17, 0xb293727fu, "sceIoChangeAsyncPriority")
static HleFn s_io_fn[18];
/* the last async read on each descriptor, hashed when its wait returns (FNV-1a), to compare with the file on the disc */
static uint32_t s_rd_buf[64], s_rd_size[64];
static void iotrace_read_done(uint32_t fd, int64_t result)
{
	if (fd >= 64 || !s_rd_size[fd] || result <= 0 || !sr_guest_span_readable(s_rd_buf[fd], (uint32_t)result))
		return;
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < (uint32_t)result; i++)
		h = (h ^ MEM_R8(s_rd_buf[fd] + i)) * 16777619u;
	fprintf(stderr, "IOTRACE read fd %u: %lld bytes fnv 0x%08x\n", fd, (long long)result, h);
	s_rd_size[fd] = 0;
}
#define IOTRACE_WRAP(i, nid, name) \
	static uint32_t iotrace_##i(CpuState *s) \
	{ \
		const uint32_t a0 = s->r[4], a1 = s->r[5], a2 = s->r[6], a3 = s->r[7], t0 = s->r[8], t1 = s->r[9]; \
		const uint32_t rc = s_io_fn[i](s); \
		if ((i) == 3 && a0 < 64) { s_rd_buf[a0] = a1; s_rd_size[a0] = a2; } \
		const int waits = (i) >= 6 && (i) <= 8 && a1 && sr_guest_span_readable(a1, 8u); \
		fprintf(stderr, "IOTRACE %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x) -> 0x%x [thread 0x%x]%s%llx\n", name, a0, a1, a2, a3, t0, t1, \
				rc, sched_current_uid(), waits ? " result 0x" : "", \
				waits ? (unsigned long long)MEM_R32(a1) | ((unsigned long long)MEM_R32(a1 + 4u) << 32) : 0ull); \
		if (waits && rc == 0) iotrace_read_done(a0, (int64_t)((uint64_t)MEM_R32(a1) | ((uint64_t)MEM_R32(a1 + 4u) << 32))); \
		return rc; \
	}
IOTRACE_CALLS(IOTRACE_WRAP)

/* (dev) DSP_HLETRACE=nid,nid,...: any calls (up to 32) logged as above, with the 8 argument registers and the result */
#define HT_N 32
static HleFn s_ht_fn[HT_N];
static uint32_t s_ht_nid[HT_N];
static const char *s_ht_name[HT_N];
/* (dev) DSP_HLETIME=1 with DSP_HLETRACE: no line per call; each call's wall time summed and said once a second ("HLETIME name ms in
 * the second, calls"): where the game's thread waits */
static int s_ht_time = -1;
static uint64_t s_ht_ns[HT_N], s_ht_calls[HT_N], s_ht_last;
static uint64_t s_ht_fns[HT_N], s_ht_fcalls[HT_N];   /* (dev) the same within the frame being drawn */
/* (dev, gui_brick.c at each frame handed out) a frame later than 18 ms: the traced calls' time within it ("LATE"); the counts start
 * over every frame */
void hle_dev_frame_end(uint64_t frame_ns)
{
	if (s_ht_time <= 0)
		return;
	if (frame_ns > 18000000ull) {
		char line[1024];
		int o = snprintf(line, sizeof(line), "LATE frame %.1f ms:", (double)frame_ns * 1e-6);
		for (int k = 0; k < HT_N && s_ht_name[k]; k++)
			if (s_ht_fcalls[k] && o < (int)sizeof(line) - 64)
				o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.1f ms (%llu)", s_ht_name[k], (double)s_ht_fns[k] * 1e-6,
							  (unsigned long long)s_ht_fcalls[k]);
		fprintf(stderr, "%s\n", line);
	}
	for (int k = 0; k < HT_N; k++)
		s_ht_fns[k] = s_ht_fcalls[k] = 0;
}
static uint64_t ht_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void ht_time_note(int i, uint64_t t0)
{
	const uint64_t t1 = ht_now();
	s_ht_ns[i] += t1 - t0;
	s_ht_calls[i]++;
	s_ht_fns[i] += t1 - t0;
	s_ht_fcalls[i]++;
	if (!s_ht_last)
		s_ht_last = t1;
	if (t1 - s_ht_last >= 1000000000ull) {
		char line[1024];
		int o = snprintf(line, sizeof(line), "HLETIME %.2f s:", (double)(t1 - s_ht_last) * 1e-9);
		for (int k = 0; k < HT_N && s_ht_name[k]; k++) {
			if (s_ht_calls[k] && o < (int)sizeof(line) - 64)
				o += snprintf(line + o, sizeof(line) - (size_t)o, " %s %.0f ms (%llu)", s_ht_name[k], (double)s_ht_ns[k] * 1e-6,
							  (unsigned long long)s_ht_calls[k]);
			s_ht_ns[k] = s_ht_calls[k] = 0;
		}
		fprintf(stderr, "%s\n", line);
		s_ht_last = t1;
	}
}
#define HT_WRAP(i) \
	static uint32_t hletrace_##i(CpuState *s) \
	{ \
		if (s_ht_time > 0) { \
			const uint64_t t0 = ht_now(); \
			const uint32_t rc_ = s_ht_fn[i](s); \
			ht_time_note(i, t0); \
			return rc_; \
		} \
		uint32_t a[8]; \
		for (int k = 0; k < 8; k++) a[k] = s->r[4 + k]; \
		const uint32_t rc = s_ht_fn[i](s); \
		fprintf(stderr, "HLETRACE %s(0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x, 0x%x) -> 0x%x [thread 0x%x v%u]\n", s_ht_name[i], a[0], a[1], \
				a[2], a[3], a[4], a[5], a[6], a[7], rc, sched_current_uid(), sr_audio_vbl()); \
		return rc; \
	}
HT_WRAP(0) HT_WRAP(1) HT_WRAP(2) HT_WRAP(3) HT_WRAP(4) HT_WRAP(5) HT_WRAP(6) HT_WRAP(7) HT_WRAP(8) HT_WRAP(9) HT_WRAP(10) HT_WRAP(11)
HT_WRAP(12) HT_WRAP(13) HT_WRAP(14) HT_WRAP(15) HT_WRAP(16) HT_WRAP(17) HT_WRAP(18) HT_WRAP(19) HT_WRAP(20) HT_WRAP(21) HT_WRAP(22)
HT_WRAP(23) HT_WRAP(24) HT_WRAP(25) HT_WRAP(26) HT_WRAP(27) HT_WRAP(28) HT_WRAP(29) HT_WRAP(30) HT_WRAP(31)
static HleFn const s_ht_wrap[HT_N] = { hletrace_0, hletrace_1, hletrace_2, hletrace_3, hletrace_4, hletrace_5, hletrace_6, hletrace_7,
	hletrace_8, hletrace_9, hletrace_10, hletrace_11, hletrace_12, hletrace_13, hletrace_14, hletrace_15, hletrace_16, hletrace_17,
	hletrace_18, hletrace_19, hletrace_20, hletrace_21, hletrace_22, hletrace_23, hletrace_24, hletrace_25, hletrace_26, hletrace_27,
	hletrace_28, hletrace_29, hletrace_30, hletrace_31 };

static void hletrace_install(void)
{
	const char *list = getenv("DSP_HLETRACE");
	const char *tm = getenv("DSP_HLETIME");
	s_ht_time = tm && tm[0] == '1';
	int n = 0;
	for (const char *p = list; p && *p && n < HT_N;) {
		char *end;
		const uint32_t nid = (uint32_t)strtoul(p, &end, 16);
		if (end == p)
			break;
		HleEntry *e = hle_find(nid);
		if (e && e->fn) {
			s_ht_fn[n] = e->fn;
			s_ht_nid[n] = nid;
			s_ht_name[n] = e->name;
			e->fn = s_ht_wrap[n];
			n++;
		} else
			fprintf(stderr, "HLETRACE: 0x%08x not registered\n", nid);
		p = *end == ',' ? end + 1 : end;
	}
	if (n)
		fprintf(stderr, "HLETRACE: %d calls traced\n", n);
}

static void iotrace_install(void)
{
	hletrace_install();
	const char *on = getenv("DSP_IOTRACE");
	if (!on || !on[0] || on[0] == '0')
		return;
#define IOTRACE_SET(i, nid, name) \
	{ HleEntry *e = hle_find(nid); if (e && e->fn) { s_io_fn[i] = e->fn; e->fn = iotrace_##i; } }
	IOTRACE_CALLS(IOTRACE_SET)
	fprintf(stderr, "IOTRACE: on\n");
}

/* (dev experiment) DSP_FTZ=1: the host's flush-to-zero and denormals-are-zero on for the run (new guest threads inherit it) */
static void ftz_experiment(void)
{
	const char *on = getenv("DSP_FTZ");
	if (!on || on[0] != '1')
		return;
#if defined(__x86_64__)
	uint32_t m;
	__asm__ __volatile__("stmxcsr %0" : "=m"(m));
	m |= 0x8040u;
	__asm__ __volatile__("ldmxcsr %0" : : "m"(m));
#elif defined(__aarch64__)
	uint64_t f;
	__asm__ __volatile__("mrs %0, fpcr" : "=r"(f));
	f |= 1ull << 24;
	__asm__ __volatile__("msr fpcr, %0" : : "r"(f));
#endif
	fprintf(stderr, "DSP_FTZ: flush-to-zero on\n");
}

/* sceKernelLoadModule(path, flags, option) on a PARAM.SFO: -1, as PPSSPP answers. Some games load their save's PARAM.SFO as a
 * module and go on by the error; given a module uid they read garbage. Other files go to the runtime's own handler. */
static HleFn s_load_module;

static uint32_t h_LoadModule_sdk(CpuState *s)
{
	char guest[256];
	uint32_t i = 0;
	for (; i + 1 < sizeof(guest) && A0 && sr_guest_span_readable(A0 + i, 1u); i++)
		if (!(guest[i] = (char)MEM_R8(A0 + i)))
			break;
	guest[i] = '\0';
	char host[1024];
	if (guest[0] && sr_ms0_resolve(sr_ms0_root(), guest, host, sizeof(host), '/')) {
		FILE *f = fopen(host, "rb");
		if (f) {
			uint8_t magic[4] = { 0 };
			const size_t n = fread(magic, 1, 4, f);
			fclose(f);
			if (n == 4 && magic[0] == 0 && magic[1] == 'P' && magic[2] == 'S' && magic[3] == 'F') {
				fprintf(stderr, "sceKernelLoadModule(\"%s\"): a PARAM.SFO, not a module: -1 (as PPSSPP)\n", guest);
				return 0xffffffffu;
			}
		}
	}
	return s_load_module(s);
}

/* the GE worker's finished lists are taken (their finish callbacks run) at the game's display calls too, as the
 * PSP's finish interrupt would have come by then */
void ge_async_poll(CpuState *s);
#define DISPLAY_CALLS(X) X(0, 0x289d82feu) X(1, 0x984c27e7u) X(2, 0x46f186c3u) X(3, 0x36cdfadeu) X(4, 0x8eb9ec49u)
static HleFn s_display_fn[5];
#define DISPLAY_WRAP(i, nid) \
	static uint32_t display_poll_##i(CpuState *s) \
	{ \
		ge_async_poll(s); \
		return s_display_fn[i](s); \
	}
DISPLAY_CALLS(DISPLAY_WRAP)

void sr_hle_register_sdk(void)
{
#define DISPLAY_SET(i, nid) \
	{ HleEntry *e = hle_find(nid); if (e && e->fn) { s_display_fn[i] = e->fn; e->fn = display_poll_##i; } }
	DISPLAY_CALLS(DISPLAY_SET)
	ftz_experiment();
	iotrace_install();
	HleEntry *lm = hle_find(0x977de386u);
	if (lm && lm->fn) {
		s_load_module = lm->fn;
		lm->fn = h_LoadModule_sdk;
	} else
		fprintf(stderr, "HLE SDK: sceKernelLoadModule not registered: a PARAM.SFO loaded as a module is answered by nothing\n");
	sr_hle_register(0xc7154136u, "sceHprmRegisterCallback", h_HprmRegisterCallback);
	sr_hle_register(0xfd7de6cdu, "sceHprmUnregisterCallback", h_HprmUnregisterCallback);
	sr_hle_register(0x444ed0b7u, "sceHprmUnregitserCallback", h_HprmUnregisterCallback);	/* the firmware's own typo (PPSSPP) */
}
