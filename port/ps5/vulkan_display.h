// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: Vulkan on the PS5's RADV, for Cemu's renderer.
//
// The driver (Mesa's RADV with a PS5 winsys, PS5_Mesa) is linked into the title, so its
// vk_icdGetInstanceProcAddr stands in for a loader. The screen is VideoOut, which the driver
// exposes as VK_KHR_display: one display, one plane and its modes (display.h), of which the
// surface takes the 3840x2160 one at 59.94 Hz, or at 119.88 Hz with the 120 Hz setting on. Swapchains
// on it are the mode's size.

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <string>

namespace ps5vk
{
	// The driver's vkGetInstanceProcAddr.
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr();

	// Instance extensions a display surface needs.
	constexpr const char* kSurfaceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME};

	// A VK_KHR_display plane surface on VideoOut at the configured output size. Returns
	// VK_NULL_HANDLE, with the reason in error, when it cannot.
	VkSurfaceKHR CreateDisplaySurface(VkInstance instance, std::string& error);
}
