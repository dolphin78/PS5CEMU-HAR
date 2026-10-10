// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReShadeRuntime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace ps5reshade
{
	namespace
	{
		struct Image
		{
			VkImage image = VK_NULL_HANDLE;
			VkDeviceMemory memory = VK_NULL_HANDLE;
			VkFormat format = VK_FORMAT_UNDEFINED;	   // the views that read texels as they are
			VkFormat srgbFormat = VK_FORMAT_UNDEFINED; // the sRGB views', where the format has a twin
			VkImageType type = VK_IMAGE_TYPE_2D;
			VkExtent3D extent{1, 1, 1};
			uint32_t levels = 1;
			VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
			VkImageView view = VK_NULL_HANDLE, srgbView = VK_NULL_HANDLE;	  // every level, sampled
			VkImageView target = VK_NULL_HANDLE, srgbTarget = VK_NULL_HANDLE; // level 0, drawn to
			std::vector<VkImageView> storage;								  // a level each, for compute
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			bool integer = false;
			bool blits = false;							   // the format takes the blits mipmaps are made with
			bool linearBlits = false;					   // ... with linear filtering
			const std::vector<uint8_t>* picture = nullptr; // what it starts with (level 0); zeros without
			uint32_t texelBytes = 4;
		};

		struct Pass
		{
			bool compute = false;
			VkPipeline pipeline = VK_NULL_HANDLE;
			VkPipelineLayout layout = VK_NULL_HANDLE;
			VkDescriptorSetLayout textureLayout = VK_NULL_HANDLE, storageLayout = VK_NULL_HANDLE;
			VkDescriptorSet textureSet = VK_NULL_HANDLE, storageSet = VK_NULL_HANDLE;
			VkRenderPass renderPass = VK_NULL_HANDLE;
			VkFramebuffer framebuffer = VK_NULL_HANDLE;
			std::vector<Image*> targets; // the textures drawn to; empty for the back buffer
			uint32_t attachments = 0;
			std::vector<Image*> storages;
			std::vector<Image*> mipmaps; // made again from level 0 after the pass
			VkExtent2D viewport{};
			uint32_t vertices = 3;
			uint32_t groups[3] = {1, 1, 1};
		};

		struct EffectObjects
		{
			bool usable = false;
			VkBuffer uniforms = VK_NULL_HANDLE;
			VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
			VkDescriptorSetLayout uniformLayout = VK_NULL_HANDLE;
			VkDescriptorSet uniformSet = VK_NULL_HANDLE;
			VkDescriptorPool pool = VK_NULL_HANDLE;
			std::vector<std::unique_ptr<Image>> owned;
			std::vector<Image*> textures; // per module.textures: its own, the COLOR copy or the stand-in
			std::vector<VkSampler> samplers;
			std::unordered_map<size_t, std::vector<Pass>> techniques;
		};

		struct FormatInfo
		{
			VkFormat format = VK_FORMAT_UNDEFINED, srgb = VK_FORMAT_UNDEFINED;
			bool integer = false;
		};

		FormatInfo FormatOf(reshadefx::texture_format format)
		{
			using F = reshadefx::texture_format;
			switch (format)
			{
			case F::r8: return {VK_FORMAT_R8_UNORM};
			case F::r16f: return {VK_FORMAT_R16_SFLOAT};
			case F::r16: return {VK_FORMAT_R16_UNORM};
			case F::r32f: return {VK_FORMAT_R32_SFLOAT};
			case F::r32u: return {VK_FORMAT_R32_UINT, VK_FORMAT_UNDEFINED, true};
			case F::r32i: return {VK_FORMAT_R32_SINT, VK_FORMAT_UNDEFINED, true};
			case F::rg8: return {VK_FORMAT_R8G8_UNORM};
			case F::rg16f: return {VK_FORMAT_R16G16_SFLOAT};
			case F::rg16: return {VK_FORMAT_R16G16_UNORM};
			case F::rg32f: return {VK_FORMAT_R32G32_SFLOAT};
			case F::rgba8: return {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
			case F::rgba16f: return {VK_FORMAT_R16G16B16A16_SFLOAT};
			case F::rgba16: return {VK_FORMAT_R16G16B16A16_UNORM};
			case F::rgba32f: return {VK_FORMAT_R32G32B32A32_SFLOAT};
			case F::rgba32u: return {VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_UNDEFINED, true};
			case F::rgba32i: return {VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_UNDEFINED, true};
			case F::rgb10a2: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32};
			case F::rg11b10f: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32};
			default: return {};
			}
		}

		// The picture's format, as the views that do not convert and the sRGB ones read it
		FormatInfo PictureFormat(VkFormat format)
		{
			switch (format)
			{
			case VK_FORMAT_B8G8R8A8_UNORM:
			case VK_FORMAT_B8G8R8A8_SRGB: return {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
			case VK_FORMAT_R8G8B8A8_UNORM:
			case VK_FORMAT_R8G8B8A8_SRGB: return {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
			case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
			case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return {VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_A8B8G8R8_SRGB_PACK32};
			default: return {format};
			}
		}

		VkBlendFactor BlendFactor(reshadefx::blend_factor factor)
		{
			using B = reshadefx::blend_factor;
			switch (factor)
			{
			case B::zero: return VK_BLEND_FACTOR_ZERO;
			case B::one: return VK_BLEND_FACTOR_ONE;
			case B::source_color: return VK_BLEND_FACTOR_SRC_COLOR;
			case B::one_minus_source_color: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
			case B::dest_color: return VK_BLEND_FACTOR_DST_COLOR;
			case B::one_minus_dest_color: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
			case B::source_alpha: return VK_BLEND_FACTOR_SRC_ALPHA;
			case B::one_minus_source_alpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
			case B::dest_alpha: return VK_BLEND_FACTOR_DST_ALPHA;
			case B::one_minus_dest_alpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
			}
			return VK_BLEND_FACTOR_ONE;
		}

		VkBlendOp BlendOp(reshadefx::blend_op op)
		{
			using O = reshadefx::blend_op;
			switch (op)
			{
			case O::add: return VK_BLEND_OP_ADD;
			case O::subtract: return VK_BLEND_OP_SUBTRACT;
			case O::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
			case O::min: return VK_BLEND_OP_MIN;
			case O::max: return VK_BLEND_OP_MAX;
			}
			return VK_BLEND_OP_ADD;
		}

		VkStencilOp StencilOp(reshadefx::stencil_op op)
		{
			using S = reshadefx::stencil_op;
			switch (op)
			{
			case S::zero: return VK_STENCIL_OP_ZERO;
			case S::keep: return VK_STENCIL_OP_KEEP;
			case S::replace: return VK_STENCIL_OP_REPLACE;
			case S::increment_saturate: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
			case S::decrement_saturate: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
			case S::invert: return VK_STENCIL_OP_INVERT;
			case S::increment: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
			case S::decrement: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
			}
			return VK_STENCIL_OP_KEEP;
		}

		VkCompareOp CompareOp(reshadefx::stencil_func func)
		{
			using S = reshadefx::stencil_func;
			switch (func)
			{
			case S::never: return VK_COMPARE_OP_NEVER;
			case S::less: return VK_COMPARE_OP_LESS;
			case S::equal: return VK_COMPARE_OP_EQUAL;
			case S::less_equal: return VK_COMPARE_OP_LESS_OR_EQUAL;
			case S::greater: return VK_COMPARE_OP_GREATER;
			case S::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
			case S::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
			case S::always: return VK_COMPARE_OP_ALWAYS;
			}
			return VK_COMPARE_OP_ALWAYS;
		}

		VkPrimitiveTopology Topology(reshadefx::primitive_topology topology)
		{
			using T = reshadefx::primitive_topology;
			switch (topology)
			{
			case T::point_list: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
			case T::line_list: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
			case T::line_strip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
			case T::triangle_list: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			case T::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
			}
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		}

		VkSamplerAddressMode AddressMode(reshadefx::texture_address_mode mode)
		{
			using A = reshadefx::texture_address_mode;
			switch (mode)
			{
			case A::wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
			case A::mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			case A::clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			case A::border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			}
			return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		}

		// Every earlier command's writes before every later command's reads and writes. The passes
		// are few and large, so nothing finer pays.
		VkImageMemoryBarrier ImageBarrierInfo(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t baseLevel = 0,
			uint32_t levels = VK_REMAINING_MIP_LEVELS)
		{
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			barrier.oldLayout = from;
			barrier.newLayout = to;
			barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = image;
			barrier.subresourceRange = {aspect, baseLevel, levels, 0, 1};
			return barrier;
		}

		void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, uint32_t baseLevel = 0,
			uint32_t levels = VK_REMAINING_MIP_LEVELS)
		{
			const VkImageMemoryBarrier barrier = ImageBarrierInfo(image, aspect, from, to, baseLevel, levels);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
		}

		void FullBarrier(VkCommandBuffer cmd)
		{
			VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
			barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
			barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		void Transition(VkCommandBuffer cmd, Image& image, VkImageLayout to)
		{
			ImageBarrier(cmd, image.image, image.aspect, image.layout, to);
			image.layout = to;
		}
	}

	struct Runtime::State
	{
		VkDevice device = VK_NULL_HANDLE;
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkPhysicalDeviceMemoryProperties memory{};
		VkPhysicalDeviceProperties properties{};
		Chain chain;
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
		std::unique_ptr<Image> backbuffer, color, standIn, stencil;
		VkDescriptorSetLayout emptyLayout = VK_NULL_HANDLE;
		std::vector<EffectObjects> effects;
		std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging; // the pictures' uploads, kept until the end
		bool initialized = false;
		std::chrono::steady_clock::time_point start, last;
		uint32_t frameCount = 0;

		~State()
		{
			for (EffectObjects& effect : effects)
				DestroyEffect(effect);
			for (auto* image : {&backbuffer, &color, &standIn, &stencil})
				if (*image)
					DestroyImage(**image);
			for (auto& [buffer, memory] : staging)
			{
				vkDestroyBuffer(device, buffer, nullptr);
				vkFreeMemory(device, memory, nullptr);
			}
			if (emptyLayout)
				vkDestroyDescriptorSetLayout(device, emptyLayout, nullptr);
		}

		uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags wanted) const
		{
			for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
				if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
					return i;
			return UINT32_MAX;
		}

		VkFormatFeatureFlags Features(VkFormat format) const
		{
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
			return properties.optimalTilingFeatures;
		}

		VkImageView MakeView(const Image& image, VkFormat format, uint32_t baseLevel, uint32_t levels, VkImageUsageFlags usage)
		{
			VkImageViewUsageCreateInfo usageInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
			usageInfo.usage = usage;
			VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			info.pNext = usage ? &usageInfo : nullptr;
			info.image = image.image;
			info.viewType = image.type == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : image.type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
			info.format = format;
			info.subresourceRange = {image.aspect, baseLevel, levels, 0, 1};
			VkImageView view = VK_NULL_HANDLE;
			if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return view;
		}

		// An image and the views its use needs; false (and nothing kept) when it cannot be made
		bool MakeImage(Image& image, VkImageUsageFlags usage, std::string& error)
		{
			const bool mutableFormat = image.srgbFormat != VK_FORMAT_UNDEFINED;
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.flags = mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
			info.imageType = image.type;
			info.format = image.format;
			info.extent = image.extent;
			info.mipLevels = image.levels;
			info.arrayLayers = 1;
			info.samples = VK_SAMPLE_COUNT_1_BIT;
			info.tiling = VK_IMAGE_TILING_OPTIMAL;
			info.usage = usage;
			info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			if (vkCreateImage(device, &info, nullptr, &image.image) != VK_SUCCESS)
			{
				error = "cannot create a " + std::to_string(image.extent.width) + "x" + std::to_string(image.extent.height) + " image";
				return false;
			}
			VkMemoryRequirements requirements{};
			vkGetImageMemoryRequirements(device, image.image, &requirements);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = requirements.size;
			allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (allocate.memoryTypeIndex == UINT32_MAX)
				allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0);
			if (vkAllocateMemory(device, &allocate, nullptr, &image.memory) != VK_SUCCESS || vkBindImageMemory(device, image.image, image.memory, 0) != VK_SUCCESS)
			{
				error = "out of GPU memory for a " + std::to_string(image.extent.width) + "x" + std::to_string(image.extent.height) + " image";
				DestroyImage(image);
				return false;
			}
			const VkImageUsageFlags srgbUsage = usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
			bool made = true;
			if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
			{
				made &= (image.view = MakeView(image, image.format, 0, image.levels, 0)) != VK_NULL_HANDLE;
				if (mutableFormat)
					made &= (image.srgbView = MakeView(image, image.srgbFormat, 0, image.levels, srgbUsage)) != VK_NULL_HANDLE;
			}
			if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
			{
				made &= (image.target = MakeView(image, image.format, 0, 1, 0)) != VK_NULL_HANDLE;
				if (mutableFormat && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
					made &= (image.srgbTarget = MakeView(image, image.srgbFormat, 0, 1, srgbUsage)) != VK_NULL_HANDLE;
			}
			if (usage & VK_IMAGE_USAGE_STORAGE_BIT)
				for (uint32_t level = 0; level < image.levels; level++)
				{
					image.storage.push_back(MakeView(image, image.format, level, 1, 0));
					made &= image.storage.back() != VK_NULL_HANDLE;
				}
			if (!made)
			{
				error = "cannot create an image's views";
				DestroyImage(image);
				return false;
			}
			const VkFormatFeatureFlags features = Features(image.format);
			image.blits = (features & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (features & VK_FORMAT_FEATURE_BLIT_DST_BIT);
			image.linearBlits = image.blits && (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && !image.integer;
			return true;
		}

		void DestroyImage(Image& image)
		{
			for (VkImageView view : {image.view, image.srgbView, image.target, image.srgbTarget})
				if (view)
					vkDestroyImageView(device, view, nullptr);
			for (VkImageView view : image.storage)
				if (view)
					vkDestroyImageView(device, view, nullptr);
			if (image.image)
				vkDestroyImage(device, image.image, nullptr);
			if (image.memory)
				vkFreeMemory(device, image.memory, nullptr);
			image = Image{};
		}

		void DestroyPass(Pass& pass)
		{
			if (pass.pipeline)
				vkDestroyPipeline(device, pass.pipeline, nullptr);
			if (pass.layout)
				vkDestroyPipelineLayout(device, pass.layout, nullptr);
			for (VkDescriptorSetLayout layout : {pass.textureLayout, pass.storageLayout})
				if (layout && layout != emptyLayout)
					vkDestroyDescriptorSetLayout(device, layout, nullptr);
			if (pass.framebuffer)
				vkDestroyFramebuffer(device, pass.framebuffer, nullptr);
			if (pass.renderPass)
				vkDestroyRenderPass(device, pass.renderPass, nullptr);
			pass = Pass{};
		}

		void DestroyEffect(EffectObjects& effect)
		{
			for (auto& [index, passes] : effect.techniques)
				for (Pass& pass : passes)
					DestroyPass(pass);
			effect.techniques.clear();
			for (VkSampler sampler : effect.samplers)
				if (sampler)
					vkDestroySampler(device, sampler, nullptr);
			effect.samplers.clear();
			if (effect.pool)
				vkDestroyDescriptorPool(device, effect.pool, nullptr);
			if (effect.uniformLayout && effect.uniformLayout != emptyLayout)
				vkDestroyDescriptorSetLayout(device, effect.uniformLayout, nullptr);
			if (effect.uniforms)
				vkDestroyBuffer(device, effect.uniforms, nullptr);
			if (effect.uniformMemory)
				vkFreeMemory(device, effect.uniformMemory, nullptr);
			for (auto& image : effect.owned)
				DestroyImage(*image);
			effect = EffectObjects{};
		}

		VkDescriptorSetLayout MakeSetLayout(VkDescriptorType type, VkShaderStageFlags stages, const std::vector<uint32_t>& bindings)
		{
			if (bindings.empty())
				return emptyLayout;
			std::vector<VkDescriptorSetLayoutBinding> entries;
			for (uint32_t binding : bindings)
				entries.push_back({binding, type, 1, stages, nullptr});
			VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			info.bindingCount = (uint32_t)entries.size();
			info.pBindings = entries.data();
			VkDescriptorSetLayout layout = VK_NULL_HANDLE;
			if (vkCreateDescriptorSetLayout(device, &info, nullptr, &layout) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return layout;
		}

		VkDescriptorSet AllocateSet(VkDescriptorPool pool, VkDescriptorSetLayout layout)
		{
			VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			info.descriptorPool = pool;
			info.descriptorSetCount = 1;
			info.pSetLayouts = &layout;
			VkDescriptorSet set = VK_NULL_HANDLE;
			if (vkAllocateDescriptorSets(device, &info, &set) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return set;
		}

		VkShaderModule MakeModule(const std::string& code)
		{
			std::vector<uint32_t> words(code.size() / 4);
			std::memcpy(words.data(), code.data(), words.size() * 4);
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = words.size() * 4;
			info.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
				return VK_NULL_HANDLE;
			return module;
		}

		bool BuildEffect(size_t index, std::string& error);
		bool BuildPass(EffectObjects& objects, const Effect& effect, const reshadefx::pass& info, bool clearStencil, Pass& pass,
			std::unordered_map<std::string, VkShaderModule>& modules, std::string& error);

		void Initialize(VkCommandBuffer cmd);
		void Upload(VkCommandBuffer cmd, Image& image);
		void MakeMipmaps(VkCommandBuffer cmd, Image& image);
		void RecordPass(VkCommandBuffer cmd, const EffectObjects& effect, Pass& pass);
	};

	bool Runtime::State::BuildEffect(size_t index, std::string& error)
	{
		Effect& effect = chain.effects[index];
		EffectObjects& objects = effects[index];
		const reshadefx::effect_module& module = effect.module;

		// the techniques this effect runs
		std::vector<size_t> used;
		for (const auto& [e, t] : chain.techniques)
			if (e == index && std::find(used.begin(), used.end(), t) == used.end())
				used.push_back(t);
		if (used.empty())
			return true;

		// its textures: COLOR is the picture's copy, other semantics (DEPTH) read the stand-in
		objects.textures.resize(module.textures.size());
		for (size_t t = 0; t < module.textures.size(); t++)
		{
			const reshadefx::texture& texture = module.textures[t];
			if (texture.semantic == "COLOR")
			{
				objects.textures[t] = color.get();
				continue;
			}
			if (!texture.semantic.empty())
			{
				objects.textures[t] = standIn.get();
				continue;
			}
			const FormatInfo format = FormatOf(texture.format);
			if (format.format == VK_FORMAT_UNDEFINED)
			{
				error = "texture " + texture.name + " has a format this runtime does not know";
				return false;
			}
			auto image = std::make_unique<Image>();
			image->format = format.format;
			image->srgbFormat = format.srgb;
			image->integer = format.integer;
			image->type = texture.type == reshadefx::texture_type::texture_1d ? VK_IMAGE_TYPE_1D
				: texture.type == reshadefx::texture_type::texture_3d		   ? VK_IMAGE_TYPE_3D
																			   : VK_IMAGE_TYPE_2D;
			image->extent = {std::max(texture.width, 1u), std::max(texture.height, 1u), std::max<uint32_t>(texture.depth, 1)};
			image->levels = std::max<uint32_t>(texture.levels, 1);
			image->texelBytes = TexelBytes(texture.format);
			if (t < effect.pictures.size() && !effect.pictures[t].empty())
				image->picture = &effect.pictures[t];
			VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			if (image->levels > 1)
				usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			if (texture.render_target)
				usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (texture.storage_access)
				usage |= VK_IMAGE_USAGE_STORAGE_BIT;
			const VkFormatFeatureFlags formatFeatures = Features(image->format);
			VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
			if (texture.render_target)
				needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
			if (texture.storage_access)
				needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
			if ((formatFeatures & needed) != needed)
			{
				error = "the GPU cannot use texture " + texture.name + "'s format as the effect does";
				return false;
			}
			if (!MakeImage(*image, usage, error))
			{
				error = "texture " + texture.name + ": " + error;
				return false;
			}
			objects.textures[t] = image.get();
			objects.owned.push_back(std::move(image));
		}

		// its samplers, as the effect declares them
		for (const reshadefx::sampler& info : module.samplers)
		{
			const Image* image = nullptr;
			for (size_t t = 0; t < module.textures.size(); t++)
				if (module.textures[t].unique_name == info.texture_name)
					image = objects.textures[t];
			const bool integer = image && image->integer;
			const uint8_t filter = (uint8_t)info.filter;
			VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			sampler.magFilter = !integer && (filter & 0x04) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			sampler.minFilter = !integer && (filter & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
			sampler.mipmapMode = !integer && (filter & 0x01) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
			sampler.addressModeU = AddressMode(info.address_u);
			sampler.addressModeV = AddressMode(info.address_v);
			sampler.addressModeW = AddressMode(info.address_w);
			sampler.mipLodBias = std::clamp(info.lod_bias, -properties.limits.maxSamplerLodBias, properties.limits.maxSamplerLodBias);
			// anisotropic filtering is a device feature the renderer may not have turned on: linear
			sampler.minLod = std::max(info.min_lod, 0.0f);
			sampler.maxLod = std::max(sampler.minLod, std::min(info.max_lod, VK_LOD_CLAMP_NONE));
			sampler.borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			VkSampler handle = VK_NULL_HANDLE;
			if (vkCreateSampler(device, &sampler, nullptr, &handle) != VK_SUCCESS)
			{
				error = "cannot create sampler " + info.name;
				return false;
			}
			objects.samplers.push_back(handle);
		}

		// what the passes' descriptor sets take, to size the pool
		uint32_t sets = 1, samplersNeeded = 0, storagesNeeded = 0;
		for (size_t t : used)
			for (const reshadefx::pass& pass : module.techniques[t].passes)
			{
				sets += 2;
				samplersNeeded += (uint32_t)pass.texture_bindings.size();
				storagesNeeded += (uint32_t)pass.storage_bindings.size();
			}
		std::vector<VkDescriptorPoolSize> sizes;
		if (!effect.uniformData.empty())
			sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
		if (samplersNeeded)
			sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplersNeeded});
		if (storagesNeeded)
			sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storagesNeeded});
		if (sizes.empty())
			sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1});
		VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		poolInfo.maxSets = sets;
		poolInfo.poolSizeCount = (uint32_t)sizes.size();
		poolInfo.pPoolSizes = sizes.data();
		if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &objects.pool) != VK_SUCCESS)
		{
			error = "cannot create a descriptor pool";
			return false;
		}

		// the uniforms: a buffer the frame's values are written into before the passes
		const VkShaderStageFlags allStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
		if (!effect.uniformData.empty())
		{
			if (effect.uniformData.size() > properties.limits.maxUniformBufferRange)
			{
				error = "its uniforms take more than a uniform buffer holds";
				return false;
			}
			VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
			bufferInfo.size = effect.uniformData.size();
			bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			if (vkCreateBuffer(device, &bufferInfo, nullptr, &objects.uniforms) != VK_SUCCESS)
			{
				error = "cannot create its uniform buffer";
				return false;
			}
			VkMemoryRequirements requirements{};
			vkGetBufferMemoryRequirements(device, objects.uniforms, &requirements);
			VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
			allocate.allocationSize = requirements.size;
			allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			if (allocate.memoryTypeIndex == UINT32_MAX)
				allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, 0);
			if (vkAllocateMemory(device, &allocate, nullptr, &objects.uniformMemory) != VK_SUCCESS ||
				vkBindBufferMemory(device, objects.uniforms, objects.uniformMemory, 0) != VK_SUCCESS)
			{
				error = "out of GPU memory for its uniforms";
				return false;
			}
			objects.uniformLayout = MakeSetLayout(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, allStages, {0});
			if (!objects.uniformLayout || !(objects.uniformSet = AllocateSet(objects.pool, objects.uniformLayout)))
			{
				error = "cannot create its uniforms' descriptor set";
				return false;
			}
			VkDescriptorBufferInfo bufferDescriptor{objects.uniforms, 0, effect.uniformData.size()};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = objects.uniformSet;
			write.dstBinding = 0;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			write.pBufferInfo = &bufferDescriptor;
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}
		else
			objects.uniformLayout = emptyLayout;

		// the passes; the first in a technique to use the stencil clears it, as ReShade's does
		std::unordered_map<std::string, VkShaderModule> modules;
		bool built = true;
		for (size_t t : used)
		{
			std::vector<Pass>& passes = objects.techniques[t];
			bool stencilCleared = false;
			for (const reshadefx::pass& info : module.techniques[t].passes)
			{
				const bool usesStencil = stencil && info.stencil_enable && info.cs_entry_point.empty() && info.render_target_names[0].empty();
				passes.emplace_back();
				if (!BuildPass(objects, effect, info, usesStencil && !stencilCleared, passes.back(), modules, error))
				{
					error = module.techniques[t].name + ", pass " + (info.name.empty() ? std::to_string(passes.size() - 1) : info.name) + ": " + error;
					built = false;
					break;
				}
				stencilCleared |= usesStencil;
			}
			if (!built)
				break;
		}
		for (auto& [name, shader] : modules)
			if (shader)
				vkDestroyShaderModule(device, shader, nullptr);
		if (!built)
			return false;
		objects.usable = true;
		return true;
	}

	bool Runtime::State::BuildPass(EffectObjects& objects, const Effect& effect, const reshadefx::pass& info, bool clearStencil, Pass& pass,
		std::unordered_map<std::string, VkShaderModule>& modules, std::string& error)
	{
		const reshadefx::effect_module& module = effect.module;
		auto moduleFor = [&](const std::string& name) -> VkShaderModule {
			if (name.empty())
				return VK_NULL_HANDLE;
			if (auto found = modules.find(name); found != modules.end())
				return found->second;
			const auto code = effect.code.find(name);
			VkShaderModule made = code == effect.code.end() ? VK_NULL_HANDLE : MakeModule(code->second);
			modules[name] = made;
			return made;
		};
		auto textureIndex = [&](const std::string& uniqueName) -> int {
			for (size_t t = 0; t < module.textures.size(); t++)
				if (module.textures[t].unique_name == uniqueName || module.textures[t].name == uniqueName)
					return (int)t;
			return -1;
		};

		pass.compute = !info.cs_entry_point.empty();
		const VkShaderStageFlags stages = pass.compute ? VK_SHADER_STAGE_COMPUTE_BIT : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

		// set 1: the sampled textures, set 2: the storage ones, at the entry points' bindings
		std::vector<uint32_t> textureBindings, storageBindings;
		for (const auto& binding : info.texture_bindings)
			textureBindings.push_back(binding.entry_point_binding);
		for (const auto& binding : info.storage_bindings)
			storageBindings.push_back(binding.entry_point_binding);
		pass.textureLayout = MakeSetLayout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stages, textureBindings);
		pass.storageLayout = MakeSetLayout(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, stages, storageBindings);
		if (!pass.textureLayout || !pass.storageLayout)
		{
			error = "cannot create its descriptor set layouts";
			return false;
		}
		const VkDescriptorSetLayout setLayouts[3] = {objects.uniformLayout, pass.textureLayout, pass.storageLayout};
		VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		layoutInfo.setLayoutCount = 3;
		layoutInfo.pSetLayouts = setLayouts;
		if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pass.layout) != VK_SUCCESS)
		{
			error = "cannot create its pipeline layout";
			return false;
		}

		std::vector<VkDescriptorImageInfo> imageInfos;
		imageInfos.reserve(info.texture_bindings.size() + info.storage_bindings.size());
		std::vector<VkWriteDescriptorSet> writes;
		if (!textureBindings.empty())
		{
			if (!(pass.textureSet = AllocateSet(objects.pool, pass.textureLayout)))
			{
				error = "out of descriptor sets";
				return false;
			}
			for (const auto& binding : info.texture_bindings)
			{
				const reshadefx::sampler& sampler = module.samplers[binding.index];
				const int t = textureIndex(sampler.texture_name);
				const Image* image = t >= 0 ? objects.textures[t] : standIn.get();
				const VkImageView view = binding.srgb && image->srgbView ? image->srgbView : image->view;
				imageInfos.push_back({objects.samplers[binding.index], view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
				VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet = pass.textureSet;
				write.dstBinding = binding.entry_point_binding;
				write.descriptorCount = 1;
				write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				write.pImageInfo = &imageInfos.back();
				writes.push_back(write);
			}
		}
		if (!storageBindings.empty())
		{
			if (!(pass.storageSet = AllocateSet(objects.pool, pass.storageLayout)))
			{
				error = "out of descriptor sets";
				return false;
			}
			for (const auto& binding : info.storage_bindings)
			{
				const reshadefx::storage& storage = module.storages[binding.index];
				const int t = textureIndex(storage.texture_name);
				Image* image = t >= 0 ? objects.textures[t] : nullptr;
				if (!image || image->storage.empty() || storage.level >= image->storage.size())
				{
					error = "storage " + storage.name + " has no texture to write";
					return false;
				}
				imageInfos.push_back({VK_NULL_HANDLE, image->storage[storage.level], VK_IMAGE_LAYOUT_GENERAL});
				VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
				write.dstSet = pass.storageSet;
				write.dstBinding = binding.entry_point_binding;
				write.descriptorCount = 1;
				write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				write.pImageInfo = &imageInfos.back();
				writes.push_back(write);
				if (std::find(pass.storages.begin(), pass.storages.end(), image) == pass.storages.end())
				{
					pass.storages.push_back(image);
					if (info.generate_mipmaps && image->levels > 1 && image->blits)
						pass.mipmaps.push_back(image);
				}
			}
		}
		if (!writes.empty())
			vkUpdateDescriptorSets(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

		if (pass.compute)
		{
			const VkShaderModule shader = moduleFor(info.cs_entry_point);
			if (!shader)
			{
				error = "no code for " + info.cs_entry_point;
				return false;
			}
			VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
			pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, shader, info.cs_entry_point.c_str()};
			pipeline.layout = pass.layout;
			if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass.pipeline) != VK_SUCCESS)
			{
				error = "the GPU's compiler refused " + info.cs_entry_point;
				return false;
			}
			pass.groups[0] = std::max(info.viewport_width, 1u);
			pass.groups[1] = std::max(info.viewport_height, 1u);
			pass.groups[2] = std::max(info.viewport_dispatch_z, 1u);
			return true;
		}

		// what the pass draws to: textures, or the back buffer
		std::vector<VkFormat> formats;
		std::vector<VkImageView> views;
		std::vector<bool> integers;
		if (info.render_target_names[0].empty())
		{
			pass.viewport = extent;
			formats.push_back(info.srgb_write_enable && backbuffer->srgbFormat ? backbuffer->srgbFormat : backbuffer->format);
			views.push_back(info.srgb_write_enable && backbuffer->srgbTarget ? backbuffer->srgbTarget : backbuffer->target);
			integers.push_back(false);
		}
		else
		{
			for (int i = 0; i < 8 && !info.render_target_names[i].empty(); i++)
			{
				const int t = textureIndex(info.render_target_names[i]);
				Image* image = t >= 0 ? objects.textures[t] : nullptr;
				if (!image || !image->target)
				{
					error = "render target " + info.render_target_names[i] + " is not a texture it can draw to";
					return false;
				}
				const bool srgb = info.srgb_write_enable && image->srgbTarget;
				formats.push_back(srgb ? image->srgbFormat : image->format);
				views.push_back(srgb ? image->srgbTarget : image->target);
				integers.push_back(image->integer);
				pass.targets.push_back(image);
				if (info.generate_mipmaps && image->levels > 1 && image->blits && std::find(pass.mipmaps.begin(), pass.mipmaps.end(), image) == pass.mipmaps.end())
					pass.mipmaps.push_back(image);
				if (i == 0)
					pass.viewport = {image->extent.width, image->extent.height};
			}
		}
		if (info.viewport_width && info.viewport_height)
			pass.viewport = {std::min(info.viewport_width, pass.viewport.width), std::min(info.viewport_height, pass.viewport.height)};
		const bool useStencil = stencil && info.stencil_enable && info.render_target_names[0].empty();

		std::vector<VkAttachmentDescription> attachments;
		std::vector<VkAttachmentReference> colorReferences;
		for (size_t i = 0; i < formats.size(); i++)
		{
			VkAttachmentDescription attachment{};
			attachment.format = formats[i];
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = info.clear_render_targets ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			attachments.push_back(attachment);
			colorReferences.push_back({(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
		}
		VkAttachmentReference stencilReference{(uint32_t)attachments.size(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
		if (useStencil)
		{
			VkAttachmentDescription attachment{};
			attachment.format = stencil->format;
			attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.stencilLoadOp = clearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			attachments.push_back(attachment);
			views.push_back(stencil->target);
		}
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = (uint32_t)colorReferences.size();
		subpass.pColorAttachments = colorReferences.data();
		subpass.pDepthStencilAttachment = useStencil ? &stencilReference : nullptr;
		VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		renderPassInfo.attachmentCount = (uint32_t)attachments.size();
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &pass.renderPass) != VK_SUCCESS)
		{
			error = "cannot create its render pass";
			return false;
		}
		pass.attachments = (uint32_t)attachments.size();
		VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebufferInfo.renderPass = pass.renderPass;
		framebufferInfo.attachmentCount = (uint32_t)views.size();
		framebufferInfo.pAttachments = views.data();
		framebufferInfo.width = pass.viewport.width;
		framebufferInfo.height = pass.viewport.height;
		framebufferInfo.layers = 1;
		if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &pass.framebuffer) != VK_SUCCESS)
		{
			error = "cannot create its framebuffer";
			return false;
		}

		const VkShaderModule vertex = moduleFor(info.vs_entry_point);
		const VkShaderModule pixel = moduleFor(info.ps_entry_point);
		if (!vertex || (!info.ps_entry_point.empty() && !pixel))
		{
			error = "no code for " + (vertex ? info.ps_entry_point : info.vs_entry_point);
			return false;
		}
		VkPipelineShaderStageCreateInfo shaderStages[2] = {
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, info.vs_entry_point.c_str()},
			{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pixel, info.ps_entry_point.c_str()},
		};
		VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = Topology(info.topology);
		VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
		raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
		if (useStencil)
		{
			depthStencil.stencilTestEnable = VK_TRUE;
			VkStencilOpState& op = depthStencil.front;
			op.failOp = StencilOp(info.stencil_fail_op);
			op.passOp = StencilOp(info.stencil_pass_op);
			op.depthFailOp = StencilOp(info.stencil_depth_fail_op);
			op.compareOp = CompareOp(info.stencil_comparison_func);
			op.compareMask = info.stencil_read_mask;
			op.writeMask = info.stencil_write_mask;
			op.reference = info.stencil_reference_value;
			depthStencil.back = op;
		}
		std::vector<VkPipelineColorBlendAttachmentState> blends;
		for (size_t i = 0; i < colorReferences.size(); i++)
		{
			VkPipelineColorBlendAttachmentState blend{};
			blend.blendEnable = info.blend_enable[i] && !integers[i];
			blend.srcColorBlendFactor = BlendFactor(info.source_color_blend_factor[i]);
			blend.dstColorBlendFactor = BlendFactor(info.dest_color_blend_factor[i]);
			blend.colorBlendOp = BlendOp(info.color_blend_op[i]);
			blend.srcAlphaBlendFactor = BlendFactor(info.source_alpha_blend_factor[i]);
			blend.dstAlphaBlendFactor = BlendFactor(info.dest_alpha_blend_factor[i]);
			blend.alphaBlendOp = BlendOp(info.alpha_blend_op[i]);
			// ReShade's mask is RGBA in bits 0 to 3, as Vulkan's is
			blend.colorWriteMask = pixel ? (info.render_target_write_mask[i] & 0xF) : 0;
			blends.push_back(blend);
		}
		VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		blendState.attachmentCount = (uint32_t)blends.size();
		blendState.pAttachments = blends.data();
		const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
		dynamic.dynamicStateCount = 2;
		dynamic.pDynamicStates = dynamicStates;
		VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		pipeline.stageCount = pixel ? 2 : 1;
		pipeline.pStages = shaderStages;
		pipeline.pVertexInputState = &vertexInput;
		pipeline.pInputAssemblyState = &assembly;
		pipeline.pViewportState = &viewportState;
		pipeline.pRasterizationState = &raster;
		pipeline.pMultisampleState = &multisample;
		pipeline.pDepthStencilState = &depthStencil;
		pipeline.pColorBlendState = &blendState;
		pipeline.pDynamicState = &dynamic;
		pipeline.layout = pass.layout;
		pipeline.renderPass = pass.renderPass;
		if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pass.pipeline) != VK_SUCCESS)
		{
			error = "the GPU's compiler refused " + info.vs_entry_point + "/" + info.ps_entry_point;
			return false;
		}
		pass.vertices = info.num_vertices;
		return true;
	}

	void Runtime::State::Upload(VkCommandBuffer cmd, Image& image)
	{
		const std::vector<uint8_t>& picture = *image.picture;
		VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		info.size = picture.size();
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS)
			return;
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(device, buffer, &requirements);
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		void* mapped = nullptr;
		if (allocate.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &allocate, nullptr, &memory) != VK_SUCCESS)
		{
			vkDestroyBuffer(device, buffer, nullptr);
			return;
		}
		staging.emplace_back(buffer, memory);
		if (vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS || vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
			return;
		std::memcpy(mapped, picture.data(), picture.size());
		vkUnmapMemory(device, memory);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = image.extent;
		vkCmdCopyBufferToImage(cmd, buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	}

	// Levels 1 and on from level 0, each blitted from the one before; every level, in whatever
	// layout the image is in, ends readable by shaders
	void Runtime::State::MakeMipmaps(VkCommandBuffer cmd, Image& image)
	{
		const VkImageLayout from = image.layout;
		ImageBarrier(cmd, image.image, image.aspect, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);
		ImageBarrier(cmd, image.image, image.aspect, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, image.levels - 1);
		for (uint32_t level = 1; level < image.levels; level++)
		{
			auto size = [&](uint32_t value, uint32_t at) { return (int32_t)std::max(value >> at, 1u); };
			VkImageBlit blit{};
			blit.srcSubresource = {image.aspect, level - 1, 0, 1};
			blit.srcOffsets[1] = {size(image.extent.width, level - 1), size(image.extent.height, level - 1), size(image.extent.depth, level - 1)};
			blit.dstSubresource = {image.aspect, level, 0, 1};
			blit.dstOffsets[1] = {size(image.extent.width, level), size(image.extent.height, level), size(image.extent.depth, level)};
			vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
				image.linearBlits ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
			ImageBarrier(cmd, image.image, image.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level, 1);
		}
		image.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		Transition(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	}

	// The first frame: the textures' pictures or zeros, every one readable by shaders
	void Runtime::State::Initialize(VkCommandBuffer cmd)
	{
		std::vector<Image*> images = {color.get(), standIn.get()};
		for (EffectObjects& effect : effects)
			for (auto& image : effect.owned)
				images.push_back(image.get());
		const VkClearColorValue zero{};
		for (Image* image : images)
		{
			Transition(cmd, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
			vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
			if (image->picture)
			{
				FullBarrier(cmd);
				Upload(cmd, *image);
			}
			if (image->picture && image->levels > 1 && image->blits)
				MakeMipmaps(cmd, *image);
			else
				Transition(cmd, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		if (stencil)
			Transition(cmd, *stencil, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		initialized = true;
	}

	void Runtime::State::RecordPass(VkCommandBuffer cmd, const EffectObjects& effect, Pass& pass)
	{
		const VkPipelineBindPoint bindPoint = pass.compute ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
		if (pass.compute)
		{
			for (Image* image : pass.storages)
				Transition(cmd, *image, VK_IMAGE_LAYOUT_GENERAL);
		}
		else
		{
			if (pass.targets.empty())
				Transition(cmd, *backbuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			for (Image* image : pass.targets)
				Transition(cmd, *image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			std::array<VkClearValue, 9> clears{};
			VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			begin.renderPass = pass.renderPass;
			begin.framebuffer = pass.framebuffer;
			begin.renderArea = {{0, 0}, pass.viewport};
			begin.clearValueCount = pass.attachments;
			begin.pClearValues = clears.data();
			vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
		}
		vkCmdBindPipeline(cmd, bindPoint, pass.pipeline);
		if (effect.uniformSet)
			vkCmdBindDescriptorSets(cmd, bindPoint, pass.layout, 0, 1, &effect.uniformSet, 0, nullptr);
		if (pass.textureSet)
			vkCmdBindDescriptorSets(cmd, bindPoint, pass.layout, 1, 1, &pass.textureSet, 0, nullptr);
		if (pass.storageSet)
			vkCmdBindDescriptorSets(cmd, bindPoint, pass.layout, 2, 1, &pass.storageSet, 0, nullptr);
		if (pass.compute)
		{
			vkCmdDispatch(cmd, pass.groups[0], pass.groups[1], pass.groups[2]);
			for (Image* image : pass.storages)
				if (std::find(pass.mipmaps.begin(), pass.mipmaps.end(), image) == pass.mipmaps.end())
					Transition(cmd, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		else
		{
			// flipped, as ReShade's Vulkan backend flips every viewport: the effects are written for
			// Direct3D's clip space, whose +Y is the top (Vulkan 1.1's negative heights)
			const VkViewport viewport{0.0f, (float)pass.viewport.height, (float)pass.viewport.width, -(float)pass.viewport.height, 0.0f, 1.0f};
			const VkRect2D scissor{{0, 0}, pass.viewport};
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			vkCmdSetScissor(cmd, 0, 1, &scissor);
			vkCmdDraw(cmd, pass.vertices, 1, 0, 0);
			vkCmdEndRenderPass(cmd);
			for (Image* image : pass.targets)
				if (std::find(pass.mipmaps.begin(), pass.mipmaps.end(), image) == pass.mipmaps.end())
					Transition(cmd, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		for (Image* image : pass.mipmaps)
			MakeMipmaps(cmd, *image);
	}

	std::unique_ptr<Runtime> Runtime::Create(VkDevice device, VkPhysicalDevice physicalDevice, Chain chain, VkFormat format, VkExtent2D extent,
		std::string& error)
	{
		std::unique_ptr<Runtime> runtime(new Runtime());
		runtime->m_state = std::make_unique<State>();
		State& s = *runtime->m_state;
		s.device = device;
		s.physicalDevice = physicalDevice;
		s.chain = std::move(chain);
		s.format = format;
		s.extent = extent;
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &s.memory);
		vkGetPhysicalDeviceProperties(physicalDevice, &s.properties);
		if (s.chain.techniques.empty())
		{
			error = s.chain.Summary();
			return nullptr;
		}
		VkDescriptorSetLayoutCreateInfo emptyInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
		if (vkCreateDescriptorSetLayout(device, &emptyInfo, nullptr, &s.emptyLayout) != VK_SUCCESS)
		{
			error = "cannot create a descriptor set layout";
			return nullptr;
		}

		// the back buffer the passes draw to, and the COLOR texture they sample: the picture's format
		const FormatInfo pictureFormat = PictureFormat(format);
		const VkFormatFeatureFlags pictureFeatures = s.Features(pictureFormat.format);
		const VkFormatFeatureFlags srgbFeatures = pictureFormat.srgb ? s.Features(pictureFormat.srgb) : 0;
		auto pictureImage = [&](VkImageUsageFlags usage, std::unique_ptr<Image>& image) {
			image = std::make_unique<Image>();
			image->format = pictureFormat.format;
			// sRGB views only where the GPU takes them as the image is used
			const VkFormatFeatureFlags wanted = (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
			image->srgbFormat = pictureFormat.srgb && (srgbFeatures & wanted) == wanted ? pictureFormat.srgb : VK_FORMAT_UNDEFINED;
			image->extent = {extent.width, extent.height, 1};
			return s.MakeImage(*image, usage, error);
		};
		if ((pictureFeatures & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) !=
				(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
			!pictureImage(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s.backbuffer) ||
			!pictureImage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, s.color))
		{
			error = "the picture's copies: " + (error.empty() ? std::string("its format cannot be drawn to") : error);
			return nullptr;
		}
		// what DEPTH and other semantics the console has no source for read: a texel of zeros
		s.standIn = std::make_unique<Image>();
		s.standIn->format = VK_FORMAT_R8G8B8A8_UNORM;
		if (!s.MakeImage(*s.standIn, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, error))
			return nullptr;

		// the stencil buffer, only if a pass tests it
		bool needsStencil = false;
		for (const auto& [e, t] : s.chain.techniques)
			for (const reshadefx::pass& pass : s.chain.effects[e].module.techniques[t].passes)
				needsStencil |= pass.stencil_enable && pass.cs_entry_point.empty() && pass.render_target_names[0].empty();
		if (needsStencil)
		{
			for (VkFormat candidate : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_S8_UINT})
			{
				if (!(s.Features(candidate) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
					continue;
				auto image = std::make_unique<Image>();
				image->format = candidate;
				image->aspect = candidate == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
				image->extent = {extent.width, extent.height, 1};
				std::string stencilError;
				if (s.MakeImage(*image, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, stencilError))
				{
					s.stencil = std::move(image);
					break;
				}
			}
			if (!s.stencil)
				s.chain.messages.push_back("no stencil buffer: passes that test the stencil run without the test");
		}

		// each effect; one that cannot be made is left out
		s.effects.resize(s.chain.effects.size());
		for (size_t e = 0; e < s.chain.effects.size(); e++)
		{
			std::string effectError;
			if (!s.BuildEffect(e, effectError))
			{
				s.chain.messages.push_back(s.chain.effects[e].name + ": " + effectError);
				s.DestroyEffect(s.effects[e]);
			}
		}
		auto& techniques = s.chain.techniques;
		techniques.erase(std::remove_if(techniques.begin(), techniques.end(), [&](const auto& entry) { return !s.effects[entry.first].usable; }),
			techniques.end());
		if (techniques.empty())
		{
			error = s.chain.messages.empty() ? "no technique could be made" : s.chain.messages.back();
			return nullptr;
		}
		s.start = s.last = std::chrono::steady_clock::now();
		return runtime;
	}

	Runtime::~Runtime() = default;

	VkFormat Runtime::Format() const
	{
		return m_state->format;
	}

	VkExtent2D Runtime::Extent() const
	{
		return m_state->extent;
	}

	const Chain& Runtime::GetChain() const
	{
		return m_state->chain;
	}

	void Runtime::Record(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, bool overlayOpen)
	{
		State& s = *m_state;
		if (!s.initialized)
			s.Initialize(cmd);

		// this frame's uniforms, written before any pass reads them
		const auto now = std::chrono::steady_clock::now();
		Frame frame;
		frame.frameTimeMs = std::chrono::duration<float, std::milli>(now - s.last).count();
		frame.timerMs = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - s.start).count();
		frame.frameCount = s.frameCount++;
		frame.overlayOpen = overlayOpen;
		s.last = now;
		FullBarrier(cmd);
		for (size_t e = 0; e < s.chain.effects.size(); e++)
		{
			EffectObjects& objects = s.effects[e];
			Effect& effect = s.chain.effects[e];
			if (!objects.usable || !objects.uniforms)
				continue;
			UpdateSpecials(effect, frame);
			for (size_t offset = 0; offset < effect.uniformData.size(); offset += 65536)
			{
				const size_t size = std::min<size_t>(65536, effect.uniformData.size() - offset);
				vkCmdUpdateBuffer(cmd, objects.uniforms, offset, size, effect.uniformData.data() + offset);
			}
		}
		FullBarrier(cmd);

		// the picture into the back buffer
		ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		s.backbuffer->layout = VK_IMAGE_LAYOUT_UNDEFINED; // all of it is written
		Transition(cmd, *s.backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy whole{};
		whole.srcSubresource = whole.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		whole.extent = {s.extent.width, s.extent.height, 1};
		vkCmdCopyImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.backbuffer->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);

		for (const auto& [e, t] : s.chain.techniques)
		{
			EffectObjects& objects = s.effects[e];
			auto passes = objects.techniques.find(t);
			if (passes == objects.techniques.end())
				continue;
			bool copyBackbuffer = true; // the first pass sees the picture as the technique starts
			for (Pass& pass : passes->second)
			{
				if (copyBackbuffer)
				{
					Transition(cmd, *s.backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					Transition(cmd, *s.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
					vkCmdCopyImage(cmd, s.backbuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.color->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
					Transition(cmd, *s.color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				}
				s.RecordPass(cmd, objects, pass);
				// a pass that drew to the back buffer leaves the next one a picture to copy
				copyBackbuffer = !pass.compute && pass.targets.empty();
			}
		}

		// the back buffer back into the picture
		Transition(cmd, *s.backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyImage(cmd, s.backbuffer->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
		ImageBarrier(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layout);
	}
}
