/* The Brick's screen and pad for Nakagawa's runtime: its gui.c (the portable variant) stays, and the calls the rest of the runtime
 * makes into it are taken here (the link's --wrap): a presented PSP framebuffer goes to the screen (brickvk.cpp: the GE's GPU target,
 * or the guest's VRAM as pixels when the GE didn't draw it), the buttons come from the pad (evdev, "TRIMUI Player1"), and SIGTERM
 * quits. */
#include "recomp.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include <stdlib.h>
#include <time.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int __real_gui_on(void);

static atomic_uint s_buttons;
static atomic_int s_quit;
static uint16_t s_cpu_frame[480 * 272];

int brick_video_init(void);
void brick_video_term(void);
void brick_prof_start(void);
void ge_worker_stop(void);
void ge_worker_set_presenter(void (*present)(uint32_t fb, int fmt, uint32_t stride));
static void brick_present_now(uint32_t fbaddr, int fmt, uint32_t stride);
void brick_video_frame(const void *data, unsigned width, unsigned height, size_t pitch, int xrgb8888);

/* PSP buttons (pspctrl.h) */
enum { P_SELECT = 0x1, P_START = 0x8, P_UP = 0x10, P_RIGHT = 0x20, P_DOWN = 0x40, P_LEFT = 0x80, P_L = 0x100, P_R = 0x200,
	P_TRIANGLE = 0x1000, P_CIRCLE = 0x2000, P_CROSS = 0x4000, P_SQUARE = 0x8000 };

static int open_pad(void)
{
	DIR *dir = opendir("/sys/class/input");
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		if (strncmp(e->d_name, "event", 5) != 0)
			continue;
		char path[300], name[64] = "";
		snprintf(path, sizeof path, "/sys/class/input/%s/device/name", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		if (!fgets(name, sizeof name, f))
			name[0] = '\0';
		fclose(f);
		if (strncmp(name, "TRIMUI Player1", 14) == 0) {
			snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
			closedir(dir);
			return open(path, O_RDONLY);
		}
	}
	if (dir)
		closedir(dir);
	return -1;
}

/* The pad's inputs by the labels on the Brick (Deadstation Portable's names: its host.cpp pressedInput) and the PSP buttons each
 * gives. Deadstation says which inputs a PSP button is on (control.<key> = inputs, comma separated, or NONE: module.cfg's control
 * lines, the player's from its Controls page), at the start and at once while the game plays (brick_control_set) */
enum { IN_A, IN_B, IN_X, IN_Y, IN_L1, IN_R1, IN_L2, IN_R2, IN_SELECT, IN_START, IN_FN1, IN_FN2, IN_UP, IN_DOWN, IN_LEFT, IN_RIGHT,
	IN_COUNT };
static const char *const s_input_names[IN_COUNT] = { "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "SELECT", "START", "FN1", "FN2",
	"UP", "DOWN", "LEFT", "RIGHT" };
/* the PSP's buttons by module.cfg's keys */
static const struct { const char *key; unsigned bit; } s_psp_keys[] = {
	{ "dpad_up", P_UP }, { "dpad_down", P_DOWN }, { "dpad_left", P_LEFT }, { "dpad_right", P_RIGHT }, { "cross", P_CROSS },
	{ "circle", P_CIRCLE }, { "square", P_SQUARE }, { "triangle", P_TRIANGLE }, { "l", P_L }, { "r", P_R }, { "select", P_SELECT },
	{ "start", P_START } };
/* each input's PSP buttons: as a PSP's places, Cross at the bottom (B), Circle on the right (A), Triangle at the top (X), Square on
 * the left (Y), until Deadstation says otherwise */
static _Atomic unsigned s_input_bits[IN_COUNT] = {
	[IN_A] = P_CIRCLE, [IN_B] = P_CROSS, [IN_X] = P_TRIANGLE, [IN_Y] = P_SQUARE, [IN_L1] = P_L, [IN_R1] = P_R,
	[IN_SELECT] = P_SELECT, [IN_START] = P_START, [IN_UP] = P_UP, [IN_DOWN] = P_DOWN, [IN_LEFT] = P_LEFT, [IN_RIGHT] = P_RIGHT };

/* control.<key> = <inputs>: the PSP button off every input, then on those named; an unknown key or input said in the log */
void brick_control_set(const char *key, const char *inputs)
{
	unsigned bit = 0;
	for (size_t i = 0; i < sizeof s_psp_keys / sizeof s_psp_keys[0]; i++)
		if (strcmp(key, s_psp_keys[i].key) == 0)
			bit = s_psp_keys[i].bit;
	if (!bit) {
		fprintf(stderr, "brick: control.%s: not a PSP button: left as it is\n", key);
		return;
	}
	unsigned on = 0;	/* the inputs named (bits by IN_) */
	char list[128];
	snprintf(list, sizeof list, "%s", inputs);
	for (char *save = NULL, *name = strtok_r(list, ", ", &save); name; name = strtok_r(NULL, ", ", &save)) {
		if (strcmp(name, "NONE") == 0)
			continue;
		int found = -1;
		for (int i = 0; i < IN_COUNT; i++)
			if (strcmp(name, s_input_names[i]) == 0)
				found = i;
		if (found < 0)
			fprintf(stderr, "brick: control.%s: no input \"%s\" on the Brick: not bound\n", key, name);
		else
			on |= 1u << found;
	}
	for (int i = 0; i < IN_COUNT; i++) {
		const unsigned was = atomic_load(&s_input_bits[i]);
		atomic_store(&s_input_bits[i], (on >> i) & 1u ? was | bit : was & ~bit);
	}
	fprintf(stderr, "brick: control.%s on %s\n", key, inputs);
}

void brick_affinity(int core);
static void *pad_thread(void *arg)
{
	brick_affinity(0);
	(void)arg;
	int fd = open_pad();
	if (fd < 0) {
		fprintf(stderr, "brick: no pad (TRIMUI Player1)\n");
		return NULL;
	}
	unsigned held = 0;	/* the inputs down (bits by IN_) */
	struct input_event ev;
	while (read(fd, &ev, sizeof ev) == sizeof ev) {
		if (ev.type == EV_KEY) {
			int in = -1;
			switch (ev.code) {
			case BTN_EAST: in = IN_A; break;
			case BTN_SOUTH: in = IN_B; break;
			case BTN_WEST: in = IN_X; break;
			case BTN_NORTH: in = IN_Y; break;
			case BTN_TL: in = IN_L1; break;
			case BTN_TR: in = IN_R1; break;
			case BTN_SELECT: in = IN_SELECT; break;
			case BTN_START: in = IN_START; break;
			case BTN_THUMBL: in = IN_FN1; break;
			case BTN_THUMBR: in = IN_FN2; break;
			}
			if (in >= 0)
				held = ev.value ? held | 1u << in : held & ~(1u << in);
		} else if (ev.type == EV_ABS && ev.code == ABS_HAT0X) {
			held = (held & ~(1u << IN_LEFT | 1u << IN_RIGHT)) | (ev.value < 0 ? 1u << IN_LEFT : ev.value > 0 ? 1u << IN_RIGHT : 0u);
		} else if (ev.type == EV_ABS && ev.code == ABS_HAT0Y) {
			held = (held & ~(1u << IN_UP | 1u << IN_DOWN)) | (ev.value < 0 ? 1u << IN_UP : ev.value > 0 ? 1u << IN_DOWN : 0u);
		} else if (ev.type == EV_ABS && (ev.code == ABS_Z || ev.code == ABS_RZ)) {
			const unsigned b = 1u << (ev.code == ABS_Z ? IN_L2 : IN_R2);
			held = ev.value > 0 ? held | b : held & ~b;
		}
		unsigned buttons = 0;
		for (int i = 0; i < IN_COUNT; i++)
			if (held >> i & 1u)
				buttons |= atomic_load(&s_input_bits[i]);
		static int padlog = -1;	/* (dev) DSP_PADLOG=1: what the game gets, at each change */
		if (padlog < 0)
			padlog = getenv("DSP_PADLOG") != NULL;
		if (padlog && buttons != atomic_load(&s_buttons))
			fprintf(stderr, "brick: pad inputs 0x%04x: PSP buttons 0x%04x\n", held, buttons);
		atomic_store(&s_buttons, buttons);
	}
	return NULL;
}

static void on_term(int sig)
{
	(void)sig;
	atomic_store(&s_quit, 1);
}

/* a module's own end: the game quits as at Deadstation's quit, then the process ends with `code` (Deadstation shows "exit code N"
 * and offers its emulator core) */
static int s_exit_code;
void brick_quit_with(int code)
{
	s_exit_code = code;
	atomic_store(&s_quit, 1);
}

static int s_gpu;

/* the GE's Vulkan objects gone before the presenter's device */
static void brick_exit(void)
{
	ge_worker_stop();	/* the GE's thread first: it records into the renderer */
	if (s_gpu)
		gegpu_shutdown();
	s_gpu = 0;
	brick_video_term();
	fprintf(stderr, "brick: screen closed\n");
	if (s_exit_code) {
		fprintf(stderr, "brick: exit code %d\n", s_exit_code);
		fflush(stderr);
		_exit(s_exit_code);
	}
}

static void brick_init(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	brick_affinity(2);	/* the game's thread (affinity.c) */
	brick_prof_start();
	signal(SIGTERM, on_term);
	pthread_t t;
	pthread_create(&t, NULL, pad_thread, NULL);
	pthread_detach(t);
	if (!brick_video_init()) {
		fprintf(stderr, "brick: no screen (Vulkan, fb0)\n");
		return;
	}
	/* the GE drawn by the GPU (ge_gpu.c), unless DEADSTATION_RECOMP_SOFTWARE_GE is set (the runtime's own rasterizer) */
	const char *soft = getenv("DEADSTATION_RECOMP_SOFTWARE_GE");
	s_gpu = !(soft && soft[0] && strcmp(soft, "0") != 0) && gegpu_init();
	ge_worker_set_presenter(brick_present_now);
	atexit(brick_exit);
	fprintf(stderr, "brick: screen ready, the GE on the %s\n", s_gpu ? "GPU" : "CPU");
}

int __wrap_gui_on(void)
{
	brick_init();
	return 1;
}

/* a PSP framebuffer word as RGB565 */
static inline uint16_t psp_to_565(uint32_t fb, int fmt, uint32_t stride, int x, int y)
{
	uint32_t r, g, b;
	if (fmt == 3) {
		uint32_t p = sr_r32(fb + (y * stride + x) * 4u);
		r = (p & 0xff) >> 3; g = ((p >> 8) & 0xff) >> 2; b = ((p >> 16) & 0xff) >> 3;
	} else {
		uint32_t p = sr_r16(fb + (y * stride + x) * 2u);
		if (fmt == 0) return (uint16_t)(((p & 31) << 11) | (p & (63 << 5)) | ((p >> 11) & 31));
		if (fmt == 1) { r = p & 31; g = ((p >> 5) & 31) << 1; b = (p >> 10) & 31; }
		else { r = (p & 15) << 1; g = ((p >> 4) & 15) << 2; b = ((p >> 8) & 15) << 1; }
	}
	return (uint16_t)((r << 11) | (g << 5) | b);
}

/* frames shown each second, in the log every 5 s: how many the GE drew, how many came as pixels */
static void count_present(int gpu)
{
	static uint64_t from;
	static unsigned n_gpu, n_cpu;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	if (from == 0)
		from = now;
	if (gpu) n_gpu++; else n_cpu++;
	if (now - from >= 5000000000ull) {
		const double secs = (double)(now - from) * 1e-9;
		fprintf(stderr, "brick: %.1f frames/s shown (GE %.1f, pixels %.1f)\n", (n_gpu + n_cpu) / secs, n_gpu / secs, n_cpu / secs);
		from = now;
		n_gpu = n_cpu = 0;
	}
}

int ge_worker_on(void);
void ge_worker_present(uint32_t fb, int fmt, uint32_t stride);
void ge_worker_set_presenter(void (*present)(uint32_t fb, int fmt, uint32_t stride));
void ge_worker_stop(void);

/* a frame to the screen: on the GE worker's thread (runtime/ge_worker.c), after the lists before it */
static void brick_present_now(uint32_t fbaddr, int fmt, uint32_t stride)
{
	if (s_gpu) {
		const int res = gegpu_present(fbaddr, fmt, stride);
		if (res == GEGPU_PRESENT_SHOWN) {
			count_present(1);
			return;
		}
		if (res == GEGPU_PRESENT_TARGET_FAILED) {
			GeGpuFbDescriptor d = { fbaddr, (uint32_t)fmt, stride, 480, 272 };
			if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED)
				return;
		}
	}
	for (int y = 0; y < 272; y++)
		for (int x = 0; x < 480; x++)
			s_cpu_frame[y * 480 + x] = psp_to_565(fbaddr, fmt, stride, x, y);
	brick_video_frame(s_cpu_frame, 480, 272, 480 * 2, 0);
	count_present(0);
}

void brick_host_frame(void);
int brick_host_quit(void);
/* the module's own (src/), optional: once a frame on the game's thread, before the frame goes to the screen */
__attribute__((weak)) void sdk_module_frame(void);

/* (dev) DSP_HLETIME=1: the game's frames as its thread hands them out: every 5 s, how many, how many later than 18 and 25 ms after the
 * one before, the longest, and the time spent handing them to the GE worker (it waits when the screen is behind) */
uint64_t ge_worker_waited_ns(void);
void hle_dev_frame_end(uint64_t frame_ns);
void brick_prof_frame(uint64_t frame_ns);
static void frame_timing(uint64_t handoff_ns)
{
	static int on = -1;
	static uint64_t last, from, longest, handoff;
	static unsigned n, late18, late25, late_ge;
	if (on < 0) on = getenv("DSP_HLETIME") != NULL;
	if (!on) return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
	if (last) {
		const uint64_t d = now - last;
		n++;
		late18 += d > 18000000ull;
		late25 += d > 25000000ull;
		/* a late frame the game spent more than 3 ms of waiting for the GE in: the GE's; else the game's own */
		const uint64_t w = ge_worker_waited_ns();
		late_ge += d > 18000000ull && w > 3000000ull;
		hle_dev_frame_end(d);
		brick_prof_frame(d);
		if (d > longest) longest = d;
	}
	last = now;
	handoff += handoff_ns;
	if (!from) from = now;
	if (now - from >= 5000000000ull) {
		fprintf(stderr, "FRAMES 5 s: %u frames, %u later than 18 ms (%u waiting for the GE), %u than 25 ms, longest %.1f ms; handing them out %.0f ms\n",
				n, late18, late_ge, late25, (double)longest * 1e-6, (double)handoff * 1e-6);
		from = now;
		n = late18 = late25 = late_ge = 0;
		longest = handoff = 0;
	}
}

int __wrap_gui_present(uint32_t fbaddr, int fmt, uint32_t stride)
{
	brick_init();
	if (sdk_module_frame)
		sdk_module_frame();
	brick_host_frame();	/* hosted: ready after the first frame, held here while Deadstation's menu is up */
	struct timespec h0, h1;
	clock_gettime(CLOCK_MONOTONIC, &h0);
	if (ge_worker_on())
		ge_worker_present(fbaddr, fmt, stride);
	else
		brick_present_now(fbaddr, fmt, stride);
	clock_gettime(CLOCK_MONOTONIC, &h1);
	frame_timing((uint64_t)(h1.tv_sec - h0.tv_sec) * 1000000000ull + (uint64_t)(h1.tv_nsec - h0.tv_nsec));
	return 1;
}

uint32_t __wrap_gui_buttons(void) { return atomic_load(&s_buttons); }
void __wrap_gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = 128; if (ly) *ly = 128; }
int __wrap_gui_pad_present(void) { return 1; }
int __wrap_gui_quit_requested(void) { return atomic_load(&s_quit) || brick_host_quit(); }
int __wrap_gui_stop_requested(void) { return atomic_load(&s_quit) || brick_host_quit(); }
