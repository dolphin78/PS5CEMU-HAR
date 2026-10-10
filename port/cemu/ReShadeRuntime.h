// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: ReShade's effects on Vulkan. A Runtime holds the GPU's side of a ps5reshade::Chain (its
// textures, samplers, uniform buffers, render passes and pipelines) and records the chain's
// techniques on a picture in place, pass by pass as ReShade's own runtime does (runtime.cpp,
// render_technique): the picture is copied into a back buffer the passes draw to, and into the
// COLOR texture they sample before each pass that needs it, and the back buffer is copied back.
//
// It records nothing but what Record is called with, and creates its objects on whichever thread
// calls Create, so the compile and the pipelines can be made away from the renderer's thread. In
// Cemu it uses Cemu's Vulkan entry points (VulkanAPI.h); on a PC (tools/reshade-check) the loader's.

#pragma once

#if defined(CEMU_PS5)
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#else
#include <vulkan/vulkan.h>
#endif

#include "ReShadeEffects.h"

#include <chrono>
#include <memory>
#include <string>

namespace ps5reshade
{
	class Runtime
	{
	public:
		// The GPU's side of a chain, for pictures of this format and size. Effects whose objects cannot
		// be made are left out, with why in the chain's messages; null (and error) when none is left.
		static std::unique_ptr<Runtime> Create(VkDevice device, VkPhysicalDevice physicalDevice, Chain chain, VkFormat format,
			VkExtent2D extent, std::string& error);
		// The GPU must be done with every command buffer Record wrote into.
		~Runtime();

		// Records the techniques on image (format and extent as created), outside a render pass. The
		// image is in layout before and after, and is read and written with transfers.
		void Record(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, bool overlayOpen = false);

		VkFormat Format() const;
		VkExtent2D Extent() const;
		const Chain& GetChain() const;

		struct State;

	private:
		Runtime() = default;
		std::unique_ptr<State> m_state;
	};
}
