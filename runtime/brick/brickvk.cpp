// The Brick's Vulkan for Nakagawa's GE (gpu_sdl3vk/ge_gpu.c): its three calls into an SDL3 window (sdl3vk.h) answered by the
// presenter (video.h, panel.h: fb0's pages, no window). The device is the presenter's; a GE frame is copied into an image of the
// presenter's own (the GE's target stays in the layout it left it in) and drawn onto the next page, the PSP's picture letterboxed
// until the game draws 4:3 itself. A frame the GE didn't draw (gegpu_present: not resident) comes as pixels (brick_video_frame).
#include "video.h"
#include "log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "gpu_sdl3vk/sdl3vk.h"
}

namespace
{

Video video;
bool started = false;

// the copy of the GE's last frame, sampled by the page's draw
struct Copy
{
	vk::UniqueImage image;
	vk::UniqueDeviceMemory memory;
	vk::UniqueImageView view;
	uint32_t width = 0, height = 0;
	bool shaderReady = false;
	int page = -1;	// the panel page the frame that last read this copy went to
};
Copy copies[2];
unsigned nextCopy = 0;
vk::UniqueCommandPool pool;
vk::UniqueCommandBuffer commands[2];
vk::Fence lastFence;

uint32_t memoryType(vk::PhysicalDevice pd, uint32_t bits, vk::MemoryPropertyFlags flags)
{
	const auto props = pd.getMemoryProperties();
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
			return i;
	return UINT32_MAX;
}

bool makeCopy(Copy& c, uint32_t w, uint32_t h)
{
	const Video::Device d = video.gpuDevice();
	c.view.reset();
	c.image.reset();
	c.memory.reset();
	c.image = d.device.createImageUnique(vk::ImageCreateInfo(vk::ImageCreateFlags(), vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm,
			vk::Extent3D(w, h, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal,
			vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled));
	const vk::MemoryRequirements req = d.device.getImageMemoryRequirements(*c.image);
	const uint32_t type = memoryType(d.physicalDevice, req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
	if (type == UINT32_MAX)
		return false;
	c.memory = d.device.allocateMemoryUnique(vk::MemoryAllocateInfo(req.size, type));
	d.device.bindImageMemory(*c.image, *c.memory, 0);
	c.view = d.device.createImageViewUnique(vk::ImageViewCreateInfo(vk::ImageViewCreateFlags(), *c.image, vk::ImageViewType::e2D,
			vk::Format::eR8G8B8A8Unorm, vk::ComponentMapping(), vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	c.width = w, c.height = h;
	c.shaderReady = false;
	return true;
}

// the PSP's 480 x 272 (or that times the GE's scale) letterboxed on the page
void letterbox(uint32_t w, uint32_t h)
{
	const uint32_t pw = 1024, ph = 768;
	uint32_t dw = pw, dh = (uint32_t)((uint64_t)pw * h / w);
	if (dh > ph)
		dh = ph, dw = (uint32_t)((uint64_t)ph * w / h);
	video.setPicture((pw - dw) / 2, (ph - dh) / 2, dw, dh);
}

}	// namespace

extern "C" int brick_video_init(void)
{
	if (started)
		return video.ready();
	started = true;
	std::string error;
	if (!video.init(true, true, error, 1024, 512))
	{
		DS_LOG("brick video: %s", error.c_str());
		return 0;
	}
	// the panel as near the PSP's rate as its lines allow (788 a frame, about 59.93 Hz); the game keeps its own clock (59.94), and
	// when two frames already wait the next is given up rather than holding the game. Restored at exit (Panel::term), and by
	// Deadstation Portable's launch.sh should the module crash. DSP_PANEL_LINES (dev): another frame length.
	{
		const char *e = getenv("DSP_PANEL_LINES");
		const uint32_t lines = e ? (uint32_t)atoi(e) : 788u;
		video.panel().matchRate(lines >= 784u && lines <= 790u ? lines : 788u);
	}
	// the screen's submissions on their own thread (the driver's kernel calls off the GE's); the GE's submissions drain these
	// first (brick_submit_drain), so the queue keeps their order
	video.setAsyncSubmit(true);
	const Video::Device d = video.gpuDevice();
	{
		// what the GE's renderer may use on this GPU, once in the log
		const vk::PhysicalDeviceFeatures f = d.physicalDevice.getFeatures();
		const auto ds = [&](vk::Format fmt) {
			return (bool)(d.physicalDevice.getFormatProperties(fmt).optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment);
		};
		DS_LOG("brick video: %s; dualSrcBlend %d, logicOp %d, D16S8 %d, D24S8 %d, D32S8 %d, S8 %d",
				d.physicalDevice.getProperties().deviceName.data(), f.dualSrcBlend, f.logicOp, ds(vk::Format::eD16UnormS8Uint),
				ds(vk::Format::eD24UnormS8Uint), ds(vk::Format::eD32SfloatS8Uint), ds(vk::Format::eS8Uint));
		std::string exts;
		for (const vk::ExtensionProperties& e : d.physicalDevice.enumerateDeviceExtensionProperties())
			exts += std::string(" ") + e.extensionName.data();
		DS_LOG("brick video: device extensions:%s", exts.c_str());
	}
	pool = d.device.createCommandPoolUnique(vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, d.queueFamily));
	auto cmds = d.device.allocateCommandBuffersUnique(vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 2));
	commands[0] = std::move(cmds[0]);
	commands[1] = std::move(cmds[1]);
	return 1;
}

// at exit, after the GE has let go of the device (gegpu_shutdown): ours, then the presenter (left to the static destructors, the
// device went with the GE's objects still in it and exit hung)
// the GE's submissions on the screen's submit thread, in order with the screen's own (vkQueueSubmit's driver work off the GE's)
extern "C" void brick_submit_async(VkCommandBuffer cmd, VkFence fence)
{
	const vk::CommandBuffer c(cmd);
	video.submit(&c, 1, vk::Fence(fence), Video::SUBMIT_GE);
}

// the GE's submissions the submit thread has made, in order: a fence is polled only once its submission
// is made (the renderer frees its T&L ring from the fences)
extern "C" uint64_t brick_ge_submitted(void)
{
	return video.geSubmittedCount();
}

extern "C" void brick_submit_drain(void)
{
	if (video.ready())
		video.submitDrain();
}

extern "C" void brick_video_term(void)
{
	if (!video.ready())
		return;
	video.submitDrain();
	video.gpuDevice().device.waitIdle();
	lastFence = nullptr;
	commands[0].reset();
	commands[1].reset();
	pool.reset();
	for (Copy& c : copies)
		c = Copy();
	video.term();
}

// a frame the GE didn't draw: pixels (RGB565 or XRGB8888)
extern "C" void brick_video_frame(const void *data, unsigned width, unsigned height, size_t pitch, int xrgb8888)
{
	if (!video.ready())
		return;
	letterbox(width, height);
	video.frame(data, width, height, pitch, xrgb8888 ? Video::Format::XRGB8888 : Video::Format::RGB565);
}

// the device has dual-source blending on (patches/nakagawa/0005)
extern "C" int brick_dual_src_blend(void)
{
	return video.ready() && video.dualSrcBlend();
}

// the device has depth clamp on (patches/nakagawa/0010)
extern "C" int brick_depth_clamp(void)
{
	static const bool off = getenv("DSP_NO_DEPTHCLAMP") != nullptr;	// (dev) for comparing its GPU cost
	return !off && video.ready() && video.depthClampOn();
}

// the device has raster-order colour attachment access on (patches/nakagawa/0009)
extern "C" int brick_raster_order(void)
{
	return video.ready() && video.rasterOrderAccess();
}

extern "C" int sdl3vk_get_vk(Sdl3VkInfo *out)
{
	if (!video.ready() || out == nullptr)
		return 0;
	const Video::Device d = video.gpuDevice();
	std::memset(out, 0, sizeof(*out));
	out->instance = (void *)(VkInstance)video.vkInstance();
	out->physical = (void *)(VkPhysicalDevice)d.physicalDevice;
	out->device = (void *)(VkDevice)d.device;
	out->queue = (void *)(VkQueue)d.queue;
	out->queue_family = d.queueFamily;
	out->denorm_preserve_f32 = 0;
	out->get_instance_proc_addr = (void *)video.getInstanceProcAddr();
	return 1;
}

// the GE's frame: its image (in TRANSFER_SRC_OPTIMAL, its writes submitted before this), srcw x srch of it shown
extern "C" int sdl3vk_present_image_ex(void *vk_image, int srcw, int srch)
{
	if (!video.ready() || vk_image == nullptr || srcw <= 0 || srch <= 0)
		return 0;
	// two frames already waiting for the panel (it runs a hair slower than the game): this one given up rather than the renderer,
	// and so the game, held for it
	video.submitDrain();
	if (video.panel().waiting() >= 2) {
		static unsigned long given;
		timespec t {};
		clock_gettime(CLOCK_MONOTONIC, &t);
		DS_LOG("brick video: a frame given up, two waiting for the panel (%lu so far, at %.1f s)", ++given, (double)t.tv_sec + t.tv_nsec * 1e-9);
		return 1;
	}
	const Video::Device d = video.gpuDevice();
	Copy& c = copies[nextCopy];
	vk::CommandBuffer cmd = *commands[nextCopy];
	nextCopy ^= 1;
	// this copy and its commands were last used two frames ago: that frame drawn first (the screen's submissions go through its
	// submit thread; reset while that frame still waited there, an older page would show a newer picture)
	if (c.page >= 0)
		video.waitPageDrawn((uint32_t)c.page);
	if ((c.width != (uint32_t)srcw || c.height != (uint32_t)srch) && !makeCopy(c, (uint32_t)srcw, (uint32_t)srch))
		return 0;
	cmd.reset();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	const vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
	// the GE's writes before the copy reads them
	const vk::MemoryBarrier written(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eTransferWrite,
			vk::AccessFlagBits::eTransferRead);
	const vk::ImageMemoryBarrier toDst(c.shaderReady ? vk::AccessFlagBits::eShaderRead : vk::AccessFlags(), vk::AccessFlagBits::eTransferWrite,
			vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *c.image, range);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eTransfer | vk::PipelineStageFlagBits::eFragmentShader,
			vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags(), written, nullptr, toDst);
	const vk::ImageSubresourceLayers layers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	const vk::ImageBlit blit(layers, { vk::Offset3D(0, 0, 0), vk::Offset3D(srcw, srch, 1) }, layers,
			{ vk::Offset3D(0, 0, 0), vk::Offset3D(srcw, srch, 1) });
	cmd.blitImage((VkImage)vk_image, vk::ImageLayout::eTransferSrcOptimal, *c.image, vk::ImageLayout::eTransferDstOptimal, blit,
			vk::Filter::eNearest);
	const vk::ImageMemoryBarrier toGeneral(vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead, vk::ImageLayout::eTransferDstOptimal,
			vk::ImageLayout::eGeneral, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, *c.image, range);
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, vk::DependencyFlags(), nullptr, nullptr,
			toGeneral);
	cmd.end();
	c.shaderReady = true;
	letterbox((uint32_t)srcw, (uint32_t)srch);
	lastFence = video.frameImage(*c.view, (uint32_t)srcw, (uint32_t)srch, 0, 0, (uint32_t)srcw, (uint32_t)srch, cmd);
	c.page = (int)video.lastPage();
	return 1;
}

extern "C" int sdl3vk_present_image(void *vk_image)
{
	(void)vk_image;
	return 0;
}

// a GE target about to go: nothing of ours reads it once the last copy's submission is done
extern "C" int sdl3vk_wait_image(void *vk_image)
{
	(void)vk_image;
	if (!video.ready())
		return 1;
	video.submitDrain();
	if (lastFence)
		(void)video.gpuDevice().device.waitForFences(lastFence, true, UINT64_MAX);
	return 1;
}
