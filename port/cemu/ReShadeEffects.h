// SPDX-License-Identifier: GPL-3.0-or-later
// PS5Cemu: ReShade's effects as far as the CPU takes them. ReShade's own compiler (.deps/reshade,
// BSD-3-Clause) turns the .fx files a preset uses into SPIR-V, one module per entry point; this
// finds the files, reads the preset (ReShade's ReShadePreset.ini, as the PC writes it), fills the
// uniforms with its values and loads the pictures textures name. PostEffectsPS5.cpp runs the
// result on Vulkan. Nothing here needs a GPU, so tools/reshade-check.sh runs it on a PC.
//
//   /data/ps5cemu/reshade/
//     ReShadePreset.ini           the preset every game uses
//     presets/<title ID>.ini      a game's own (00050000101C9500.ini: Breath of the Wild, EUR)
//     Shaders/, Textures/         .fx and .fxh files, and the pictures they load
//     reshade-shaders/            or the reshade-shaders folder from a PC, copied whole

#pragma once

#include "effect_module.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps5reshade
{
	namespace fs = std::filesystem;

	// What the effects are compiled for: BUFFER_WIDTH and the rest of ReShade's predefined macros
	struct Target
	{
		uint32_t width = 0, height = 0;
		uint32_t colorFormat = 87; // reshade::api::format: b8g8r8a8_unorm, VideoOut's
		uint32_t colorBitDepth = 8;
		uint32_t vendorId = 0x1002, deviceId = 0;
		uint32_t renderer = 0x21300; // Vulkan 1.3, as ReShade names it
	};

	// The ReShade preset: which techniques run in which order, and the values set for each file
	struct Preset
	{
		fs::path path;
		std::vector<std::string> techniques; // "Name@File.fx" (or "Name"), in the order they run
		std::vector<std::pair<std::string, std::string>> definitions; // PreprocessorDefinitions
		// [File.fx] sections: a uniform's name to its value as written ("0.5,0.25")
		std::map<std::string, std::map<std::string, std::string>> sections;
	};
	bool ReadPreset(const fs::path& file, Preset& preset);

	// A uniform ReShade fills itself each frame (its "source" annotation)
	enum class Special : uint8_t
	{
		None,
		FrameTime,
		FrameCount,
		Random,
		PingPong,
		Date,
		Timer,
		OverlayOpen,
		Unsupported, // keys, the mouse, a screenshot: none on a console; they read 0
	};

	struct Effect
	{
		std::string name; // the file's name, "SMAA.fx"
		reshadefx::effect_module module;
		std::unordered_map<std::string, std::string> code; // an entry point's SPIR-V, by its name
		std::vector<uint8_t> uniformData;					// what the uniform buffer holds
		std::vector<Special> specials;						// per module.uniforms
		// per module.textures: what one with a "source" annotation loads, in its format's texel
		// layout with rows packed; empty for the rest
		std::vector<std::vector<uint8_t>> pictures;
	};

	struct Chain
	{
		std::vector<Effect> effects;
		std::vector<std::pair<size_t, size_t>> techniques; // (effect, technique), in the order they run
		std::vector<std::string> messages;				   // what went wrong or was left out, a line each
		fs::path preset;
		std::string Summary() const; // "SMAA, LumaSharpen" or why nothing runs
	};

	// The folder the effects, textures and presets are looked for in (/data/ps5cemu/reshade)
	fs::path Root();
	// The preset for a game: its own in presets/, or ReShadePreset.ini; empty when there is neither
	fs::path PresetFor(const fs::path& root, uint64_t titleId);
	// Compiles what the preset turns on. The chain has no techniques when nothing could run.
	void Build(const fs::path& root, uint64_t titleId, const Target& target, Chain& chain);

	struct Frame
	{
		float frameTimeMs = 0;
		uint32_t frameCount = 0;
		uint32_t timerMs = 0;
		bool overlayOpen = false;
	};
	// The special uniforms' values for this frame, into effect.uniformData
	void UpdateSpecials(Effect& effect, const Frame& frame);

	// Bytes per texel of a texture format, 0 for one ReShade has no upload for
	uint32_t TexelBytes(reshadefx::texture_format format);
}
