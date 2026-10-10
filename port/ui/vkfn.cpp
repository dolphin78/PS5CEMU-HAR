// SPDX-License-Identifier: GPL-3.0-or-later
#include "vkfn.h"

#include <cstring>

namespace ui::vk
{
#define UI_VK_DEFINE(name) PFN_##name name = nullptr;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkCreateInstance vkCreateInstance = nullptr;
	UI_VK_INSTANCE_FUNCTIONS(UI_VK_DEFINE)
	UI_VK_DEVICE_FUNCTIONS(UI_VK_DEFINE)
#undef UI_VK_DEFINE

	namespace
	{
		const char* s_missing = "";

		bool IsSurfaceFunction(const char* name)
		{
			return std::strstr(name, "Surface") != nullptr;
		}
	}

	bool LoadGlobal(PFN_vkGetInstanceProcAddr gipa)
	{
		vkGetInstanceProcAddr = gipa;
		vkCreateInstance = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance")) : nullptr;
		if (!vkCreateInstance)
		{
			s_missing = "vkCreateInstance";
			return false;
		}
		return true;
	}

	bool LoadInstance(VkInstance instance, bool surface)
	{
		bool ok = true;
#define UI_VK_LOAD(name)                                                                                               \
	name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));                                       \
	if (!name && (surface || !IsSurfaceFunction(#name)) && ok)                                                         \
	{                                                                                                                  \
		s_missing = #name;                                                                                             \
		ok = false;                                                                                                    \
	}
		UI_VK_INSTANCE_FUNCTIONS(UI_VK_LOAD)
#undef UI_VK_LOAD
		return ok;
	}

	bool LoadDevice(VkDevice device)
	{
		bool ok = true;
#define UI_VK_LOAD(name)                                                                                               \
	name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                                           \
	if (!name && std::strstr(#name, "KHR") == nullptr && ok)                                                           \
	{                                                                                                                  \
		s_missing = #name;                                                                                             \
		ok = false;                                                                                                    \
	}
		UI_VK_DEVICE_FUNCTIONS(UI_VK_LOAD)
#undef UI_VK_LOAD
		return ok;
	}

	const char* Missing()
	{
		return s_missing;
	}
}
