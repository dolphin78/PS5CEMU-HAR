// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: the Vulkan functions it calls, loaded through the driver's
// vkGetInstanceProcAddr (on the console RADV is linked into the app, with no loader: vulkan_display.h;
// the PC preview passes the loader's). One table per process; the kit has one device at a time.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#define UI_VK_INSTANCE_FUNCTIONS(X)                                                                                    \
	X(vkDestroyInstance)                                                                                               \
	X(vkEnumeratePhysicalDevices)                                                                                      \
	X(vkGetPhysicalDeviceProperties)                                                                                   \
	X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
	X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
	X(vkGetPhysicalDeviceFormatProperties)                                                                             \
	X(vkCreateDevice)                                                                                                  \
	X(vkGetDeviceProcAddr)                                                                                             \
	X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
	X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
	X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
	X(vkDestroySurfaceKHR)

#define UI_VK_DEVICE_FUNCTIONS(X)                                                                                      \
	X(vkDestroyDevice)                                                                                                 \
	X(vkGetDeviceQueue)                                                                                                \
	X(vkDeviceWaitIdle)                                                                                                \
	X(vkQueueSubmit)                                                                                                   \
	X(vkQueueWaitIdle)                                                                                                 \
	X(vkCreateSwapchainKHR)                                                                                            \
	X(vkDestroySwapchainKHR)                                                                                           \
	X(vkGetSwapchainImagesKHR)                                                                                         \
	X(vkAcquireNextImageKHR)                                                                                           \
	X(vkQueuePresentKHR)                                                                                               \
	X(vkCreateCommandPool)                                                                                             \
	X(vkDestroyCommandPool)                                                                                            \
	X(vkAllocateCommandBuffers)                                                                                        \
	X(vkResetCommandBuffer)                                                                                            \
	X(vkBeginCommandBuffer)                                                                                            \
	X(vkEndCommandBuffer)                                                                                              \
	X(vkCreateFence)                                                                                                   \
	X(vkDestroyFence)                                                                                                  \
	X(vkWaitForFences)                                                                                                 \
	X(vkResetFences)                                                                                                   \
	X(vkCreateSemaphore)                                                                                               \
	X(vkDestroySemaphore)                                                                                              \
	X(vkCreateRenderPass)                                                                                              \
	X(vkDestroyRenderPass)                                                                                             \
	X(vkCreateFramebuffer)                                                                                             \
	X(vkDestroyFramebuffer)                                                                                            \
	X(vkCreateImageView)                                                                                               \
	X(vkDestroyImageView)                                                                                              \
	X(vkCreateImage)                                                                                                   \
	X(vkDestroyImage)                                                                                                  \
	X(vkGetImageMemoryRequirements)                                                                                    \
	X(vkBindImageMemory)                                                                                               \
	X(vkCreateBuffer)                                                                                                  \
	X(vkDestroyBuffer)                                                                                                 \
	X(vkGetBufferMemoryRequirements)                                                                                   \
	X(vkBindBufferMemory)                                                                                              \
	X(vkAllocateMemory)                                                                                                \
	X(vkFreeMemory)                                                                                                    \
	X(vkMapMemory)                                                                                                     \
	X(vkUnmapMemory)                                                                                                   \
	X(vkFlushMappedMemoryRanges)                                                                                       \
	X(vkInvalidateMappedMemoryRanges)                                                                                  \
	X(vkCreateShaderModule)                                                                                            \
	X(vkDestroyShaderModule)                                                                                           \
	X(vkCreatePipelineLayout)                                                                                          \
	X(vkDestroyPipelineLayout)                                                                                         \
	X(vkCreateGraphicsPipelines)                                                                                       \
	X(vkDestroyPipeline)                                                                                               \
	X(vkCreatePipelineCache)                                                                                           \
	X(vkDestroyPipelineCache)                                                                                          \
	X(vkGetPipelineCacheData)                                                                                          \
	X(vkCreateDescriptorSetLayout)                                                                                     \
	X(vkDestroyDescriptorSetLayout)                                                                                    \
	X(vkCreateDescriptorPool)                                                                                          \
	X(vkDestroyDescriptorPool)                                                                                         \
	X(vkAllocateDescriptorSets)                                                                                        \
	X(vkFreeDescriptorSets)                                                                                            \
	X(vkUpdateDescriptorSets)                                                                                          \
	X(vkCreateSampler)                                                                                                 \
	X(vkDestroySampler)                                                                                                \
	X(vkCmdBeginRenderPass)                                                                                            \
	X(vkCmdEndRenderPass)                                                                                              \
	X(vkCmdBindPipeline)                                                                                               \
	X(vkCmdBindDescriptorSets)                                                                                         \
	X(vkCmdBindVertexBuffers)                                                                                          \
	X(vkCmdDraw)                                                                                                       \
	X(vkCmdSetViewport)                                                                                                \
	X(vkCmdSetScissor)                                                                                                 \
	X(vkCmdPushConstants)                                                                                              \
	X(vkCmdPipelineBarrier)                                                                                            \
	X(vkCmdCopyBufferToImage)                                                                                          \
	X(vkCmdCopyImageToBuffer)

namespace ui::vk
{
#define UI_VK_DECLARE(name) extern PFN_##name name;
	extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	extern PFN_vkCreateInstance vkCreateInstance;
	UI_VK_INSTANCE_FUNCTIONS(UI_VK_DECLARE)
	UI_VK_DEVICE_FUNCTIONS(UI_VK_DECLARE)
#undef UI_VK_DECLARE

	// The global and instance functions from gipa; false when one the kit needs is missing.
	bool LoadGlobal(PFN_vkGetInstanceProcAddr gipa);
	bool LoadInstance(VkInstance instance, bool surface);
	bool LoadDevice(VkDevice device);
	// The name of the first function that could not be loaded, for the log.
	const char* Missing();
}
