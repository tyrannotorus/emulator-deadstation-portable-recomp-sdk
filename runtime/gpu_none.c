/* No GPU (the headless build): no GE target lives outside guest memory, so a guest framebuffer is always already there. */
#include "gpu_sdl3vk/ge_gpu.h"

int gegpu_sync_guest_fb(const GeGpuFbDescriptor *desc)
{
	(void)desc;
	return GEGPU_SYNC_NO_TARGET;
}
