/* A recomp module's link to Deadstation Portable, which hosts it (the shared UI's host.cpp): Deadstation owns the MENU button, the
 * menu, Options and the store; the module plays the game and does as it is told. Two pipes, DEADUI_HOST=<read fd>,<write fd>,
 * lines of text:
 *   from Deadstation: pause, resume, set <key> <value>, quit
 *   to Deadstation:   ready (the first frame shown), paused (nothing more of this process reaches the screen)
 * Paused, the game's thread waits at its next frame (gui_brick.c brick_host_frame), the GE worker drained and the sound held: the
 * screen is Deadstation's until resume.
 *
 * Deadstation starts a module as <exec> <disc> [--set key=value]...; this makes Nakagawa's driver arguments from it (main is
 * wrapped): the disc for PSP_ISO, the game's memory image and entry from module.cfg beside the executable (image, entry), and the
 * working folder the module's data folder ($DEADSTATION_PORTABLE_HOME/recomp-data/<id>: its memory stick, its caches). Run any
 * other way (the SDK's dev runs: --image ...), main is Nakagawa's, untouched, and nothing here is active. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_main(int argc, char **argv);
void sr_audio_pause_enter(void);
void sr_audio_pause_leave(void);
void ge_worker_drain(void);
int sdk_build_image(const char *disc, const char *out_path, uint32_t base, char *err, size_t errn);
int sdk_file_sha256(const char *path, char hex[65]);

/* the module's own (src/), each optional: a setting at its value (at start and when Deadstation changes one) */
__attribute__((weak)) void deadstation_module_setting(const char *key, const char *value);

static int s_in = -1, s_out = -1;
static atomic_int s_pause_wanted, s_quit_wanted;
static int s_ready_said;
static pthread_mutex_t s_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cv = PTHREAD_COND_INITIALIZER;

int brick_host_quit(void) { return atomic_load(&s_quit_wanted); }

static void say(const char *line)
{
	if (s_out < 0)
		return;
	char buf[64];
	const int n = snprintf(buf, sizeof(buf), "%s\n", line);
	if (n > 0 && write(s_out, buf, (size_t)n) < 0)
		fprintf(stderr, "host link: Deadstation can't be told \"%s\" (%s)\n", line, strerror(errno));
}

void brick_control_set(const char *key, const char *inputs);	/* gui_brick.c */
static void setting(const char *key, const char *value)
{
	fprintf(stderr, "host link: %s = %s\n", key, value);
	if (strncmp(key, "control.", 8) == 0) {	/* a PSP button's inputs (module.cfg's control lines, Deadstation's Controls page) */
		brick_control_set(key + 8, value);
		return;
	}
	if (deadstation_module_setting)
		deadstation_module_setting(key, value);
}

void brick_affinity(int core);
static void *reader(void *arg)
{
	brick_affinity(0);
	(void)arg;
	char line[512];
	size_t have = 0;
	for (;;) {
		const ssize_t n = read(s_in, line + have, sizeof(line) - 1 - have);
		if (n <= 0) {
			fprintf(stderr, "host link: Deadstation is gone: quitting\n");
			atomic_store(&s_quit_wanted, 1);
			pthread_mutex_lock(&s_m);
			pthread_cond_broadcast(&s_cv);
			pthread_mutex_unlock(&s_m);
			return NULL;
		}
		have += (size_t)n;
		line[have] = '\0';
		char *start = line, *nl;
		while ((nl = strchr(start, '\n')) != NULL) {
			*nl = '\0';
			if (strcmp(start, "pause") == 0)
				atomic_store(&s_pause_wanted, 1);
			else if (strcmp(start, "resume") == 0) {
				pthread_mutex_lock(&s_m);
				atomic_store(&s_pause_wanted, 0);
				pthread_cond_broadcast(&s_cv);
				pthread_mutex_unlock(&s_m);
			} else if (strcmp(start, "quit") == 0) {
				pthread_mutex_lock(&s_m);
				atomic_store(&s_quit_wanted, 1);
				atomic_store(&s_pause_wanted, 0);
				pthread_cond_broadcast(&s_cv);
				pthread_mutex_unlock(&s_m);
			} else if (strncmp(start, "set ", 4) == 0) {
				char *key = start + 4, *value = strchr(key, ' ');
				if (value != NULL) {
					*value++ = '\0';
					setting(key, value);
				}
			} else if (start[0] != '\0')
				fprintf(stderr, "host link: \"%s\" from Deadstation isn't known\n", start);
			start = nl + 1;
		}
		have = strlen(start);
		memmove(line, start, have);
		if (have >= sizeof(line) - 1)
			have = 0;
	}
}

/* each frame, on the game's thread (gui_brick.c): the first one says ready; a pause holds the game here until resume */
void brick_host_frame(void)
{
	if (s_out < 0)
		return;
	if (!s_ready_said) {
		s_ready_said = 1;
		say("ready");
	}
	if (!atomic_load(&s_pause_wanted))
		return;
	ge_worker_drain();
	sr_audio_pause_enter();
	say("paused");
	pthread_mutex_lock(&s_m);
	while (atomic_load(&s_pause_wanted) && !atomic_load(&s_quit_wanted))
		pthread_cond_wait(&s_cv, &s_m);
	pthread_mutex_unlock(&s_m);
	sr_audio_pause_leave();
}

/* module.cfg beside the executable: a key's value, or "" */
static void cfg_value(const char *dir, const char *key, char *out, size_t size)
{
	out[0] = '\0';
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/module.cfg", dir);
	FILE *f = fopen(path, "r");
	if (f == NULL)
		return;
	char line[512];
	const size_t klen = strlen(key);
	while (fgets(line, sizeof(line), f) != NULL) {
		if (strncmp(line, key, klen) != 0)
			continue;
		const char *p = line + klen;
		while (*p == ' ' || *p == '\t') p++;
		if (*p != '=')
			continue;
		p++;
		while (*p == ' ' || *p == '\t') p++;
		snprintf(out, size, "%s", p);
		out[strcspn(out, "\r\n")] = '\0';
		break;
	}
	fclose(f);
}

int __wrap_main(int argc, char **argv)
{
	/* not started by Deadstation (a dev run with Nakagawa's own arguments): its main as it is */
	if (argc < 2 || strncmp(argv[1], "--", 2) == 0)
		return __real_main(argc, argv);

	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", argv[0]);
	char *slash = strrchr(dir, '/');
	if (slash != NULL)
		*slash = '\0';
	else
		snprintf(dir, sizeof(dir), ".");
	char id[128], image[256], entry[64];
	cfg_value(dir, "id", id, sizeof(id));
	cfg_value(dir, "image", image, sizeof(image));
	cfg_value(dir, "entry", entry, sizeof(entry));
	if (id[0] == '\0' || image[0] == '\0' || entry[0] == '\0') {
		fprintf(stderr, "host link: module.cfg beside %s names no id, image or entry\n", argv[0]);
		return 2;
	}

	/* the disc, the settings */
	setenv("PSP_ISO", argv[1], 1);
	for (int i = 2; i + 1 < argc; i += 2) {
		if (strcmp(argv[i], "--set") != 0)
			continue;
		char kv[512];
		snprintf(kv, sizeof(kv), "%s", argv[i + 1]);
		char *eq = strchr(kv, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		setting(kv, eq + 1);
	}

	/* the module's data folder: its working folder (the memory stick, the caches) */
	const char *home = getenv("DEADSTATION_PORTABLE_HOME");
	if (home != NULL && home[0] != '\0') {
		char data[PATH_MAX];
		snprintf(data, sizeof(data), "%s/recomp-data", home);
		mkdir(data, 0755);
		snprintf(data, sizeof(data), "%s/recomp-data/%s", home, id);
		mkdir(data, 0755);
		if (chdir(data) != 0)
			fprintf(stderr, "host link: %s can't be the working folder (%s)\n", data, strerror(errno));
	}

	/* the link */
	const char *fds = getenv("DEADUI_HOST");
	if (fds != NULL && sscanf(fds, "%d,%d", &s_in, &s_out) == 2) {
		pthread_t t;
		pthread_create(&t, NULL, reader, NULL);
		pthread_detach(t);
	} else
		s_in = s_out = -1;

	/* Nakagawa's driver: the game's memory image at 0x08804000, its entry, the scheduler. The image is made from the player's own
	 * disc into the data folder (image_build.c) and checked against module.cfg's image_sha256; a module ships no game data */
	char image_path[PATH_MAX], want_sha[80], have_sha[65] = "";
	snprintf(image_path, sizeof(image_path), "%s", image);
	cfg_value(dir, "image_sha256", want_sha, sizeof(want_sha));
	if (!sdk_file_sha256(image_path, have_sha) || (want_sha[0] && strcmp(have_sha, want_sha) != 0)) {
		char why[256] = "";
		fprintf(stderr, "host link: making %s from the disc\n", image_path);
		if (!sdk_build_image(argv[1], image_path, 0x08804000u, why, sizeof(why))) {
			fprintf(stderr, "host link: the game's image can't be made: %s\n", why);
			return 3;
		}
		if (!sdk_file_sha256(image_path, have_sha) || (want_sha[0] && strcmp(have_sha, want_sha) != 0)) {
			fprintf(stderr, "host link: the image made from this disc isn't the one the module was built for (SHA-256 %s, not %s)\n",
					have_sha, want_sha);
			remove(image_path);
			return 3;
		}
	}
	char *args[] = { argv[0], "--image", image_path, "0x8804000", entry, "none", "none", "--sched", NULL };
	fprintf(stderr, "host link: %s from %s, %s at %s\n", id, argv[1], image_path, entry);
	return __real_main(8, args);
}
