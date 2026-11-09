/* No sound: the headless backend in place of Nakagawa's audio_sdl3.c (the same entry points). The runtime then paces by its clock. */
#include <stdint.h>

int sr_audio_init(void) { return -1; }
void sr_audio_push(int ch, const int16_t *lr, int nframes, int volL, int volR)
{
	(void)ch; (void)lr; (void)nframes; (void)volL; (void)volR;
}
int sr_audio_queued(int ch) { (void)ch; return -1; }
void sr_audio_pause_enter(void) {}
void sr_audio_pause_leave(void) {}
void sr_audio_dump_stats(void) {}
void sr_audio_dump_finish(void) {}
int sr_audio_is_active(void) { return 0; }
int sr_audio_pause_script_ready(void) { return 1; }
