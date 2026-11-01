/*
HOST_VK_PRESENT.C

The Vulkan renderer's surface and swapchain (port/android/VULKAN.md, phase 2),
on the game's window: the surface is made with vkCreateAndroidSurfaceKHR on the
window's ANativeWindow (no SDL_Vulkan_* call: SDL would load the phone's driver
beside the chosen one), the swapchain is FIFO with the surface's own images'
count plus one, and a frame is shown by a letterboxed blit of the game's back
buffer into the acquired image.

Android destroys the window's ANativeWindow when the app goes to the background
and makes a new one on return; SDL's lifecycle events do not reach the game's
loop (phase 0), so the loss is found from what the calls return and from the
window's ANativeWindow changing. While there is no swapchain a frame's work is
still submitted (its fence and number move on) and nothing goes to the screen.
*/

#include "host.h"
#include "host_vk.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define B host_vkb

#define ACQUIRE_TIMEOUT_NS 1000000000ull
/* how often (in presented frames) the surface's size is looked at */
#define EXTENT_CHECK_FRAMES 60

static uint64_t now_ns(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000000000ull + (uint64_t)time.tv_nsec;
}

/* the window's ANativeWindow as it is now (a new one after the app was away), or NULL */
static void *native_window(void)
{
	if (!host_vk_window)
		return NULL;
	return SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)host_vk_window),
		SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
}

int host_vk_surface_make(int quiet)
{
	VkAndroidSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
	void *native = native_window();
	VkResult result;

	if (B.surface)
		return 1;
	if (!native)
		return 0;
	info.window = (struct ANativeWindow *)native;
	result = host_vk.vkCreateAndroidSurfaceKHR(host_vk.instance, &info, NULL, &B.surface);
	if (result != VK_SUCCESS)
	{
		B.surface = VK_NULL_HANDLE;
		if (!quiet)
			host_vk_check(result, "vkCreateAndroidSurfaceKHR");
		return 0;
	}
	B.native_window = native;
	return 1;
}

/* ---------- the swapchain */

static const char *transform_name(VkSurfaceTransformFlagBitsKHR transform)
{
	switch (transform)
	{
	case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "identity";
	case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "rotate_90";
	case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "rotate_180";
	case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "rotate_270";
	default: return "other";
	}
}

static void swapchain_destroy(struct host_vk_swapchain *chain)
{
	uint32_t index;

	for (index = 0; index < chain->count; index++)
	{
		if (chain->render_done[index])
			vkDestroySemaphore(B.device, chain->render_done[index], NULL);
		if (chain->views[index])
			vkDestroyImageView(B.device, chain->views[index], NULL);
	}
	if (chain->handle)
		vkDestroySwapchainKHR(B.device, chain->handle, NULL);
	memset(chain, 0, sizeof(*chain));
}

/* the size the surface says its images are: its current extent, or the window's pixels where the surface leaves
it to the swapchain */
static int surface_extent(const VkSurfaceCapabilitiesKHR *caps, VkExtent2D *extent)
{
	*extent = caps->currentExtent;
	if (extent->width == 0xffffffffu)
	{
		int width = 0, height = 0;

		if (host_vk_window)
			SDL_GetWindowSizeInPixels((SDL_Window *)host_vk_window, &width, &height);
		extent->width = (uint32_t)width;
		extent->height = (uint32_t)height;
	}
	return extent->width && extent->height;
}

/* makes a swapchain for the surface as it is now: FIFO, the surface's own transform left to the compositor
(preTransform identity: Android turns the picture), the first of B8G8R8A8_UNORM or R8G8B8A8_UNORM it lists in the
sRGB-nonlinear colour space, one image more than the minimum. Returns 0 (and says why, unless quiet) if it cannot. */
static int swapchain_create(struct host_vk_swapchain *chain, VkSwapchainKHR old, int quiet, VkSurfaceTransformFlagBitsKHR *transform_out)
{
	VkSurfaceCapabilitiesKHR caps;
	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	VkSurfaceFormatKHR formats[32];
	uint32_t format_count = 32, index, count;
	VkResult result;
	int chosen = -1;
	VkCompositeAlphaFlagBitsKHR alpha;
	VkExtent2D extent;
	VkFormatProperties properties;

	memset(chain, 0, sizeof(*chain));
	result = host_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(host_vk.physical, B.surface, &caps);
	if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		return 0;
	}
	if (result != VK_SUCCESS)
		return quiet ? 0 : host_vk_check(result, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	result = host_vk.vkGetPhysicalDeviceSurfaceFormatsKHR(host_vk.physical, B.surface, &format_count, formats);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return quiet ? 0 : host_vk_check(result, "vkGetPhysicalDeviceSurfaceFormatsKHR");
	for (index = 0; index < format_count && chosen < 0; index++)
	{
		if ((formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) &&
			formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			chosen = (int)index;
	}
	if (chosen < 0)
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the surface lists neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM (sRGB nonlinear)");
		return 0;
	}
	if (!surface_extent(&caps, &extent))
	{
		if (!quiet)
			host_logf(HOST_LOG_WARN, "vk: the surface has no size yet");
		return 0;
	}
	if ((caps.supportedUsageFlags & (VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) !=
		(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the surface cannot take colour attachment and transfer destination usage");
		return 0;
	}
	/* the picture is blitted into the swapchain's images, so their format has to be a blit destination */
	host_vk.vkGetPhysicalDeviceFormatProperties(host_vk.physical, formats[chosen].format, &properties);
	if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
	{
		if (!quiet)
			host_logf(HOST_LOG_ERROR, "vk: the swapchain format %d is not a blit destination here", (int)formats[chosen].format);
		return 0;
	}
	count = caps.minImageCount + 1;
	if (caps.maxImageCount && count > caps.maxImageCount)
		count = caps.maxImageCount;
	/* opaque if the surface offers it, else the first of the others (this device's surface offers only inherit) */
	alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
		(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR :
		(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR :
		VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	info.surface = B.surface;
	info.minImageCount = count;
	info.imageFormat = formats[chosen].format;
	info.imageColorSpace = formats[chosen].colorSpace;
	info.imageExtent = extent;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ?
		VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
	info.compositeAlpha = alpha;
	info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old;
	result = vkCreateSwapchainKHR(B.device, &info, NULL, &chain->handle);
	if (result == VK_ERROR_SURFACE_LOST_KHR || result == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
	{
		chain->handle = VK_NULL_HANDLE;
		host_vk_surface_lost("vkCreateSwapchainKHR");
		return 0;
	}
	if (result != VK_SUCCESS)
	{
		chain->handle = VK_NULL_HANDLE;
		return quiet ? 0 : host_vk_check(result, "vkCreateSwapchainKHR");
	}
	chain->format = info.imageFormat;
	chain->extent = info.imageExtent;
	count = HOST_VK_SWAPCHAIN_IMAGES;
	result = vkGetSwapchainImagesKHR(B.device, chain->handle, &count, chain->images);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
	{
		host_vk_check(result, "vkGetSwapchainImagesKHR");
		swapchain_destroy(chain);
		return 0;
	}
	chain->count = count;
	for (index = 0; index < count; index++)
	{
		VkSemaphoreCreateInfo semaphore = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

		if (!host_vk_check(vkCreateSemaphore(B.device, &semaphore, NULL, &chain->render_done[index]), "vkCreateSemaphore"))
		{
			/* the handle and the semaphores made so far */
			swapchain_destroy(chain);
			return 0;
		}
		{
			/* used for the UI overlay and debug.vk_present_marker */
			VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };

			view.image = chain->images[index];
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = chain->format;
			view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			view.subresourceRange.levelCount = 1;
			view.subresourceRange.layerCount = 1;
			if (!host_vk_check(vkCreateImageView(B.device, &view, NULL, &chain->views[index]), "vkCreateImageView"))
			{
				swapchain_destroy(chain);
				return 0;
			}
		}
	}
	if (transform_out)
		*transform_out = info.preTransform;
	return 1;
}

/* the swapchain and the surface are gone: waits for the device, then destroys them */
static void present_teardown(void)
{
	if (B.device)
		vkDeviceWaitIdle(B.device);
	swapchain_destroy(&B.chain);
	if (B.surface)
		host_vk.vkDestroySurfaceKHR(host_vk.instance, B.surface, NULL);
	B.surface = VK_NULL_HANDLE;
	B.native_window = NULL;
	host_vk_presenting = 0;
}

/* the surface is gone (found by acquire, by present, or by the window's ANativeWindow changing): counted and
logged once, however many tries it takes to make another */
void host_vk_surface_lost(const char *where)
{
	if (!B.lost_at)
	{
		B.lost_at = now_ns();
		B.lost_attempts = 0;
		host_logf(HOST_LOG_INFO, "vk: the surface is lost (%s)", where);
		/* the app is going to the background, which Android may follow with the process's end */
		host_vk_pipeline_cache_save("the surface was lost");
	}
	present_teardown();
}

/* a swapchain, if there is none: on the surface there is, or on one made now. Logs the return from a loss once. */
static int ensure_swapchain(void)
{
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	void *native = native_window();

	/* the window has a new ANativeWindow: the surface is on the old one */
	if (B.surface && native != B.native_window)
		host_vk_surface_lost(native ? "the window's ANativeWindow changed" : "the window has no ANativeWindow");
	if (B.chain.handle)
		return 1;
	if (B.lost_at)
		B.lost_attempts++;
	if (!B.surface && !host_vk_surface_make(1))
		return 0;
	if (!swapchain_create(&B.chain, VK_NULL_HANDLE, B.lost_at != 0, &transform))
		return 0;
	if (B.lost_at)
	{
		host_logf(HOST_LOG_INFO, "vk: the surface is back after %.1f ms, %u attempts: swapchain %ux%u, %u images, "
			"transform %s", (double)(now_ns() - B.lost_at) / 1e6, B.lost_attempts, B.chain.extent.width,
			B.chain.extent.height, B.chain.count, transform_name(transform));
		B.lost_at = 0;
	}
	else
	{
		host_logf(HOST_LOG_INFO, "vk: swapchain %ux%u, %u images, format %s, FIFO, preTransform %s", B.chain.extent.width,
			B.chain.extent.height, B.chain.count, B.chain.format == VK_FORMAT_B8G8R8A8_UNORM ? "B8G8R8A8_UNORM" :
			"R8G8B8A8_UNORM", transform_name(transform));
	}
	return 1;
}

/* makes the swapchain again from the old one, with the device idle first; the reason and the time are logged */
static void swapchain_recreate(const char *reason)
{
	uint64_t start = now_ns();
	struct host_vk_swapchain made, retired;
	VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	int ok;

	vkDeviceWaitIdle(B.device);
	ok = swapchain_create(&made, B.chain.handle, 0, &transform);
	/* the old swapchain is retired by the new one's creation; the device is idle, so its images are not in use */
	/* (if the creation found the surface lost, host_vk_surface_lost has destroyed the old swapchain and the surface
	already, and B.chain is empty here) */
	retired = B.chain;
	memset(&B.chain, 0, sizeof(B.chain));
	swapchain_destroy(&retired);
	if (ok)
	{
		B.chain = made;
		B.counts.recreations++;
		host_logf(HOST_LOG_INFO, "vk: swapchain made again (%s): %ux%u, %u images, %.2f ms", reason, B.chain.extent.width,
			B.chain.extent.height, B.chain.count, (double)(now_ns() - start) / 1e6);
	}
	else
	{
		host_vk_presenting = 0;
		host_logf(HOST_LOG_WARN, "vk: the swapchain could not be made again (%s)", reason);
	}
}

/* does the surface's size differ from the swapchain's? Looked at when the driver says SUBOPTIMAL (Android says so
for the rotation of the display, which is not a reason to make the swapchain again) and every so often */
static void extent_check(void)
{
	VkSurfaceCapabilitiesKHR caps;
	VkExtent2D extent;
	VkResult result;

	result = host_vk.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(host_vk.physical, B.surface, &caps);
	if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
		return;
	}
	if (result != VK_SUCCESS || !surface_extent(&caps, &extent))
		return;
	if (extent.width != B.chain.extent.width || extent.height != B.chain.extent.height)
		swapchain_recreate("the surface's size changed");
}

/* ---------- the UI overlay */

#define OVERLAY_ATLAS_SIZE 2048
#define OVERLAY_ICON_SIZE 128
#define OVERLAY_ICON_LAYERS 10
#define OVERLAY_ICON_LEVELS 8

static struct
{
	int ready, failed;
	VkImage atlas_image, icons_image, picture_image;
	VkDeviceMemory atlas_memory, icons_memory, picture_memory;
	VkImageView atlas_view, icons_view, picture_view;
	uint32_t picture_width, picture_height;
	VkImage r_images[HOST_VK_FRAMES];
	VkDeviceMemory r_memories[HOST_VK_FRAMES];
	VkImageView r_views[HOST_VK_FRAMES];
	VkSampler linear_sampler, mipmap_sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool pool;
	VkDescriptorSet sets[HOST_VK_FRAMES];
	VkPipelineLayout layout;
	VkShaderModule vertex_shader, fragment_shader;
	VkPipeline pipeline;
	VkFormat pipeline_format;
	struct vk_command_overlay pending;
	int has_pending;
} ov;

static const char overlay_vertex_glsl[] =
	"#version 450\n"
	"layout(location = 0) in vec2 position;\n"
	"layout(location = 1) in vec2 texture_coordinate;\n"
	"layout(location = 2) in vec4 shape;\n"
	"layout(location = 3) in vec2 edge;\n"
	"layout(location = 4) in vec4 color;\n"
	"layout(push_constant) uniform Push {\n"
	"\tvec4 scale;\n"
	"\tvec4 cutouts[4];\n"
	"} pc;\n"
	"layout(location = 0) out vec2 v_texture_coordinate;\n"
	"layout(location = 1) out vec4 v_shape;\n"
	"layout(location = 2) out vec2 v_edge;\n"
	"layout(location = 3) out vec4 v_color;\n"
	"void main()\n"
	"{\n"
	"\tv_texture_coordinate = texture_coordinate;\n"
	"\tv_shape = shape;\n"
	"\tv_edge = edge;\n"
	"\tv_color = color;\n"
	"\tgl_Position = vec4(position * pc.scale.xy - vec2(1.0, 1.0), 0.0, 1.0);\n"
	"}\n";

static const char overlay_fragment_glsl[] =
	"#version 450\n"
	"layout(location = 0) in vec2 v_texture_coordinate;\n"
	"layout(location = 1) in vec4 v_shape;\n"
	"layout(location = 2) in vec2 v_edge;\n"
	"layout(location = 3) in vec4 v_color;\n"
	"layout(set = 0, binding = 0) uniform sampler2D atlas;\n"
	"layout(set = 0, binding = 1) uniform sampler2DArray icons;\n"
	"layout(set = 0, binding = 2) uniform sampler2D picture;\n"
	"layout(push_constant) uniform Push {\n"
	"\tvec4 scale;\n"
	"\tvec4 cutouts[4];\n"
	"} pc;\n"
	"layout(location = 0) out vec4 fragment;\n"
	"void main()\n"
	"{\n"
	"\tfloat alpha;\n"
	"\tvec2 p = gl_FragCoord.xy;\n"
	"\tfor (int i = 0; i < 4; i++)\n"
	"\t\tif (p.x >= pc.cutouts[i].x && p.y >= pc.cutouts[i].y && p.x < pc.cutouts[i].z && p.y < pc.cutouts[i].w)\n"
	"\t\t\tdiscard;\n"
	"\tif (v_edge.x < -2.5)\n"
	"\t{\n"
	"\t\tfragment = texture(picture, v_texture_coordinate) * v_color;\n"
	"\t\treturn;\n"
	"\t}\n"
	"\tif (v_edge.x < -1.5)\n"
	"\t{\n"
	"\t\tvec4 texel = texture(icons, vec3(v_texture_coordinate, v_shape.x));\n"
	"\t\tfragment = vec4(texel.rgb / max(texel.a, 0.001), texel.a) * v_color;\n"
	"\t\treturn;\n"
	"\t}\n"
	"\tif (v_texture_coordinate.x >= 0.0)\n"
	"\t\talpha = texture(atlas, v_texture_coordinate).r;\n"
	"\telse\n"
	"\t{\n"
	"\t\tvec2 q = abs(v_shape.xy) - v_shape.zw + vec2(v_edge.x);\n"
	"\t\tfloat d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_edge.x;\n"
	"\t\talpha = clamp(0.5 - d, 0.0, 1.0);\n"
	"\t\tif (v_edge.y > 0.0)\n"
	"\t\t\talpha *= clamp(d + v_edge.y + 0.5, 0.0, 1.0);\n"
	"\t}\n"
	"\tfragment = vec4(v_color.rgb, v_color.a * alpha);\n"
	"}\n";

static void ov_image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
	uint32_t base_level, uint32_t levels, uint32_t layers)
{
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

	barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : (VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT);
	barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = base_level;
	barrier.subresourceRange.levelCount = levels;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = layers;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
		1, &barrier);
}

static int ov_make_image(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
	VkImageUsageFlags usage, VkImageViewType view_type, VkImage *image_out, VkDeviceMemory *memory_out, VkImageView *view_out)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkMemoryRequirements req;
	VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };

	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = layers;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (!HOST_VK_CHECK(vkCreateImage(B.device, &info, NULL, image_out)))
		return 0;
	vkGetImageMemoryRequirements(B.device, *image_out, &req);
	alloc.allocationSize = req.size;
	alloc.memoryTypeIndex = host_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (alloc.memoryTypeIndex == UINT32_MAX ||
		!HOST_VK_CHECK(vkAllocateMemory(B.device, &alloc, NULL, memory_out)) ||
		!HOST_VK_CHECK(vkBindImageMemory(B.device, *image_out, *memory_out, 0)))
		return 0;
	view.image = *image_out;
	view.viewType = view_type;
	view.format = format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = levels;
	view.subresourceRange.layerCount = layers;
	return HOST_VK_CHECK(vkCreateImageView(B.device, &view, NULL, view_out));
}

static VkShaderModule ov_compile_shader(const char *source, int fragment)
{
	uint32_t *words = NULL;
	size_t count = 0;
	char message[512] = "";
	VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	VkShaderModule module = VK_NULL_HANDLE;

	if (!host_vk_glslang_compile(source, fragment, &words, &count, message, sizeof(message)))
	{
		host_logf(HOST_LOG_ERROR, "vk: overlay shader failed to compile: %s", message);
		return VK_NULL_HANDLE;
	}
	info.codeSize = count * sizeof(uint32_t);
	info.pCode = words;
	HOST_VK_CHECK(vkCreateShaderModule(B.device, &info, NULL, &module));
	free(words);
	return module;
}

static void ov_update_set(int slot)
{
	VkDescriptorImageInfo images[3];
	VkWriteDescriptorSet writes[3];
	int i;

	memset(images, 0, sizeof(images));
	memset(writes, 0, sizeof(writes));
	images[0].sampler = ov.linear_sampler;
	images[0].imageView = ov.atlas_view;
	images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	images[1].sampler = ov.mipmap_sampler;
	images[1].imageView = ov.icons_view;
	images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	images[2].sampler = ov.linear_sampler;
	images[2].imageView = ov.picture_view;
	images[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for (i = 0; i < 3; i++)
	{
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = ov.sets[slot];
		writes[i].dstBinding = (uint32_t)i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(B.device, 3, writes, 0, NULL);
}

static int ov_ensure(VkCommandBuffer command)
{
	VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	VkDescriptorSetLayoutBinding bindings[3];
	VkDescriptorSetLayoutCreateInfo set_layout = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	VkDescriptorSetLayout layouts[HOST_VK_FRAMES];
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	VkClearColorValue zero = { { 0.0f, 0.0f, 0.0f, 0.0f } };
	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	int i;

	if (ov.ready || ov.failed)
		return ov.ready;
	if (!ov_make_image(VK_FORMAT_R8_UNORM, OVERLAY_ATLAS_SIZE, OVERLAY_ATLAS_SIZE, 1, 1,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_VIEW_TYPE_2D,
			&ov.atlas_image, &ov.atlas_memory, &ov.atlas_view) ||
		!ov_make_image(VK_FORMAT_R8G8B8A8_UNORM, OVERLAY_ICON_SIZE, OVERLAY_ICON_SIZE, OVERLAY_ICON_LEVELS, OVERLAY_ICON_LAYERS,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			VK_IMAGE_VIEW_TYPE_2D_ARRAY, &ov.icons_image, &ov.icons_memory, &ov.icons_view) ||
		!ov_make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1,
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_VIEW_TYPE_2D,
			&ov.picture_image, &ov.picture_memory, &ov.picture_view))
	{
		ov.failed = 1;
		return 0;
	}
	ov.picture_width = 1;
	ov.picture_height = 1;
	ov_image_barrier(command, ov.atlas_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
	vkCmdClearColorImage(command, ov.atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
	ov_image_barrier(command, ov.atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

	range.levelCount = OVERLAY_ICON_LEVELS;
	range.layerCount = OVERLAY_ICON_LAYERS;
	ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		0, OVERLAY_ICON_LEVELS, OVERLAY_ICON_LAYERS);
	vkCmdClearColorImage(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
	ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		0, OVERLAY_ICON_LEVELS, OVERLAY_ICON_LAYERS);

	range.levelCount = 1;
	range.layerCount = 1;
	ov_image_barrier(command, ov.picture_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
	vkCmdClearColorImage(command, ov.picture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
	ov_image_barrier(command, ov.picture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);

	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	if (!HOST_VK_CHECK(vkCreateSampler(B.device, &sampler, NULL, &ov.linear_sampler)))
	{
		ov.failed = 1;
		return 0;
	}
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sampler.maxLod = 16.0f;
	if (!HOST_VK_CHECK(vkCreateSampler(B.device, &sampler, NULL, &ov.mipmap_sampler)))
	{
		ov.failed = 1;
		return 0;
	}
	memset(bindings, 0, sizeof(bindings));
	for (i = 0; i < 3; i++)
	{
		bindings[i].binding = (uint32_t)i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	set_layout.bindingCount = 3;
	set_layout.pBindings = bindings;
	if (!HOST_VK_CHECK(vkCreateDescriptorSetLayout(B.device, &set_layout, NULL, &ov.set_layout)))
	{
		ov.failed = 1;
		return 0;
	}
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 3 * HOST_VK_FRAMES;
	pool.maxSets = HOST_VK_FRAMES;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &pool_size;
	if (!HOST_VK_CHECK(vkCreateDescriptorPool(B.device, &pool, NULL, &ov.pool)))
	{
		ov.failed = 1;
		return 0;
	}
	for (i = 0; i < HOST_VK_FRAMES; i++)
		layouts[i] = ov.set_layout;
	alloc.descriptorPool = ov.pool;
	alloc.descriptorSetCount = HOST_VK_FRAMES;
	alloc.pSetLayouts = layouts;
	if (!HOST_VK_CHECK(vkAllocateDescriptorSets(B.device, &alloc, ov.sets)))
	{
		ov.failed = 1;
		return 0;
	}
	for (i = 0; i < HOST_VK_FRAMES; i++)
		ov_update_set(i);

	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0;
	push.size = 80;
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &ov.set_layout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &push;
	if (!HOST_VK_CHECK(vkCreatePipelineLayout(B.device, &layout, NULL, &ov.layout)))
	{
		ov.failed = 1;
		return 0;
	}
	ov.vertex_shader = ov_compile_shader(overlay_vertex_glsl, 0);
	ov.fragment_shader = ov_compile_shader(overlay_fragment_glsl, 1);
	if (!ov.vertex_shader || !ov.fragment_shader)
	{
		ov.failed = 1;
		return 0;
	}
	ov.ready = 1;
	return 1;
}

static VkPipeline ov_pipeline_get(VkFormat format)
{
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attrs[5];
	VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	VkPipelineInputAssemblyStateCreateInfo assembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	VkPipelineColorBlendAttachmentState blend_att;
	VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	VkPipelineRenderingCreateInfoKHR rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
	VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };

	if (ov.pipeline && ov.pipeline_format == format)
		return ov.pipeline;
	if (ov.pipeline)
	{
		vkDestroyPipeline(B.device, ov.pipeline, NULL);
		ov.pipeline = VK_NULL_HANDLE;
	}
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = ov.vertex_shader;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = ov.fragment_shader;
	stages[1].pName = "main";

	binding.binding = 0;
	binding.stride = 44;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	memset(attrs, 0, sizeof(attrs));
	attrs[0].location = 0; attrs[0].binding = 0; attrs[0].format = VK_FORMAT_R32G32_SFLOAT; attrs[0].offset = 0;
	attrs[1].location = 1; attrs[1].binding = 0; attrs[1].format = VK_FORMAT_R32G32_SFLOAT; attrs[1].offset = 8;
	attrs[2].location = 2; attrs[2].binding = 0; attrs[2].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[2].offset = 16;
	attrs[3].location = 3; attrs[3].binding = 0; attrs[3].format = VK_FORMAT_R32G32_SFLOAT; attrs[3].offset = 32;
	attrs[4].location = 4; attrs[4].binding = 0; attrs[4].format = VK_FORMAT_R8G8B8A8_UNORM; attrs[4].offset = 40;

	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &binding;
	vertex_input.vertexAttributeDescriptionCount = 5;
	vertex_input.pVertexAttributeDescriptions = attrs;

	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	viewport.viewportCount = 1;
	viewport.scissorCount = 1;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
	raster.lineWidth = 1.0f;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	memset(&blend_att, 0, sizeof(blend_att));
	blend_att.blendEnable = VK_TRUE;
	blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_att.colorBlendOp = VK_BLEND_OP_ADD;
	blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_att;

	dynamic.dynamicStateCount = 2;
	dynamic.pDynamicStates = dynamic_states;

	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachmentFormats = &format;

	info.pNext = &rendering;
	info.stageCount = 2;
	info.pStages = stages;
	info.pVertexInputState = &vertex_input;
	info.pInputAssemblyState = &assembly;
	info.pViewportState = &viewport;
	info.pRasterizationState = &raster;
	info.pMultisampleState = &multisample;
	info.pColorBlendState = &blend;
	info.pDynamicState = &dynamic;
	info.layout = ov.layout;
	if (!HOST_VK_CHECK(vkCreateGraphicsPipelines(B.device, B.pipeline_cache, 1, &info, NULL, &ov.pipeline)))
		return VK_NULL_HANDLE;
	ov.pipeline_format = format;
	return ov.pipeline;
}

void host_vk_overlay_command(const struct vk_command_overlay *cmd)
{
	VkCommandBuffer command;
	VkBuffer buf;
	VkDeviceSize offset;

	host_vk_rendering_end();
	command = host_vk_frame_command();
	if (ov.r_views[B.frame])
	{
		vkDestroyImageView(B.device, ov.r_views[B.frame], NULL);
		vkDestroyImage(B.device, ov.r_images[B.frame], NULL);
		vkFreeMemory(B.device, ov.r_memories[B.frame], NULL);
		ov.r_views[B.frame] = VK_NULL_HANDLE;
		ov.r_images[B.frame] = VK_NULL_HANDLE;
		ov.r_memories[B.frame] = VK_NULL_HANDLE;
	}
	if (!ov_ensure(command))
		return;
	if (cmd->atlas.id && cmd->dirty_rows > 0 && cmd->atlas_size == OVERLAY_ATLAS_SIZE &&
		cmd->dirty_top + cmd->dirty_rows <= OVERLAY_ATLAS_SIZE &&
		host_vk_data_find(cmd->atlas.id, cmd->atlas.offset, cmd->dirty_rows * cmd->atlas_size, &buf, &offset))
	{
		VkBufferImageCopy copy;

		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = offset;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageOffset.y = (int32_t)cmd->dirty_top;
		copy.imageExtent.width = cmd->atlas_size;
		copy.imageExtent.height = cmd->dirty_rows;
		copy.imageExtent.depth = 1;
		ov_image_barrier(command, ov.atlas_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
		vkCmdCopyBufferToImage(command, buf, ov.atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		ov_image_barrier(command, ov.atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);
	}
	if (cmd->icons.id && cmd->icon_size == OVERLAY_ICON_SIZE && cmd->icon_count == OVERLAY_ICON_LAYERS &&
		host_vk_data_find(cmd->icons.id, cmd->icons.offset,
			cmd->icon_size * cmd->icon_size * 4 * cmd->icon_count, &buf, &offset))
	{
		VkBufferImageCopy copy;
		uint32_t level, w = OVERLAY_ICON_SIZE, h = OVERLAY_ICON_SIZE;

		memset(&copy, 0, sizeof(copy));
		copy.bufferOffset = offset;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.baseArrayLayer = 0;
		copy.imageSubresource.layerCount = OVERLAY_ICON_LAYERS;
		copy.imageExtent.width = OVERLAY_ICON_SIZE;
		copy.imageExtent.height = OVERLAY_ICON_SIZE;
		copy.imageExtent.depth = 1;
		ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, OVERLAY_ICON_LEVELS, OVERLAY_ICON_LAYERS);
		vkCmdCopyBufferToImage(command, buf, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		for (level = 1; level < OVERLAY_ICON_LEVELS; level++)
		{
			VkImageBlit blit;
			uint32_t nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;

			ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level - 1, 1, OVERLAY_ICON_LAYERS);
			memset(&blit, 0, sizeof(blit));
			blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.srcSubresource.mipLevel = level - 1;
			blit.srcSubresource.layerCount = OVERLAY_ICON_LAYERS;
			blit.srcOffsets[1].x = (int32_t)w;
			blit.srcOffsets[1].y = (int32_t)h;
			blit.srcOffsets[1].z = 1;
			blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			blit.dstSubresource.mipLevel = level;
			blit.dstSubresource.layerCount = OVERLAY_ICON_LAYERS;
			blit.dstOffsets[1].x = (int32_t)nw;
			blit.dstOffsets[1].y = (int32_t)nh;
			blit.dstOffsets[1].z = 1;
			vkCmdBlitImage(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ov.icons_image,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
			ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, level - 1, 1, OVERLAY_ICON_LAYERS);
			w = nw;
			h = nh;
		}
		ov_image_barrier(command, ov.icons_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, OVERLAY_ICON_LEVELS - 1, 1, OVERLAY_ICON_LAYERS);
	}
	if (cmd->picture.id && cmd->picture_width > 0 && cmd->picture_height > 0 &&
		host_vk_data_find(cmd->picture.id, cmd->picture.offset, cmd->picture_width * cmd->picture_height * 4, &buf, &offset))
	{
		VkBufferImageCopy copy;

		if (cmd->picture_width != ov.picture_width || cmd->picture_height != ov.picture_height)
		{
			VkImage new_img = VK_NULL_HANDLE;
			VkDeviceMemory new_mem = VK_NULL_HANDLE;
			VkImageView new_view = VK_NULL_HANDLE;

			if (ov_make_image(VK_FORMAT_R8G8B8A8_UNORM, cmd->picture_width, cmd->picture_height, 1, 1,
				VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_VIEW_TYPE_2D,
				&new_img, &new_mem, &new_view))
			{
				ov.r_images[B.frame] = ov.picture_image;
				ov.r_memories[B.frame] = ov.picture_memory;
				ov.r_views[B.frame] = ov.picture_view;
				ov.picture_image = new_img;
				ov.picture_memory = new_mem;
				ov.picture_view = new_view;
				ov.picture_width = cmd->picture_width;
				ov.picture_height = cmd->picture_height;
				ov_image_barrier(command, ov.picture_image, VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);
			}
		}
		if (cmd->picture_width == ov.picture_width && cmd->picture_height == ov.picture_height)
		{
			memset(&copy, 0, sizeof(copy));
			copy.bufferOffset = offset;
			copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copy.imageSubresource.layerCount = 1;
			copy.imageExtent.width = cmd->picture_width;
			copy.imageExtent.height = cmd->picture_height;
			copy.imageExtent.depth = 1;
			ov_image_barrier(command, ov.picture_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 1, 1);
			vkCmdCopyBufferToImage(command, buf, ov.picture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			ov_image_barrier(command, ov.picture_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 1, 1);
		}
	}
	ov_update_set(B.frame);
	ov.pending = *cmd;
	ov.has_pending = 1;
}

static void ov_draw(VkCommandBuffer command, VkImageView view)
{
	VkPipeline pipeline;
	VkBuffer vbuf;
	VkDeviceSize voffset;
	VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	VkViewport viewport;
	VkRect2D scissor;
	float push[20];
	float sx, sy;
	int i;

	if (!ov.has_pending)
		return;
	ov.has_pending = 0;
	if (!ov.ready || ov.pending.vertex_count == 0 || ov.pending.vertex_bytes != 44 ||
		ov.pending.window_width == 0 || ov.pending.window_height == 0)
		return;
	if (!host_vk_data_find(ov.pending.vertices.id, ov.pending.vertices.offset,
		ov.pending.vertex_count * ov.pending.vertex_bytes, &vbuf, &voffset))
		return;
	pipeline = ov_pipeline_get(B.chain.format);
	if (!pipeline)
		return;

	attachment.imageView = view;
	attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	rendering.renderArea.extent = B.chain.extent;
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &attachment;
	host_vk_cmd_begin_rendering(command, &rendering);

	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)B.chain.extent.width;
	viewport.height = (float)B.chain.extent.height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(command, 0, 1, &viewport);
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent = B.chain.extent;
	vkCmdSetScissor(command, 0, 1, &scissor);

	sx = (float)B.chain.extent.width / (float)ov.pending.window_width;
	sy = (float)B.chain.extent.height / (float)ov.pending.window_height;
	memset(push, 0, sizeof(push));
	push[0] = 2.0f / (float)ov.pending.window_width;
	push[1] = 2.0f / (float)ov.pending.window_height;
	for (i = 0; i < 4; i++)
	{
		push[4 + i * 4 + 0] = ov.pending.cutouts[i][0] * sx;
		push[4 + i * 4 + 1] = ov.pending.cutouts[i][1] * sy;
		push[4 + i * 4 + 2] = ov.pending.cutouts[i][2] * sx;
		push[4 + i * 4 + 3] = ov.pending.cutouts[i][3] * sy;
	}
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, ov.layout, 0, 1, &ov.sets[B.frame], 0, NULL);
	vkCmdPushConstants(command, ov.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
	vkCmdBindVertexBuffers(command, 0, 1, &vbuf, &voffset);
	vkCmdDraw(command, ov.pending.vertex_count, 1, 0, 0);
	host_vk_cmd_end_rendering(command);
}

/* ---------- presenting */

uint32_t host_vk_present_record(VkCommandBuffer command, struct host_vk_target *back_buffer)
{
	static unsigned frames_since_check;
	VkResult result;
	uint32_t index = 0;
	struct host_vk_frame *frame = &B.frames[B.frame];
	VkImage image;
	VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	VkImageBlit blit;
	int attempt, acquired = 0;
	int64_t width, height, x, y;

	if (!ensure_swapchain())
	{
		host_vk_presenting = 0;
		return UINT32_MAX;
	}
	if (++frames_since_check >= EXTENT_CHECK_FRAMES)
	{
		frames_since_check = 0;
		extent_check();
		if (!B.chain.handle)
			return UINT32_MAX;
	}
	for (attempt = 0; attempt < 2 && !acquired; attempt++)
	{
		result = vkAcquireNextImageKHR(B.device, B.chain.handle, ACQUIRE_TIMEOUT_NS, frame->acquired, VK_NULL_HANDLE, &index);
		if (result == VK_SUCCESS)
		{
			acquired = 1;
		}
		else if (result == VK_SUBOPTIMAL_KHR)
		{
			/* the image is acquired and its semaphore signalled: it is used; the size is looked at after the present */
			acquired = 1;
			frames_since_check = EXTENT_CHECK_FRAMES;
		}
		else if (result == VK_ERROR_OUT_OF_DATE_KHR)
		{
			swapchain_recreate("out of date");
			if (!B.chain.handle)
				return UINT32_MAX;
		}
		else if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			host_vk_surface_lost("vkAcquireNextImageKHR");
			return UINT32_MAX;
		}
		else if (result == VK_TIMEOUT || result == VK_NOT_READY)
		{
			/* with the app away nothing frees an image: the surface is as good as lost */
			host_vk_surface_lost("vkAcquireNextImageKHR timed out");
			return UINT32_MAX;
		}
		else
		{
			host_vk_check(result, "vkAcquireNextImageKHR");
			return UINT32_MAX;
		}
	}
	if (!acquired)
		return UINT32_MAX;
	image = B.chain.images[index];

	/* the back buffer is read by the blit; the image is cleared black (the bars) and the picture blitted into it, the
	letterbox as d3d8_gl.c's Present works it out: the window's width, or its height, whichever the picture's shape fits */
	host_vk_target_transition(command, back_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
		1, &barrier);
	{
		VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		vkCmdClearColorImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
	}
	/* (the clear is a transfer write and the blit another: a barrier between them) */
	{
		VkMemoryBarrier memory = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

		memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		memory.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, NULL,
			0, NULL);
	}
	width = B.chain.extent.width;
	height = width * (int64_t)back_buffer->key.pixel_height / (int64_t)back_buffer->key.pixel_width;
	if (height > (int64_t)B.chain.extent.height)
	{
		height = B.chain.extent.height;
		width = height * (int64_t)back_buffer->key.pixel_width / (int64_t)back_buffer->key.pixel_height;
	}
	x = ((int64_t)B.chain.extent.width - width) / 2;
	y = ((int64_t)B.chain.extent.height - height) / 2;
	if (width > 0 && height > 0)
	{
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1].x = (int32_t)back_buffer->key.pixel_width;
		blit.srcOffsets[1].y = (int32_t)back_buffer->key.pixel_height;
		blit.srcOffsets[1].z = 1;
		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[0].x = (int32_t)x;
		blit.dstOffsets[0].y = (int32_t)y;
		blit.dstOffsets[1].x = (int32_t)(x + width);
		blit.dstOffsets[1].y = (int32_t)(y + height);
		blit.dstOffsets[1].z = 1;
		vkCmdBlitImage(command, back_buffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, B.blit_filter);
	}
	if ((ov.has_pending || (host_vk_present_marker && width > 0 && height > 0)) && B.chain.views[index])
	{
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
			NULL, 1, &barrier);
		if (ov.has_pending)
			ov_draw(command, B.chain.views[index]);
		if (host_vk_present_marker && width > 0 && height > 0)
		{
			/* a red square at the picture's top left and a green one at its top right, so that a screenshot shows
			which way up the picture is and where its corners land. vkCmdClearColorImage clears whole subresources only,
			so the squares are cleared by vkCmdClearAttachments in a rendering on the swapchain image. */
			VkRenderingAttachmentInfo attachment = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
			VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
			VkClearAttachment clear;
			VkClearRect rect;
			uint32_t size = (uint32_t)(width / 16 > 16 ? width / 16 : 16);
			int square;

			if (size > (uint32_t)height)
				size = (uint32_t)height;
			attachment.imageView = B.chain.views[index];
			attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			rendering.renderArea.extent = B.chain.extent;
			rendering.layerCount = 1;
			rendering.colorAttachmentCount = 1;
			rendering.pColorAttachments = &attachment;
			host_vk_cmd_begin_rendering(command, &rendering);
			memset(&clear, 0, sizeof(clear));
			clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			memset(&rect, 0, sizeof(rect));
			rect.layerCount = 1;
			rect.rect.extent.width = size;
			rect.rect.extent.height = size;
			rect.rect.offset.y = (int32_t)y;
			for (square = 0; square < 2; square++)
			{
				clear.clearValue.color.float32[0] = square == 0 ? 1.0f : 0.0f;
				clear.clearValue.color.float32[1] = square == 0 ? 0.0f : 1.0f;
				clear.clearValue.color.float32[3] = 1.0f;
				rect.rect.offset.x = square == 0 ? (int32_t)x : (int32_t)(x + width - size);
				vkCmdClearAttachments(command, 1, &clear, 1, &rect);
			}
			host_vk_cmd_end_rendering(command);
		}
		barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}
	else
	{
		ov.has_pending = 0;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	}
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = 0;
	barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL,
		1, &barrier);
	return index;
}

void host_vk_present_queue(uint32_t image_index)
{
	VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	VkResult result;
	static int first = 1;

	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &B.chain.render_done[image_index];
	present.swapchainCount = 1;
	present.pSwapchains = &B.chain.handle;
	present.pImageIndices = &image_index;
	result = vkQueuePresentKHR(B.queue, &present);
	if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
	{
		if (first)
			host_logf(HOST_LOG_INFO, "vk: first frame presented");
		first = 0;
		host_vk_presenting = 1;
		if (result == VK_SUBOPTIMAL_KHR)
			extent_check();
	}
	else if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		swapchain_recreate("out of date");
	}
	else if (result == VK_ERROR_SURFACE_LOST_KHR)
	{
		host_vk_surface_lost("vkQueuePresentKHR");
	}
	else
	{
		host_vk_check(result, "vkQueuePresentKHR");
		host_vk_presenting = 0;
	}
}
