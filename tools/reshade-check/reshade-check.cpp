// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's ReShade on a PC (tools/reshade-check.sh builds and runs it): the effects a preset
// turns on, compiled as the app compiles them (port/cemu/ReShadeEffects.cpp), each entry point's
// SPIR-V checked with spirv-val when it is installed, and with --run drawn as the app draws them
// (port/cemu/ReShadeRuntime.cpp) on the PC's Vulkan driver, under Khronos' validation layer when
// it is installed, onto a picture saved as a PNG.
//
//   reshade-check FOLDER [--title ID] [--size WxH] [--run IN.png|- OUT.png] [--frames N]
//
// FOLDER is laid out as /data/ps5cemu/reshade is on the console. The exit status is 0 only when
// every technique the preset turns on compiled, was made and (with --run) drew without a
// validation error.

#include "ReShadeEffects.h"
#include "ReShadeRuntime.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
	int s_validationErrors = 0;

	VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
		const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
	{
		if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		{
			s_validationErrors++;
			std::fprintf(stderr, "validation: %s\n", data->pMessage);
		}
		else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
			std::fprintf(stderr, "validation warning: %s\n", data->pMessage);
		return VK_FALSE;
	}

	bool HasProgram(const char* name)
	{
		const std::string command = std::string("command -v ") + name + " >/dev/null 2>&1";
		return std::system(command.c_str()) == 0;
	}

	// spirv-val on each entry point's code; the number that fail
	int ValidateSpirv(const ps5reshade::Chain& chain)
	{
		if (!HasProgram("spirv-val"))
		{
			std::printf("spirv-val is not installed: the SPIR-V is not checked\n");
			return 0;
		}
		int failed = 0, checked = 0;
		char path[] = "/tmp/reshade-check-XXXXXX";
		const int fd = mkstemp(path);
		if (fd < 0)
			return 0;
		close(fd);
		for (const auto& effect : chain.effects)
			for (const auto& [name, code] : effect.code)
			{
				FILE* file = std::fopen(path, "wb");
				std::fwrite(code.data(), 1, code.size(), file);
				std::fclose(file);
				const std::string command = std::string("spirv-val --target-env vulkan1.1 ") + path + " 2>&1";
				FILE* output = popen(command.c_str(), "r");
				std::string text;
				char buffer[512];
				while (output && std::fgets(buffer, sizeof(buffer), output))
					text += buffer;
				const int status = output ? pclose(output) : -1;
				checked++;
				if (status != 0)
				{
					failed++;
					std::printf("  spirv-val: %s %s: %s", effect.name.c_str(), name.c_str(), text.c_str());
				}
			}
		unlink(path);
		std::printf("spirv-val: %d of %d entry points valid\n", checked - failed, checked);
		return failed;
	}

	struct Gpu
	{
		VkInstance instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = 0;
		VkCommandPool pool = VK_NULL_HANDLE;
		bool swapchainLayouts = false; // VK_KHR_swapchain: the picture rests in PRESENT_SRC as Cemu's does
		std::string name;
	};

	bool HasLayer(const char* wanted)
	{
		uint32_t count = 0;
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> layers(count);
		vkEnumerateInstanceLayerProperties(&count, layers.data());
		for (const auto& layer : layers)
			if (std::strcmp(layer.layerName, wanted) == 0)
				return true;
		return false;
	}

	bool StartGpu(Gpu& gpu)
	{
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "reshade-check";
		app.apiVersion = VK_API_VERSION_1_1;
		const char* layer = "VK_LAYER_KHRONOS_validation";
		const bool validation = HasLayer(layer);
		// VK_KHR_surface, which the device's VK_KHR_swapchain (for PRESENT_SRC) needs
		std::vector<const char*> instanceExtensions = {VK_KHR_SURFACE_EXTENSION_NAME};
		if (validation)
			instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		info.pApplicationInfo = &app;
		info.enabledLayerCount = validation ? 1 : 0;
		info.ppEnabledLayerNames = &layer;
		info.enabledExtensionCount = (uint32_t)instanceExtensions.size();
		info.ppEnabledExtensionNames = instanceExtensions.data();
		if (vkCreateInstance(&info, nullptr, &gpu.instance) != VK_SUCCESS)
			return false;
		std::printf("Vulkan: validation layer %s\n", validation ? "on" : "not installed: drawing is not checked");
		if (validation)
		{
			auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT");
			VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
			messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
			messenger.pfnUserCallback = OnValidation;
			if (create)
				create(gpu.instance, &messenger, nullptr, &gpu.messenger);
		}
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
		if (count == 0)
			return false;
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());
		gpu.physicalDevice = devices[0];
		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(gpu.physicalDevice, &properties);
		gpu.name = properties.deviceName;
		vkGetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(gpu.physicalDevice, &count, families.data());
		for (uint32_t i = 0; i < count; i++)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				gpu.family = i;
				break;
			}
		uint32_t extensionCount = 0;
		vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &extensionCount, nullptr);
		std::vector<VkExtensionProperties> extensions(extensionCount);
		vkEnumerateDeviceExtensionProperties(gpu.physicalDevice, nullptr, &extensionCount, extensions.data());
		for (const auto& e : extensions)
			gpu.swapchainLayouts |= std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
		const float priority = 1.0f;
		VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queue.queueFamilyIndex = gpu.family;
		queue.queueCount = 1;
		queue.pQueuePriorities = &priority;
		const char* swapchain = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queue;
		deviceInfo.enabledExtensionCount = gpu.swapchainLayouts ? 1 : 0;
		deviceInfo.ppEnabledExtensionNames = &swapchain;
		if (vkCreateDevice(gpu.physicalDevice, &deviceInfo, nullptr, &gpu.device) != VK_SUCCESS)
			return false;
		vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
		VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = gpu.family;
		return vkCreateCommandPool(gpu.device, &poolInfo, nullptr, &gpu.pool) == VK_SUCCESS;
	}

	uint32_t MemoryType(const Gpu& gpu, uint32_t bits, VkMemoryPropertyFlags wanted)
	{
		VkPhysicalDeviceMemoryProperties memory{};
		vkGetPhysicalDeviceMemoryProperties(gpu.physicalDevice, &memory);
		for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
				return i;
		return 0;
	}

	// Runs work in a command buffer of its own and waits for it
	template<typename Work>
	void Submit(const Gpu& gpu, Work work)
	{
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = gpu.pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		vkAllocateCommandBuffers(gpu.device, &allocate, &cmd);
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &begin);
		work(cmd);
		vkEndCommandBuffer(cmd);
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &cmd;
		vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE);
		vkQueueWaitIdle(gpu.queue);
		vkFreeCommandBuffers(gpu.device, gpu.pool, 1, &cmd);
	}

	void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to)
	{
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.oldLayout = from;
		barrier.newLayout = to;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	}

	// The picture to draw on: a PNG, or bars and a gradient
	std::vector<uint8_t> InputPicture(const std::string& path, uint32_t width, uint32_t height)
	{
		std::vector<uint8_t> bgra((size_t)width * height * 4);
		int w = 0, h = 0, channels = 0;
		stbi_uc* pixels = path == "-" ? nullptr : stbi_load(path.c_str(), &w, &h, &channels, 4);
		if (path != "-" && !pixels)
			std::printf("cannot read %s: drawing on a test pattern\n", path.c_str());
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++)
			{
				uint8_t* out = &bgra[((size_t)y * width + x) * 4];
				if (pixels)
				{
					const stbi_uc* in = &pixels[((size_t)(y * h / height) * w + (x * w / width)) * 4];
					out[0] = in[2], out[1] = in[1], out[2] = in[0], out[3] = 255;
				}
				else
				{
					const uint32_t bar = x * 8 / width;
					const uint8_t level = (uint8_t)(255 * y / std::max(height - 1, 1u));
					out[0] = (bar & 1) ? level : 32;
					out[1] = (bar & 2) ? level : 32;
					out[2] = (bar & 4) ? level : 32;
					out[3] = 255;
				}
			}
		if (pixels)
			stbi_image_free(pixels);
		return bgra;
	}

	int Run(const ps5reshade::Chain& compiled, VkExtent2D extent, const std::string& input, const std::string& output, int frames)
	{
		Gpu gpu;
		if (!StartGpu(gpu))
		{
			std::printf("no Vulkan device: nothing drawn\n");
			return 1;
		}
		std::printf("Vulkan: %s\n", gpu.name.c_str());
		const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM; // as Cemu's swapchain on VideoOut
		const VkImageLayout resting = gpu.swapchainLayouts ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

		// the picture, as a swapchain image: copied from and to
		VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = format;
		imageInfo.extent = {extent.width, extent.height, 1};
		imageInfo.mipLevels = imageInfo.arrayLayers = 1;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		VkImage picture = VK_NULL_HANDLE;
		vkCreateImage(gpu.device, &imageInfo, nullptr, &picture);
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(gpu.device, picture, &requirements);
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory pictureMemory = VK_NULL_HANDLE;
		vkAllocateMemory(gpu.device, &allocate, nullptr, &pictureMemory);
		vkBindImageMemory(gpu.device, picture, pictureMemory, 0);

		const size_t bytes = (size_t)extent.width * extent.height * 4;
		VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bufferInfo.size = bytes;
		bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		VkBuffer buffer = VK_NULL_HANDLE;
		vkCreateBuffer(gpu.device, &bufferInfo, nullptr, &buffer);
		vkGetBufferMemoryRequirements(gpu.device, buffer, &requirements);
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(gpu, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
		vkAllocateMemory(gpu.device, &allocate, nullptr, &bufferMemory);
		vkBindBufferMemory(gpu.device, buffer, bufferMemory, 0);
		void* mapped = nullptr;
		vkMapMemory(gpu.device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
		const std::vector<uint8_t> in = InputPicture(input, extent.width, extent.height);
		std::memcpy(mapped, in.data(), bytes);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {extent.width, extent.height, 1};
		Submit(gpu, [&](VkCommandBuffer cmd) {
			Barrier(cmd, picture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			vkCmdCopyBufferToImage(cmd, buffer, picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			Barrier(cmd, picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, resting);
		});

		std::string error;
		const auto made = std::chrono::steady_clock::now();
		auto runtime = ps5reshade::Runtime::Create(gpu.device, gpu.physicalDevice, compiled, format, extent, error);
		const double makeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - made).count();
		int status = 0;
		if (!runtime)
		{
			std::printf("the runtime was not made: %s\n", error.c_str());
			status = 1;
		}
		else
		{
			for (const std::string& message : runtime->GetChain().messages)
				std::printf("  %s\n", message.c_str());
			std::printf("made in %.0f ms: %s\n", makeMs, runtime->GetChain().Summary().c_str());
			for (int frame = 0; frame < frames; frame++)
			{
				const auto start = std::chrono::steady_clock::now();
				Submit(gpu, [&](VkCommandBuffer cmd) { runtime->Record(cmd, picture, resting); });
				std::printf("frame %d: %.1f ms on %s\n", frame, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(),
					gpu.name.c_str());
			}
			Submit(gpu, [&](VkCommandBuffer cmd) {
				Barrier(cmd, picture, resting, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				vkCmdCopyImageToBuffer(cmd, picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
				Barrier(cmd, picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, resting);
			});
			std::vector<uint8_t> rgba(bytes);
			const uint8_t* bgra = (const uint8_t*)mapped;
			size_t changed = 0;
			int largest = 0;
			for (size_t i = 0; i < bytes; i += 4)
			{
				rgba[i] = bgra[i + 2], rgba[i + 1] = bgra[i + 1], rgba[i + 2] = bgra[i], rgba[i + 3] = 255;
				changed += std::memcmp(&bgra[i], &in[i], 3) != 0;
				for (int c = 0; c < 3; c++)
					largest = std::max(largest, std::abs((int)bgra[i + c] - (int)in[i + c]));
			}
			std::printf("%.1f%% of the picture's pixels changed, by up to %d of 255\n", 100.0 * changed / (bytes / 4), largest);
			if (!output.empty())
			{
				if (stbi_write_png(output.c_str(), (int)extent.width, (int)extent.height, 4, rgba.data(), (int)extent.width * 4))
					std::printf("wrote %s\n", output.c_str());
				else
					std::printf("cannot write %s\n", output.c_str());
			}
		}
		vkDeviceWaitIdle(gpu.device);
		runtime.reset();
		vkUnmapMemory(gpu.device, bufferMemory);
		vkDestroyBuffer(gpu.device, buffer, nullptr);
		vkFreeMemory(gpu.device, bufferMemory, nullptr);
		vkDestroyImage(gpu.device, picture, nullptr);
		vkFreeMemory(gpu.device, pictureMemory, nullptr);
		vkDestroyCommandPool(gpu.device, gpu.pool, nullptr);
		vkDestroyDevice(gpu.device, nullptr);
		if (gpu.messenger)
			((PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(gpu.instance, "vkDestroyDebugUtilsMessengerEXT"))(gpu.instance, gpu.messenger, nullptr);
		vkDestroyInstance(gpu.instance, nullptr);
		if (s_validationErrors)
		{
			std::printf("%d validation errors\n", s_validationErrors);
			status = 1;
		}
		return status;
	}
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: reshade-check FOLDER [--title ID] [--size WxH] [--run IN.png|- OUT.png] [--frames N]\n");
		return 2;
	}
	const std::string folder = argv[1];
	uint64_t titleId = 0;
	VkExtent2D extent{3840, 2160};
	std::string input, output;
	bool run = false;
	int frames = 3;
	for (int i = 2; i < argc; i++)
	{
		const std::string arg = argv[i];
		if (arg == "--title" && i + 1 < argc)
			titleId = std::strtoull(argv[++i], nullptr, 16);
		else if (arg == "--size" && i + 1 < argc)
			std::sscanf(argv[++i], "%ux%u", &extent.width, &extent.height);
		else if (arg == "--run" && i + 2 < argc)
		{
			run = true;
			input = argv[++i];
			output = argv[++i];
		}
		else if (arg == "--frames" && i + 1 < argc)
			frames = std::atoi(argv[++i]);
		else
		{
			std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
			return 2;
		}
	}

	ps5reshade::Target target;
	target.width = extent.width;
	target.height = extent.height;
	ps5reshade::Chain chain;
	const auto start = std::chrono::steady_clock::now();
	ps5reshade::Build(folder, titleId, target, chain);
	const double compileMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::printf("preset: %s\n", chain.preset.empty() ? "(none)" : chain.preset.c_str());
	std::printf("compiled %zu effects in %.0f ms at %ux%u\n", chain.effects.size(), compileMs, extent.width, extent.height);
	for (const auto& effect : chain.effects)
	{
		size_t passes = 0, compute = 0, words = 0;
		for (const auto& technique : effect.module.techniques)
			for (const auto& pass : technique.passes)
			{
				passes++;
				compute += !pass.cs_entry_point.empty();
			}
		for (const auto& [name, code] : effect.code)
			words += code.size() / 4;
		std::printf("  %s: %zu techniques, %zu passes (%zu compute), %zu textures, %zu bytes of uniforms, %zu SPIR-V words\n", effect.name.c_str(),
			effect.module.techniques.size(), passes, compute, effect.module.textures.size(), effect.uniformData.size(), words);
	}
	for (const std::string& message : chain.messages)
		std::printf("  %s\n", message.c_str());
	std::printf("runs: %s\n", chain.Summary().c_str());

	int status = chain.techniques.empty() || !chain.messages.empty() ? 1 : 0;
	if (ValidateSpirv(chain))
		status = 1;
	if (run && Run(chain, extent, input, output, frames))
		status = 1;
	return status;
}
