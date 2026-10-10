// SPDX-License-Identifier: GPL-3.0-or-later
#include "ReShadeEffects.h"

#include "effect_codegen.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"

// stb's loaders as ReShade builds them (deps/stb_impl.c), static to this file so they meet no other
// copy in the app
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_DDS_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_dds.h"
#include "stb_image_resize2.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>

#ifndef PS5CEMU_DATA
#define PS5CEMU_DATA "/data/ps5cemu"
#endif

namespace ps5reshade
{
	namespace
	{
		std::string_view Trim(std::string_view text, const char* characters = " \t\r\n")
		{
			const size_t first = text.find_first_not_of(characters);
			if (first == std::string_view::npos)
				return {};
			return text.substr(first, text.find_last_not_of(characters) - first + 1);
		}

		bool EqualNoCase(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
				return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
			});
		}

		// A value as ReShade's ini_file splits it: on single commas, ",," being a comma of its own
		std::vector<std::string> SplitValue(std::string_view value)
		{
			std::vector<std::string> elements;
			std::string element;
			for (size_t i = 0; i <= value.size(); i++)
			{
				if (i == value.size())
				{
					elements.push_back(std::string(Trim(element)));
					break;
				}
				if (value[i] == ',' && i + 1 < value.size() && value[i + 1] == ',')
				{
					element += ',';
					i++;
					continue;
				}
				if (value[i] == ',')
				{
					elements.push_back(std::string(Trim(element)));
					element.clear();
					continue;
				}
				element += value[i];
			}
			return elements;
		}

		std::pair<std::string, std::string> Definition(std::string_view text)
		{
			const size_t equals = text.find('=');
			if (equals == std::string_view::npos)
				return {std::string(Trim(text)), "1"};
			std::string value(Trim(text.substr(equals + 1)));
			return {std::string(Trim(text.substr(0, equals))), value.empty() ? "1" : value};
		}

		const reshadefx::annotation* FindAnnotation(const std::vector<reshadefx::annotation>& annotations, std::string_view name)
		{
			for (const auto& annotation : annotations)
				if (annotation.name == name)
					return &annotation;
			return nullptr;
		}

		std::string AnnotationString(const std::vector<reshadefx::annotation>& annotations, std::string_view name)
		{
			const auto* annotation = FindAnnotation(annotations, name);
			return annotation && annotation->type.base == reshadefx::type::t_string ? annotation->value.string_data : std::string();
		}

		float AnnotationFloat(const std::vector<reshadefx::annotation>& annotations, std::string_view name, size_t index, float fallback)
		{
			const auto* annotation = FindAnnotation(annotations, name);
			if (!annotation || index >= 16)
				return fallback;
			if (annotation->type.is_floating_point())
				return annotation->value.as_float[index];
			if (annotation->type.is_signed())
				return (float)annotation->value.as_int[index];
			return (float)annotation->value.as_uint[index];
		}

		int AnnotationInt(const std::vector<reshadefx::annotation>& annotations, std::string_view name, size_t index, int fallback)
		{
			const auto* annotation = FindAnnotation(annotations, name);
			if (!annotation || index >= 16)
				return fallback;
			if (annotation->type.is_floating_point())
				return (int)annotation->value.as_float[index];
			return annotation->value.as_int[index];
		}

		// ReShade's uniform layout (runtime.cpp, set_uniform_value_data): a matrix's rows and an
		// array's elements each start on 16 bytes; anything else is packed.
		void WriteUniform(Effect& effect, const reshadefx::uniform& uniform, const uint32_t* words, size_t count, size_t arrayIndex = 0)
		{
			std::vector<uint8_t>& data = effect.uniformData;
			const auto& type = uniform.type;
			const size_t arrayLength = type.is_array() ? type.array_length : 1;
			if (arrayIndex >= arrayLength || uniform.offset >= data.size())
				return;
			count = std::min<size_t>(count, uniform.size / 4);
			auto store = [&](size_t byteOffset, uint32_t word) {
				const size_t at = uniform.offset + byteOffset;
				if (at + 4 <= data.size() && byteOffset + 4 <= uniform.size)
					std::memcpy(data.data() + at, &word, 4);
			};
			if (type.is_matrix())
			{
				for (size_t a = arrayIndex, i = 0; a < arrayLength; a++)
					for (size_t row = 0; row < type.rows; row++)
						for (size_t col = 0; i < count && col < type.cols; col++, i++)
							store((a * type.rows * 4 + row * 4 + col) * 4, words[i]);
			}
			else if (arrayLength > 1)
			{
				for (size_t a = arrayIndex, i = 0; a < arrayLength; a++)
					for (size_t row = 0; i < count && row < type.rows; row++, i++)
						store((a * 4 + row) * 4, words[i]);
			}
			else
			{
				for (size_t i = 0; i < count; i++)
					store(i * 4, words[i]);
			}
		}

		// Values as the uniform's own type holds them (ReShade's typed setters)
		void WriteFloats(Effect& effect, const reshadefx::uniform& uniform, const float* values, size_t count, size_t arrayIndex = 0)
		{
			std::vector<uint32_t> words(count);
			for (size_t i = 0; i < count; i++)
			{
				if (uniform.type.is_floating_point())
					std::memcpy(&words[i], &values[i], 4);
				else if (uniform.type.is_boolean())
					words[i] = values[i] != 0.0f;
				else
					words[i] = (uint32_t)(int32_t)values[i];
			}
			WriteUniform(effect, uniform, words.data(), count, arrayIndex);
		}

		void WriteInts(Effect& effect, const reshadefx::uniform& uniform, const int32_t* values, size_t count, size_t arrayIndex = 0)
		{
			std::vector<uint32_t> words(count);
			for (size_t i = 0; i < count; i++)
			{
				if (uniform.type.is_floating_point())
				{
					const float value = (float)values[i];
					std::memcpy(&words[i], &value, 4);
				}
				else if (uniform.type.is_boolean())
					words[i] = values[i] != 0;
				else
					words[i] = (uint32_t)values[i];
			}
			WriteUniform(effect, uniform, words.data(), count, arrayIndex);
		}

		void WriteUints(Effect& effect, const reshadefx::uniform& uniform, const uint32_t* values, size_t count, size_t arrayIndex = 0)
		{
			std::vector<uint32_t> words(count);
			for (size_t i = 0; i < count; i++)
			{
				if (uniform.type.is_floating_point())
				{
					const float value = (float)values[i];
					std::memcpy(&words[i], &value, 4);
				}
				else if (uniform.type.is_boolean())
					words[i] = values[i] != 0;
				else
					words[i] = values[i];
			}
			WriteUniform(effect, uniform, words.data(), count, arrayIndex);
		}

		float ReadFloat(const Effect& effect, const reshadefx::uniform& uniform, size_t component)
		{
			const size_t at = uniform.offset + component * 4;
			if (at + 4 > effect.uniformData.size())
				return 0;
			uint32_t word;
			std::memcpy(&word, effect.uniformData.data() + at, 4);
			if (uniform.type.is_floating_point())
			{
				float value;
				std::memcpy(&value, &word, 4);
				return value;
			}
			return uniform.type.is_signed() ? (float)(int32_t)word : (float)word;
		}

		// A uniform's initial value (ReShade's reset_uniform_value)
		void ResetUniform(Effect& effect, const reshadefx::uniform& uniform, Special special)
		{
			if (special != Special::None)
			{
				const size_t end = std::min<size_t>(uniform.offset + uniform.size, effect.uniformData.size());
				if (uniform.offset < end)
					std::memset(effect.uniformData.data() + uniform.offset, 0, end - uniform.offset);
				return;
			}
			const reshadefx::constant zero{};
			const size_t arrayLength = uniform.type.is_array() ? uniform.type.array_length : 1;
			for (size_t i = 0; i < arrayLength; i++)
			{
				const reshadefx::constant& value = !uniform.has_initializer_value ? zero
					: uniform.type.is_array() ? (i < uniform.initializer_value.array_data.size() ? uniform.initializer_value.array_data[i] : zero)
											  : uniform.initializer_value;
				const size_t components = uniform.type.components();
				switch (uniform.type.base)
				{
				case reshadefx::type::t_int:
				case reshadefx::type::t_min16int:
					WriteInts(effect, uniform, value.as_int, components, i);
					break;
				case reshadefx::type::t_bool:
				case reshadefx::type::t_uint:
				case reshadefx::type::t_min16uint:
					WriteUints(effect, uniform, value.as_uint, components, i);
					break;
				default:
					WriteFloats(effect, uniform, value.as_float, components, i);
					break;
				}
			}
		}

		// A value from the preset ("0.5,0.25"), read as the uniform's type
		void ApplyPresetValue(Effect& effect, const reshadefx::uniform& uniform, const std::string& text)
		{
			const std::vector<std::string> parts = SplitValue(text);
			const size_t count = std::min<size_t>(parts.size(), 16 * std::max<size_t>(1, uniform.type.is_array() ? uniform.type.array_length : 1));
			if (uniform.type.is_floating_point())
			{
				std::vector<float> values(count);
				for (size_t i = 0; i < count; i++)
					values[i] = std::strtof(parts[i].c_str(), nullptr);
				WriteFloats(effect, uniform, values.data(), count);
			}
			else if (uniform.type.is_signed())
			{
				std::vector<int32_t> values(count);
				for (size_t i = 0; i < count; i++)
					values[i] = (int32_t)std::strtol(parts[i].c_str(), nullptr, 0);
				WriteInts(effect, uniform, values.data(), count);
			}
			else
			{
				std::vector<uint32_t> values(count);
				for (size_t i = 0; i < count; i++)
				{
					const std::string& part = parts[i];
					values[i] = EqualNoCase(part, "true") ? 1u : EqualNoCase(part, "false") ? 0u : (uint32_t)std::strtoul(part.c_str(), nullptr, 0);
				}
				WriteUints(effect, uniform, values.data(), count);
			}
		}

		Special SpecialOf(const std::string& source)
		{
			if (source.empty())
				return Special::None;
			if (source == "frametime")
				return Special::FrameTime;
			if (source == "framecount")
				return Special::FrameCount;
			if (source == "random")
				return Special::Random;
			if (source == "pingpong")
				return Special::PingPong;
			if (source == "date")
				return Special::Date;
			if (source == "timer")
				return Special::Timer;
			if (source == "ui_open" || source == "overlay_open")
				return Special::OverlayOpen;
			return Special::Unsupported;
		}

		// Every folder below the ones given, which the effects' #includes are looked for in
		std::vector<fs::path> FoldersBelow(const std::vector<fs::path>& tops)
		{
			std::vector<fs::path> folders;
			std::error_code ec;
			for (const fs::path& top : tops)
			{
				if (!fs::is_directory(top, ec))
					continue;
				folders.push_back(top);
				for (auto it = fs::recursive_directory_iterator(top, fs::directory_options::skip_permission_denied, ec);
					 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
				{
					if (it->is_directory(ec))
						folders.push_back(it->path());
				}
			}
			return folders;
		}

		fs::path FindFile(const std::vector<fs::path>& folders, const fs::path& name)
		{
			std::error_code ec;
			for (const fs::path& folder : folders)
			{
				const fs::path candidate = folder / name;
				if (fs::is_regular_file(candidate, ec))
					return candidate;
			}
			// names in presets from Windows can differ in case
			const std::string wanted = name.filename().string();
			for (const fs::path& folder : folders)
			{
				for (const auto& entry : fs::directory_iterator(folder, ec))
				{
					if (entry.is_regular_file(ec) && EqualNoCase(entry.path().filename().string(), wanted))
						return entry.path();
				}
			}
			return {};
		}

		bool ReadWholeFile(const fs::path& path, std::vector<uint8_t>& data)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file)
				return false;
			data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
			return true;
		}

		// A texture's picture from its file, as ReShade loads it (runtime.cpp, load_textures)
		bool LoadPicture(const reshadefx::texture& texture, const fs::path& path, std::vector<uint8_t>& out, std::string& error)
		{
			if (texture.type != reshadefx::texture_type::texture_2d)
			{
				error = "only 2D textures load from files here";
				return false;
			}
			std::vector<uint8_t> file;
			if (!ReadWholeFile(path, file) || file.empty())
			{
				error = "cannot read it";
				return false;
			}
			const bool floating = texture.format == reshadefx::texture_format::r32f || texture.format == reshadefx::texture_format::rg32f ||
				texture.format == reshadefx::texture_format::rgba32f;
			int width = 0, height = 0, depth = 1, channels = 0;
			void* pixels = nullptr;
			if (floating)
				pixels = stbi_loadf_from_memory(file.data(), (int)file.size(), &width, &height, &channels, STBI_rgb_alpha);
			else if (stbi_dds_test_memory(file.data(), (int)file.size()))
				pixels = stbi_dds_load_from_memory(file.data(), (int)file.size(), &width, &height, &depth, &channels, STBI_rgb_alpha);
			else
				pixels = stbi_load_from_memory(file.data(), (int)file.size(), &width, &height, &channels, STBI_rgb_alpha);
			if (!pixels || width <= 0 || height <= 0)
			{
				if (pixels)
					stbi_image_free(pixels);
				error = stbi_failure_reason() ? stbi_failure_reason() : "not a picture stb_image reads";
				return false;
			}
			const size_t componentBytes = floating ? 4 : 1;
			std::vector<uint8_t> rgba((size_t)width * height * 4 * componentBytes);
			std::memcpy(rgba.data(), pixels, rgba.size());
			stbi_image_free(pixels);
			// the texture's size, as ReShade resizes a file that differs
			if ((uint32_t)width != texture.width || (uint32_t)height != texture.height)
			{
				std::vector<uint8_t> resized((size_t)texture.width * texture.height * 4 * componentBytes);
				const void* done = stbir_resize(rgba.data(), width, height, 0, resized.data(), (int)texture.width, (int)texture.height, 0,
					STBIR_RGBA, floating ? STBIR_TYPE_FLOAT : STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT);
				if (!done)
				{
					error = "cannot resize it to the texture's size";
					return false;
				}
				rgba = std::move(resized);
			}
			const size_t texels = (size_t)texture.width * texture.height;
			// the texture's own components, from RGBA
			size_t keep = 4;
			switch (texture.format)
			{
			case reshadefx::texture_format::r8:
			case reshadefx::texture_format::r32f:
				keep = 1;
				break;
			case reshadefx::texture_format::rg8:
			case reshadefx::texture_format::rg32f:
				keep = 2;
				break;
			case reshadefx::texture_format::rgba8:
			case reshadefx::texture_format::rgba32f:
				keep = 4;
				break;
			default:
				error = "its texture's format takes no picture";
				return false;
			}
			out.resize(texels * keep * componentBytes);
			for (size_t t = 0; t < texels; t++)
				std::memcpy(out.data() + t * keep * componentBytes, rgba.data() + t * 4 * componentBytes, keep * componentBytes);
			return true;
		}

		struct TechniqueName
		{
			std::string name, file;
		};

		TechniqueName SplitTechnique(std::string_view entry)
		{
			const size_t at = entry.find('@');
			if (at == std::string_view::npos)
				return {std::string(Trim(entry)), {}};
			return {std::string(Trim(entry.substr(0, at))), std::string(Trim(entry.substr(at + 1)))};
		}

		uint32_t ApplicationHash()
		{
			// ReShade hashes the executable's name; std::hash is not the same everywhere, so FNV-1a
			uint32_t hash = 2166136261u;
			for (const char c : std::string_view("Cemu"))
				hash = (hash ^ (uint8_t)c) * 16777619u;
			return hash;
		}
	}

	uint32_t TexelBytes(reshadefx::texture_format format)
	{
		switch (format)
		{
		case reshadefx::texture_format::r8: return 1;
		case reshadefx::texture_format::r16f:
		case reshadefx::texture_format::r16:
		case reshadefx::texture_format::rg8: return 2;
		case reshadefx::texture_format::r32f:
		case reshadefx::texture_format::r32u:
		case reshadefx::texture_format::r32i:
		case reshadefx::texture_format::rg16f:
		case reshadefx::texture_format::rg16:
		case reshadefx::texture_format::rgba8:
		case reshadefx::texture_format::rgb10a2:
		case reshadefx::texture_format::rg11b10f: return 4;
		case reshadefx::texture_format::rg32f:
		case reshadefx::texture_format::rgba16f:
		case reshadefx::texture_format::rgba16: return 8;
		case reshadefx::texture_format::rgba32f:
		case reshadefx::texture_format::rgba32u:
		case reshadefx::texture_format::rgba32i: return 16;
		default: return 0;
		}
	}

	bool ReadPreset(const fs::path& file, Preset& preset)
	{
		std::ifstream in(file);
		if (!in)
			return false;
		preset = {};
		preset.path = file;
		std::vector<std::string> sorting;
		std::string section, line;
		bool first = true;
		while (std::getline(in, line))
		{
			std::string_view text = line;
			if (first && text.size() >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
				text.remove_prefix(3); // a byte order mark
			first = false;
			text = Trim(text);
			if (text.empty() || text[0] == ';' || text[0] == '/' || text[0] == '#')
				continue;
			if (text[0] == '[')
			{
				section = std::string(Trim(text.substr(0, text.find(']')), " \t[]"));
				continue;
			}
			const size_t equals = text.find('=');
			if (equals == std::string_view::npos)
				continue;
			const std::string key(Trim(text.substr(0, equals)));
			const std::string_view value = Trim(text.substr(equals + 1));
			if (section.empty())
			{
				if (key == "Techniques")
					preset.techniques = SplitValue(value);
				else if (key == "TechniqueSorting")
					sorting = SplitValue(value);
				else if (key == "PreprocessorDefinitions")
				{
					for (const std::string& definition : SplitValue(value))
						if (!definition.empty())
							preset.definitions.push_back(Definition(definition));
				}
				continue;
			}
			preset.sections[section][key] = std::string(value);
		}
		preset.techniques.erase(std::remove(preset.techniques.begin(), preset.techniques.end(), std::string()), preset.techniques.end());
		// the order they run in is TechniqueSorting's, for the ones Techniques turns on
		if (!sorting.empty())
		{
			std::vector<std::string> ordered;
			for (const std::string& entry : sorting)
				if (std::find(preset.techniques.begin(), preset.techniques.end(), entry) != preset.techniques.end() &&
					std::find(ordered.begin(), ordered.end(), entry) == ordered.end())
					ordered.push_back(entry);
			for (const std::string& entry : preset.techniques)
				if (std::find(ordered.begin(), ordered.end(), entry) == ordered.end())
					ordered.push_back(entry);
			preset.techniques = std::move(ordered);
		}
		return true;
	}

	std::string Chain::Summary() const
	{
		if (techniques.empty())
		{
			if (!messages.empty())
				return messages.front();
			return "no technique is turned on";
		}
		std::string text;
		for (const auto& [effect, technique] : techniques)
		{
			if (!text.empty())
				text += ", ";
			text += effects[effect].module.techniques[technique].name;
		}
		return text;
	}

	fs::path Root()
	{
		return fs::path(PS5CEMU_DATA) / "reshade";
	}

	fs::path PresetFor(const fs::path& root, uint64_t titleId)
	{
		std::error_code ec;
		char name[32];
		std::snprintf(name, sizeof(name), "%016llX.ini", (unsigned long long)titleId);
		for (const fs::path& candidate : {root / "presets" / name, root / "ReShadePreset.ini"})
			if (fs::is_regular_file(candidate, ec))
				return candidate;
		// a lower-case title ID, as some write it
		std::string lower = name;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		if (fs::is_regular_file(root / "presets" / lower, ec))
			return root / "presets" / lower;
		return {};
	}

	void Build(const fs::path& root, uint64_t titleId, const Target& target, Chain& chain)
	{
		chain = {};
		chain.preset = PresetFor(root, titleId);
		if (chain.preset.empty())
		{
			chain.messages.push_back("no preset: put ReShadePreset.ini in " + root.string() + " (or presets/<title ID>.ini for one game)");
			return;
		}
		Preset preset;
		if (!ReadPreset(chain.preset, preset))
		{
			chain.messages.push_back("cannot read " + chain.preset.string());
			return;
		}
		if (preset.techniques.empty())
		{
			chain.messages.push_back(chain.preset.filename().string() + " turns no technique on");
			return;
		}

		const std::vector<fs::path> shaderFolders = FoldersBelow({root / "Shaders", root / "reshade-shaders" / "Shaders"});
		const std::vector<fs::path> textureFolders = FoldersBelow({root / "Textures", root / "reshade-shaders" / "Textures"});
		if (shaderFolders.empty())
		{
			chain.messages.push_back("no Shaders folder in " + root.string());
			return;
		}

		// the files to compile: those the techniques name, or every one when a technique names none
		std::vector<TechniqueName> wanted;
		std::vector<std::string> files;
		bool every = false;
		for (const std::string& entry : preset.techniques)
		{
			wanted.push_back(SplitTechnique(entry));
			if (wanted.back().file.empty())
				every = true;
			else if (std::find(files.begin(), files.end(), wanted.back().file) == files.end())
				files.push_back(wanted.back().file);
		}
		if (every)
		{
			std::error_code ec;
			for (const fs::path& folder : shaderFolders)
				for (const auto& entry : fs::directory_iterator(folder, ec))
					if (entry.is_regular_file(ec) && EqualNoCase(entry.path().extension().string(), ".fx") &&
						std::find(files.begin(), files.end(), entry.path().filename().string()) == files.end())
						files.push_back(entry.path().filename().string());
		}

		for (const std::string& file : files)
		{
			const fs::path path = FindFile(shaderFolders, file);
			if (path.empty())
			{
				chain.messages.push_back(file + ": not found in the Shaders folders");
				continue;
			}
			reshadefx::preprocessor pp;
			pp.add_macro_definition("__RESHADE__", "60800");
			pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
			pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
			pp.add_macro_definition("__VENDOR__", std::to_string(target.vendorId));
			pp.add_macro_definition("__DEVICE__", std::to_string(target.deviceId));
			pp.add_macro_definition("__RENDERER__", std::to_string(target.renderer));
			pp.add_macro_definition("__APPLICATION__", std::to_string(ApplicationHash()));
			pp.add_macro_definition("BUFFER_WIDTH", std::to_string(target.width));
			pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(target.height));
			pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
			pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
			pp.add_macro_definition("BUFFER_COLOR_SPACE", "1"); // sRGB
			pp.add_macro_definition("BUFFER_COLOR_FORMAT", std::to_string(target.colorFormat));
			pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", std::to_string(target.colorBitDepth));
			for (const auto& [name, value] : preset.definitions)
				pp.add_macro_definition(name, value);
			if (auto section = preset.sections.find(file); section != preset.sections.end())
				if (auto definitions = section->second.find("PreprocessorDefinitions"); definitions != section->second.end())
					for (const std::string& definition : SplitValue(definitions->second))
						if (!definition.empty())
						{
							const auto [name, value] = Definition(definition);
							pp.add_macro_definition(name, value);
						}
			pp.add_include_path(path.parent_path());
			for (const fs::path& folder : shaderFolders)
				pp.add_include_path(folder);
			// what ReShade adds for effects written for its older versions
			pp.append_string(
				"#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
				"#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
				"#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
				"#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
				"#define tex2Dgather0 tex2DgatherR\n"
				"#define tex2Dgather1 tex2DgatherG\n"
				"#define tex2Dgather2 tex2DgatherB\n"
				"#define tex2Dgather3 tex2DgatherA\n");
			const bool preprocessed = pp.append_file(path);
			std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_spirv(true, false, false, false, false));
			reshadefx::parser parser;
			const bool parsed = preprocessed && parser.parse(pp.output(), codegen.get());
			if (!parsed)
			{
				std::istringstream errors(pp.errors() + parser.errors());
				std::string errorLine;
				int shown = 0;
				while (std::getline(errors, errorLine) && shown < 8)
					if (errorLine.find("error") != std::string::npos)
					{
						chain.messages.push_back(file + ": " + errorLine);
						shown++;
					}
				if (shown == 0)
					chain.messages.push_back(file + ": did not compile");
				continue;
			}

			Effect effect;
			effect.name = file;
			effect.module = codegen->module();
			bool assembled = true;
			for (const auto& [entryPoint, type] : effect.module.entry_points)
			{
				std::string binary, assembly, errors;
				if (!codegen->assemble_code_for_entry_point(entryPoint, binary, assembly, errors) || binary.empty() || binary.size() % 4)
				{
					chain.messages.push_back(file + ": " + entryPoint + " did not assemble " + errors);
					assembled = false;
					break;
				}
				effect.code[entryPoint] = std::move(binary);
			}
			if (!assembled)
				continue;

			// the uniforms: their initial values, then the preset's
			effect.uniformData.assign((effect.module.total_uniform_size + 15) & ~15u, 0);
			const auto section = preset.sections.find(file);
			for (const reshadefx::uniform& uniform : effect.module.uniforms)
			{
				const Special special = SpecialOf(AnnotationString(uniform.annotations, "source"));
				effect.specials.push_back(special);
				ResetUniform(effect, uniform, special);
				if (special != Special::None || section == preset.sections.end())
					continue;
				if (auto value = section->second.find(uniform.name); value != section->second.end())
					ApplyPresetValue(effect, uniform, value->second);
			}

			// the pictures textures load
			effect.pictures.resize(effect.module.textures.size());
			for (size_t t = 0; t < effect.module.textures.size(); t++)
			{
				const reshadefx::texture& texture = effect.module.textures[t];
				const std::string source = AnnotationString(texture.annotations, "source");
				if (source.empty() || !texture.semantic.empty())
					continue;
				const fs::path picturePath = FindFile(textureFolders, fs::path(source));
				std::string error = "not found in the Textures folders";
				if (picturePath.empty() || !LoadPicture(texture, picturePath, effect.pictures[t], error))
					chain.messages.push_back(file + ": texture " + texture.name + " from " + source + ": " + error);
			}
			chain.effects.push_back(std::move(effect));
		}

		// the techniques, in the preset's order
		for (const TechniqueName& name : wanted)
		{
			bool found = false;
			for (size_t e = 0; e < chain.effects.size() && !found; e++)
			{
				if (!name.file.empty() && !EqualNoCase(chain.effects[e].name, name.file))
					continue;
				const auto& techniques = chain.effects[e].module.techniques;
				for (size_t t = 0; t < techniques.size(); t++)
				{
					if (techniques[t].name != name.name)
						continue;
					if (techniques[t].passes.empty())
						break;
					chain.techniques.emplace_back(e, t);
					found = true;
					break;
				}
			}
			if (!found && std::none_of(chain.messages.begin(), chain.messages.end(), [&](const std::string& message) {
					return !name.file.empty() && message.rfind(name.file + ":", 0) == 0;
				}))
				chain.messages.push_back("technique " + name.name + (name.file.empty() ? "" : "@" + name.file) + " is not there");
		}
	}

	void UpdateSpecials(Effect& effect, const Frame& frame)
	{
		for (size_t u = 0; u < effect.module.uniforms.size() && u < effect.specials.size(); u++)
		{
			const reshadefx::uniform& uniform = effect.module.uniforms[u];
			switch (effect.specials[u])
			{
			case Special::FrameTime:
				WriteFloats(effect, uniform, &frame.frameTimeMs, 1);
				break;
			case Special::FrameCount:
				if (uniform.type.is_boolean())
				{
					const uint32_t even = (frame.frameCount % 2) == 0;
					WriteUints(effect, uniform, &even, 1);
				}
				else
					WriteUints(effect, uniform, &frame.frameCount, 1);
				break;
			case Special::Random:
			{
				const int low = AnnotationInt(uniform.annotations, "min", 0, 0);
				const int high = AnnotationInt(uniform.annotations, "max", 0, RAND_MAX);
				const int32_t value = low + (std::rand() % (std::abs(high - low) + 1));
				WriteInts(effect, uniform, &value, 1);
				break;
			}
			case Special::PingPong:
			{
				// ReShade's: up from min to max and back, step units a second
				const float low = AnnotationFloat(uniform.annotations, "min", 0, 0.0f);
				const float high = AnnotationFloat(uniform.annotations, "max", 0, 1.0f);
				const float stepMin = AnnotationFloat(uniform.annotations, "step", 0, 0.0f);
				const float stepMax = AnnotationFloat(uniform.annotations, "step", 1, 0.0f);
				float increment = stepMax == 0 ? stepMin : (stepMin + std::fmod((float)std::rand(), stepMax - stepMin + 1));
				const float smoothing = AnnotationFloat(uniform.annotations, "smoothing", 0, 0.0f);
				float value[2] = {ReadFloat(effect, uniform, 0), ReadFloat(effect, uniform, 1)};
				const float seconds = frame.frameTimeMs * 1e-3f;
				if (value[1] >= 0)
				{
					increment = std::max(increment - std::max(0.0f, smoothing - (high - value[0])), 0.05f) * seconds;
					if ((value[0] += increment) >= high)
						value[0] = high, value[1] = -1;
				}
				else
				{
					increment = std::max(increment - std::max(0.0f, smoothing - (value[0] - low)), 0.05f) * seconds;
					if ((value[0] -= increment) <= low)
						value[0] = low, value[1] = +1;
				}
				WriteFloats(effect, uniform, value, 2);
				break;
			}
			case Special::Date:
			{
				const std::time_t now = std::time(nullptr);
				std::tm local{};
				localtime_r(&now, &local);
				const int32_t value[4] = {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec};
				WriteInts(effect, uniform, value, 4);
				break;
			}
			case Special::Timer:
				WriteUints(effect, uniform, &frame.timerMs, 1);
				break;
			case Special::OverlayOpen:
			{
				const uint32_t open = frame.overlayOpen;
				WriteUints(effect, uniform, &open, 1);
				break;
			}
			case Special::None:
			case Special::Unsupported:
				break;
			}
		}
	}
}
