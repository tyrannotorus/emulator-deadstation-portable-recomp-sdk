/* The SDL3 calls Nakagawa's runtime makes outside its presenter (sched.c's clock, locks, conditions and threads; the OSK overlay's
 * software renderer), on the Brick, which has no SDL3: pthreads and the monotonic clock, with SDL3's meaning (recursive mutexes, a
 * nanosecond clock from the program's start). The overlay's renderer is absent: its calls fail, and the overlay isn't drawn. */
#include <SDL3/SDL.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t nowNs(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t s_start;
__attribute__((constructor)) static void shim_start(void) { s_start = nowNs(); }

Uint64 SDL_GetTicksNS(void) { return nowNs() - s_start; }

void SDL_DelayNS(Uint64 ns)
{
	struct timespec t = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
	while (nanosleep(&t, &t) != 0 && errno == EINTR) { }
}

/* a sleep to an absolute deadline on the monotonic clock (Linux's high-resolution timers: tens of microseconds), not a spin: the
 * runtime waits here for its vblanks and for the GE, and a spin there was a third of the game thread's CPU time, heat on a
 * passively cooled device */
void SDL_DelayPrecise(Uint64 ns)
{
	const uint64_t until = nowNs() + ns;
	struct timespec t = { (time_t)(until / 1000000000u), (long)(until % 1000000000u) };
	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) { }
}

struct SDL_Mutex { pthread_mutex_t m; };
struct SDL_Condition { pthread_cond_t c; };
struct SDL_Thread { pthread_t t; SDL_ThreadFunction fn; void *data; int status; };

SDL_Mutex *SDL_CreateMutex(void)
{
	SDL_Mutex *m = malloc(sizeof *m);
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->m, &a);
	pthread_mutexattr_destroy(&a);
	return m;
}
void SDL_LockMutex(SDL_Mutex *mutex) { if (mutex) pthread_mutex_lock(&mutex->m); }
void SDL_UnlockMutex(SDL_Mutex *mutex) { if (mutex) pthread_mutex_unlock(&mutex->m); }
void SDL_DestroyMutex(SDL_Mutex *mutex)
{
	if (mutex) {
		pthread_mutex_destroy(&mutex->m);
		free(mutex);
	}
}

SDL_Condition *SDL_CreateCondition(void)
{
	SDL_Condition *c = malloc(sizeof *c);
	pthread_condattr_t a;
	pthread_condattr_init(&a);
	pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
	pthread_cond_init(&c->c, &a);
	pthread_condattr_destroy(&a);
	return c;
}
void SDL_SignalCondition(SDL_Condition *cond) { if (cond) pthread_cond_signal(&cond->c); }
void SDL_BroadcastCondition(SDL_Condition *cond) { if (cond) pthread_cond_broadcast(&cond->c); }
void SDL_DestroyCondition(SDL_Condition *cond)
{
	if (cond) {
		pthread_cond_destroy(&cond->c);
		free(cond);
	}
}
void SDL_WaitCondition(SDL_Condition *cond, SDL_Mutex *mutex) { pthread_cond_wait(&cond->c, &mutex->m); }
bool SDL_WaitConditionTimeout(SDL_Condition *cond, SDL_Mutex *mutex, Sint32 timeoutMS)
{
	if (timeoutMS < 0) {
		pthread_cond_wait(&cond->c, &mutex->m);
		return true;
	}
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	t.tv_sec += timeoutMS / 1000;
	t.tv_nsec += (long)(timeoutMS % 1000) * 1000000L;
	if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
	return pthread_cond_timedwait(&cond->c, &mutex->m, &t) == 0;
}

void brick_affinity(int core);
static void *thread_main(void *p)
{
	brick_affinity(0);
	SDL_Thread *t = p;
	t->status = t->fn(t->data);
	return NULL;
}
SDL_Thread *SDL_CreateThreadRuntime(SDL_ThreadFunction fn, const char *name, void *data, SDL_FunctionPointer begin, SDL_FunctionPointer end)
{
	(void)begin; (void)end;
	SDL_Thread *t = malloc(sizeof *t);
	t->fn = fn;
	t->data = data;
	if (pthread_create(&t->t, NULL, thread_main, t) != 0) {
		free(t);
		return NULL;
	}
	if (name)
		pthread_setname_np(t->t, name);
	return t;
}
void SDL_DetachThread(SDL_Thread *thread) { if (thread) pthread_detach(thread->t); }
void SDL_WaitThread(SDL_Thread *thread, int *status)
{
	if (!thread)
		return;
	pthread_join(thread->t, NULL);
	if (status)
		*status = thread->status;
	free(thread);
}

const char *SDL_GetCurrentVideoDriver(void) { return NULL; }

static char s_error[256];
const char *SDL_GetError(void) { return s_error; }
bool SDL_SetError(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s_error, sizeof s_error, fmt, ap);
	va_end(ap);
	return false;
}

SDL_Renderer *SDL_CreateSoftwareRenderer(SDL_Surface *surface) { (void)surface; SDL_SetError("no renderer on the Brick"); return NULL; }
SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch)
{
	(void)width; (void)height; (void)format; (void)pixels; (void)pitch;
	SDL_SetError("no surfaces on the Brick");
	return NULL;
}
void SDL_DestroyRenderer(SDL_Renderer *renderer) { (void)renderer; }
void SDL_DestroySurface(SDL_Surface *surface) { (void)surface; }
bool SDL_RenderDebugText(SDL_Renderer *r, float x, float y, const char *str) { (void)r; (void)x; (void)y; (void)str; return false; }
bool SDL_RenderFillRect(SDL_Renderer *r, const SDL_FRect *rect) { (void)r; (void)rect; return false; }
bool SDL_RenderPresent(SDL_Renderer *r) { (void)r; return false; }
bool SDL_RenderRect(SDL_Renderer *r, const SDL_FRect *rect) { (void)r; (void)rect; return false; }
bool SDL_SetRenderDrawBlendMode(SDL_Renderer *r, SDL_BlendMode m) { (void)r; (void)m; return false; }
bool SDL_SetRenderDrawColor(SDL_Renderer *r, Uint8 cr, Uint8 g, Uint8 b, Uint8 a) { (void)r; (void)cr; (void)g; (void)b; (void)a; return false; }
