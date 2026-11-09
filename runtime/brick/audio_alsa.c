/* The PSP's sound to the Brick's speaker through ALSA ("default": NextUI's softvol and dmix), in place of Nakagawa's audio_sdl3.c
 * (the same entry points). Each of the runtime's nine outputs (sceAudio's eight channels, Output2 at 8) is a ring of 44.1 kHz stereo
 * frames, its volume applied as it is pushed (0x8000 unity); one thread mixes the rings and writes the device. sr_audio_queued is a
 * ring's fill, what the guest's blocking output paces against. Never stretched or resampled here: a ring that runs dry plays silence
 * (counted, in the log every 5 s), the rest of the mix goes on. */
#include <alsa/asoundlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHANNELS 9
#define RATE 44100
#define PERIOD 512					/* frames a write */
#define DEVICE_BUFFER (PERIOD * 4)	/* the device's buffer: about 46 ms */
#define RING (65536 * 4)			/* frames a ring holds (as audio_sdl3.c's ceiling) */

typedef struct {
	int16_t *frames;	/* RING stereo frames */
	uint64_t w, r;		/* frames written, read */
	uint64_t pushed;
} Ring;

static Ring s_rings[CHANNELS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static snd_pcm_t *s_pcm;
static pthread_t s_thread;
static atomic_int s_state;		/* 0 not started, 1 playing, -1 none */
static atomic_int s_stop;
static atomic_int s_paused;
static unsigned s_dry, s_dropped, s_xruns;	/* since the last log line */

static int16_t clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static int16_t scale(int16_t s, int vol)
{
	const int64_t v = ((int64_t)s * vol) >> 15;
	return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

void brick_affinity(int core);
static void *mixer(void *arg)
{
	brick_affinity(0);
	(void)arg;
	int32_t acc[PERIOD * 2];
	int16_t out[PERIOD * 2];
	struct timespec from;
	clock_gettime(CLOCK_MONOTONIC, &from);
	while (!atomic_load(&s_stop)) {
		memset(acc, 0, sizeof(acc));
		if (!atomic_load(&s_paused)) {
			pthread_mutex_lock(&s_lock);
			for (int ch = 0; ch < CHANNELS; ch++) {
				Ring *g = &s_rings[ch];
				if (!g->frames)
					continue;
				const uint64_t have = g->w - g->r;
				const uint32_t n = have < PERIOD ? (uint32_t)have : PERIOD;
				if (n < PERIOD && g->pushed > 0 && have > 0)
					s_dry++;
				for (uint32_t i = 0; i < n; i++) {
					const uint32_t at = (uint32_t)((g->r + i) % RING) * 2;
					acc[i * 2] += g->frames[at];
					acc[i * 2 + 1] += g->frames[at + 1];
				}
				g->r += n;
			}
			pthread_mutex_unlock(&s_lock);
		}
		for (int i = 0; i < PERIOD * 2; i++)
			out[i] = clamp16(acc[i]);
		int16_t *p = out;
		snd_pcm_sframes_t left = PERIOD;
		while (left > 0 && !atomic_load(&s_stop)) {
			const snd_pcm_sframes_t n = snd_pcm_writei(s_pcm, p, (snd_pcm_uframes_t)left);
			if (n < 0) {
				s_xruns++;
				if (snd_pcm_recover(s_pcm, (int)n, 1) < 0) {
					fprintf(stderr, "audio: the device failed (%s): no sound from now on\n", snd_strerror((int)n));
					atomic_store(&s_state, -1);
					return NULL;
				}
				continue;
			}
			p += n * 2;
			left -= n;
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - from.tv_sec >= 5) {
			if (s_dry || s_dropped || s_xruns)
				fprintf(stderr, "audio: in 5 s: %u periods a channel ran dry, %u pushes dropped (ring full), %u device underruns\n",
						s_dry, s_dropped, s_xruns);
			s_dry = s_dropped = s_xruns = 0;
			from = now;
		}
	}
	return NULL;
}

static void stop_audio(void)
{
	if (atomic_load(&s_state) != 1)
		return;
	atomic_store(&s_stop, 1);
	pthread_join(s_thread, NULL);
	snd_pcm_drop(s_pcm);
	snd_pcm_close(s_pcm);
	s_pcm = NULL;
	atomic_store(&s_state, 0);
}

int sr_audio_init(void)
{
	const int state = atomic_load(&s_state);
	if (state != 0)
		return state == 1 ? 0 : -1;
	int err = snd_pcm_open(&s_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "audio: no sound: the device can't be opened (%s)\n", snd_strerror(err));
		atomic_store(&s_state, -1);
		return -1;
	}
	err = snd_pcm_set_params(s_pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, RATE, 1,
			(unsigned)((uint64_t)DEVICE_BUFFER * 1000000u / RATE));
	if (err < 0) {
		fprintf(stderr, "audio: no sound: 44.1 kHz stereo refused (%s)\n", snd_strerror(err));
		snd_pcm_close(s_pcm);
		s_pcm = NULL;
		atomic_store(&s_state, -1);
		return -1;
	}
	snd_pcm_uframes_t buffer = 0, period = 0;
	snd_pcm_get_params(s_pcm, &buffer, &period);
	for (int ch = 0; ch < CHANNELS; ch++)
		s_rings[ch].frames = calloc(RING * 2, sizeof(int16_t));
	atomic_store(&s_stop, 0);
	if (pthread_create(&s_thread, NULL, mixer, NULL) != 0) {
		fprintf(stderr, "audio: no sound: no mixer thread\n");
		snd_pcm_close(s_pcm);
		s_pcm = NULL;
		atomic_store(&s_state, -1);
		return -1;
	}
	atomic_store(&s_state, 1);
	atexit(stop_audio);
	fprintf(stderr, "audio: ALSA default, %d Hz stereo, device buffer %lu frames, period %lu\n", RATE, (unsigned long)buffer,
			(unsigned long)period);
	return 0;
}

void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR)
{
	if (ch < 0 || ch >= CHANNELS || !lr || nframes <= 0)
		return;
	if (atomic_load(&s_state) == 0)
		sr_audio_init();
	if (atomic_load(&s_state) != 1)
		return;
	pthread_mutex_lock(&s_lock);
	Ring *g = &s_rings[ch];
	if (g->w - g->r + (uint64_t)nframes > RING) {
		s_dropped++;
		pthread_mutex_unlock(&s_lock);
		return;
	}
	const int unity = volL == 0x8000 && volR == 0x8000;
	for (int i = 0; i < nframes; i++) {
		const uint32_t at = (uint32_t)((g->w + (uint64_t)i) % RING) * 2;
		g->frames[at] = unity ? lr[i * 2] : scale(lr[i * 2], volL);
		g->frames[at + 1] = unity ? lr[i * 2 + 1] : scale(lr[i * 2 + 1], volR);
	}
	g->w += (uint64_t)nframes;
	g->pushed += (uint64_t)nframes;
	pthread_mutex_unlock(&s_lock);
}

int sr_audio_queued(int ch)
{
	if (ch < 0 || ch >= CHANNELS)
		return -1;
	if (atomic_load(&s_state) == 0)
		sr_audio_init();
	if (atomic_load(&s_state) != 1)
		return -1;
	pthread_mutex_lock(&s_lock);
	const uint64_t n = s_rings[ch].w - s_rings[ch].r;
	pthread_mutex_unlock(&s_lock);
	return (int)n;
}

/* the runtime's pause (Deadstation's menu later): the rings keep what they hold, the speaker plays silence */
void sr_audio_pause_enter(void) { atomic_store(&s_paused, 1); }
void sr_audio_pause_leave(void) { atomic_store(&s_paused, 0); }
void sr_audio_dump_stats(void) {}
void sr_audio_dump_finish(void) {}
int sr_audio_is_active(void) { return atomic_load(&s_state) == 1; }
int sr_audio_pause_script_ready(void) { return 1; }
