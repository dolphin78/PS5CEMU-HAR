// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: text (text.h).

#include "text.h"
#include "gfx.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace ui
{
	namespace
	{
		constexpr uint32_t kFreeRows = 640; // for the glyphs rendered on demand
		constexpr char32_t kEllipsis = 0x2026;
		// how much bolder a fallback glyph is drawn for each weight (it is rendered once, regular)
		constexpr float kFallbackWeight[] = {0.0f, 0.03f, 0.06f, 0.09f};

		uint64_t Key(uint32_t codepoint, Weight weight)
		{
			return (uint64_t)codepoint << 8 | (uint8_t)weight;
		}

		template<typename T>
		bool Get(const std::vector<uint8_t>& data, size_t& at, T& value)
		{
			if (at + sizeof(T) > data.size())
				return false;
			std::memcpy(&value, data.data() + at, sizeof(T));
			at += sizeof(T);
			return true;
		}

		bool IsCjk(char32_t c)
		{
			return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef) ||
				c >= 0x20000;
		}

		bool IsSpace(char32_t c)
		{
			return c == ' ' || c == '\t' || c == 0x3000;
		}

		// Font files in a folder and the folders in it, two levels down
		void FindFonts(const std::string& folder, int depth, std::vector<std::string>& out)
		{
			DIR* dir = opendir(folder.c_str());
			if (!dir)
				return;
			std::vector<std::string> names;
			while (dirent* entry = readdir(dir))
				if (entry->d_name[0] != '.')
					names.push_back(entry->d_name);
			closedir(dir);
			std::sort(names.begin(), names.end());
			for (const std::string& name : names)
			{
				const std::string path = folder + "/" + name;
				struct stat info{};
				if (stat(path.c_str(), &info) != 0)
					continue;
				if (S_ISDIR(info.st_mode))
				{
					if (depth > 0)
						FindFonts(path, depth - 1, out);
					continue;
				}
				std::string lower = name;
				for (char& c : lower)
					c = (char)std::tolower((unsigned char)c);
				if (lower.ends_with(".ttf") || lower.ends_with(".otf") || lower.ends_with(".ttc"))
					out.push_back(path);
			}
		}
	}

	struct Fonts::Fallback
	{
		FT_Library library = nullptr;
		std::vector<FT_Face> faces;
		bool opened = false;

		~Fallback()
		{
			for (FT_Face face : faces)
				FT_Done_Face(face);
			if (library)
				FT_Done_FreeType(library);
		}
	};

	Fonts::Fonts() = default;
	Fonts::~Fonts() = default;

	bool Fonts::Load(const std::string& path, std::string& error)
	{
		std::ifstream file(path, std::ios::binary);
		const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		size_t at = 8;
		if (data.size() < 64 || std::memcmp(data.data(), "UISDF01", 8) != 0)
		{
			error = path + " is missing or not the kit's font";
			return false;
		}
		int32_t ascender, descender, lineGap, unitsPerEm;
		uint32_t width, height, weights, glyphs;
		Get(data, at, m_size);
		Get(data, at, m_spread);
		Get(data, at, ascender);
		Get(data, at, descender);
		Get(data, at, lineGap);
		Get(data, at, unitsPerEm);
		Get(data, at, width);
		Get(data, at, height);
		Get(data, at, weights);
		if (!Get(data, at, glyphs) || unitsPerEm <= 0 || weights > 4 || width == 0 || width > 8192 || height > 8192)
		{
			error = path + " has a header the kit cannot read";
			return false;
		}
		m_ascender = (float)ascender / unitsPerEm;
		m_descender = (float)descender / unitsPerEm;
		at += weights * sizeof(uint16_t);
		m_glyphs.clear();
		for (uint32_t i = 0; i < glyphs; i++)
		{
			uint32_t codepoint;
			uint16_t weight;
			Glyph glyph;
			Get(data, at, codepoint);
			Get(data, at, weight);
			Get(data, at, glyph.x);
			Get(data, at, glyph.y);
			Get(data, at, glyph.width);
			Get(data, at, glyph.height);
			Get(data, at, glyph.left);
			Get(data, at, glyph.top);
			if (!Get(data, at, glyph.advance))
			{
				error = path + " is cut short";
				return false;
			}
			glyph.found = true;
			m_glyphs[Key(codepoint, (Weight)weight)] = glyph;
		}
		uint32_t packed = 0;
		if (!Get(data, at, packed) || at + packed > data.size())
		{
			error = path + " is cut short";
			return false;
		}
		m_width = width;
		m_bakedRows = height;
		m_height = height + kFreeRows;
		m_atlas.assign((size_t)m_width * m_height, 0);
		uLongf unpacked = (uLongf)width * height;
		if (uncompress(m_atlas.data(), &unpacked, data.data() + at, packed) != Z_OK || unpacked != (uLongf)width * height)
		{
			error = path + "'s atlas cannot be unpacked";
			return false;
		}
		for (int weight = 0; weight < 4; weight++)
		{
			m_digitAdvance[weight] = 0;
			for (char32_t digit = '0'; digit <= '9'; digit++)
				m_digitAdvance[weight] = std::max(m_digitAdvance[weight], Find(digit, (Weight)weight).advance);
		}
		m_shelfX = 0;
		m_shelfY = m_bakedRows + 1;
		m_shelfHeight = 0;
		m_changedFirst = 0;
		m_changedEnd = m_height;
		return true;
	}

	void Fonts::SetFallbackFolders(std::vector<std::string> folders)
	{
		m_folders = std::move(folders);
	}

	bool Fonts::TakeChanged(uint32_t& firstRow, uint32_t& rows)
	{
		if (m_changedEnd <= m_changedFirst)
			return false;
		firstRow = m_changedFirst;
		rows = m_changedEnd - m_changedFirst;
		m_changedFirst = m_changedEnd = 0;
		return true;
	}

	const Fonts::Glyph& Fonts::Find(uint32_t codepoint, Weight weight)
	{
		const auto it = m_glyphs.find(Key(codepoint, weight));
		if (it != m_glyphs.end())
			return it->second;
		// another weight baked (a fallback glyph is rendered once, for all of them)
		if (weight != Weight::Regular)
		{
			const auto regular = m_glyphs.find(Key(codepoint, Weight::Regular));
			if (regular != m_glyphs.end() && !regular->second.found)
				return regular->second;
		}
		return Render(codepoint, weight);
	}

	// A glyph Lexend lacks, from the first fallback font that has it, as a signed distance from its
	// bitmap (as tools/render-sdf-font.cpp makes Lexend's, at the atlas's size)
	const Fonts::Glyph& Fonts::Render(uint32_t codepoint, Weight weight)
	{
		Glyph& regular = m_glyphs[Key(codepoint, Weight::Regular)];
		if (regular.found || codepoint < 0x20)
			return weight == Weight::Regular ? regular : (m_glyphs[Key(codepoint, weight)] = regular);
		if (!m_fallback)
			m_fallback = std::make_unique<Fallback>();
		Fallback& fallback = *m_fallback;
		if (!fallback.opened)
		{
			fallback.opened = true;
			std::vector<std::string> files;
			for (const std::string& folder : m_folders)
				FindFonts(folder, 2, files);
			if (!files.empty() && FT_Init_FreeType(&fallback.library) == 0)
			{
				FT_Int spread = (FT_Int)m_spread;
				FT_Property_Set(fallback.library, "bsdf", "spread", &spread);
				for (const std::string& file : files)
				{
					FT_Face face = nullptr;
					if (fallback.faces.size() < 64 && FT_New_Face(fallback.library, file.c_str(), 0, &face) == 0)
					{
						FT_Set_Pixel_Sizes(face, 0, m_size);
						fallback.faces.push_back(face);
					}
				}
			}
			Log("[ui] text: " + std::to_string(fallback.faces.size()) + " fallback fonts of " + std::to_string(files.size()) + " files found");
		}
		Glyph glyph;
		for (FT_Face face : fallback.faces)
		{
			const FT_UInt index = FT_Get_Char_Index(face, codepoint);
			if (index == 0 || FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING) != 0)
				continue;
			glyph.found = true;
			glyph.advance = face->glyph->advance.x / 64.0f;
			if (face->glyph->outline.n_points == 0 || FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0 ||
				FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF) != 0)
				break;
			const FT_Bitmap& bitmap = face->glyph->bitmap;
			if (m_shelfX + bitmap.width + 1 > m_width)
			{
				m_shelfX = 0;
				m_shelfY += m_shelfHeight + 1;
				m_shelfHeight = 0;
			}
			if (m_shelfY + bitmap.rows > m_height)
			{
				Log("[ui] text: the atlas is full");
				break;
			}
			for (unsigned row = 0; row < bitmap.rows; row++)
				std::memcpy(&m_atlas[(size_t)(m_shelfY + row) * m_width + m_shelfX], bitmap.buffer + (ptrdiff_t)row * bitmap.pitch, bitmap.width);
			glyph.x = (uint16_t)m_shelfX;
			glyph.y = (uint16_t)m_shelfY;
			glyph.width = (uint16_t)bitmap.width;
			glyph.height = (uint16_t)bitmap.rows;
			glyph.left = (int16_t)face->glyph->bitmap_left;
			glyph.top = (int16_t)face->glyph->bitmap_top;
			if (m_changedEnd <= m_changedFirst)
				m_changedFirst = m_shelfY, m_changedEnd = m_shelfY + bitmap.rows;
			else
				m_changedFirst = std::min(m_changedFirst, m_shelfY), m_changedEnd = std::max(m_changedEnd, m_shelfY + bitmap.rows);
			m_shelfX += bitmap.width + 1;
			m_shelfHeight = std::max(m_shelfHeight, (uint32_t)bitmap.rows);
			break;
		}
		regular = glyph;
		for (int other = 1; other < 4; other++)
		{
			Glyph bolder = glyph;
			bolder.weight = kFallbackWeight[other];
			m_glyphs[Key(codepoint, (Weight)other)] = bolder;
		}
		return m_glyphs[Key(codepoint, weight)];
	}

	float Fonts::Advance(uint32_t codepoint, const TextStyle& style)
	{
		const float scale = style.size / m_size;
		const bool digit = style.tabular && codepoint >= '0' && codepoint <= '9';
		const Glyph& glyph = Find(codepoint, style.weight);
		return (digit ? m_digitAdvance[(int)style.weight] : glyph.advance) * scale + (glyph.found || digit ? style.tracking : 0.0f);
	}

	TextBlock Fonts::Layout(const TextStyle& style, std::string_view utf8, float maxWidth, int maxLines)
	{
		std::u32string text = Decode(utf8);
		if (style.upper)
			for (char32_t& c : text)
				c = Upper(c);
		// the lines, as runs of the text
		std::vector<std::u32string> lines;
		bool truncated = false;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find(U'\n', start);
			if (end == std::u32string::npos)
				end = text.size();
			const std::u32string_view paragraph(text.data() + start, end - start);
			// greedy: as many words as fit; a word longer than a line is broken where it must be
			size_t lineStart = 0;
			while (lineStart < paragraph.size() || (lineStart == 0 && paragraph.empty()))
			{
				float width = 0;
				size_t at = lineStart, lastBreak = std::u32string::npos;
				for (; at < paragraph.size(); at++)
				{
					const char32_t c = paragraph[at];
					if (IsSpace(c))
						lastBreak = at;
					else if (IsCjk(c) && at > lineStart)
						lastBreak = at; // before it
					const float advance = Advance(c, style);
					if (maxWidth > 0 && !IsSpace(c) && width + advance > maxWidth + 0.01f && at > lineStart)
						break;
					width += advance;
				}
				size_t lineEnd = at, next = at;
				if (at < paragraph.size())
				{
					if (lastBreak != std::u32string::npos && lastBreak > lineStart)
						lineEnd = next = lastBreak;
				}
				std::u32string line(paragraph.substr(lineStart, lineEnd - lineStart));
				while (!line.empty() && IsSpace(line.back()))
					line.pop_back();
				lines.push_back(std::move(line));
				while (next < paragraph.size() && IsSpace(paragraph[next]))
					next++;
				if (next == lineStart)
					next++;
				lineStart = next;
				if (paragraph.empty())
					break;
			}
			start = end + 1;
		}
		if (maxLines > 0 && (int)lines.size() > maxLines)
		{
			lines.resize(maxLines);
			truncated = true;
		}
		if (truncated && !lines.empty())
		{
			// the last line's words go, last first, until it and the ellipsis fit
			std::u32string& last = lines.back();
			const float ellipsis = Advance(kEllipsis, style);
			auto width = [&](const std::u32string& line) {
				float w = 0;
				for (char32_t c : line)
					w += Advance(c, style);
				return w;
			};
			while (maxWidth > 0 && !last.empty() && width(last) + ellipsis > maxWidth)
			{
				size_t cut = last.find_last_of(U" \t");
				if (cut == std::u32string::npos || cut == 0)
				{
					last.pop_back(); // one long word: a character at a time
					continue;
				}
				last.resize(cut);
			}
			while (!last.empty() && (IsSpace(last.back()) || last.back() == ',' || last.back() == ':' || last.back() == ';' || last.back() == '.'))
				last.pop_back();
			last.push_back(kEllipsis);
		}

		TextBlock block;
		block.truncated = truncated;
		block.lineHeight = style.size * style.lineHeight;
		const float ascent = m_ascender * style.size, descent = -m_descender * style.size;
		block.baseline = (block.lineHeight - (ascent + descent)) * 0.5f + ascent;
		const float scale = style.size / m_size;
		const float pixelU = 1.0f / m_width, pixelV = 1.0f / m_height;
		for (size_t index = 0; index < lines.size(); index++)
		{
			const float baseline = block.baseline + block.lineHeight * index;
			TextBlock::Line line{(uint32_t)block.glyphs.size(), 0, 0};
			float pen = 0;
			for (char32_t c : lines[index])
			{
				const Glyph& glyph = Find(c, style.weight);
				const bool digit = style.tabular && c >= '0' && c <= '9';
				const float advance = Advance(c, style);
				const float centre = digit ? (m_digitAdvance[(int)style.weight] - glyph.advance) * 0.5f * scale : 0.0f;
				if (glyph.width > 0)
				{
					PlacedGlyph placed;
					placed.x = pen + centre + glyph.left * scale;
					placed.y = baseline - glyph.top * scale;
					placed.width = glyph.width * scale;
					placed.height = glyph.height * scale;
					placed.u0 = glyph.x * pixelU;
					placed.v0 = glyph.y * pixelV;
					placed.u1 = (glyph.x + glyph.width) * pixelU;
					placed.v1 = (glyph.y + glyph.height) * pixelV;
					placed.weight = glyph.weight;
					block.glyphs.push_back(placed);
				}
				pen += advance;
			}
			line.count = (uint32_t)block.glyphs.size() - line.first;
			line.width = lines[index].empty() ? 0.0f : pen - style.tracking;
			block.width = std::max(block.width, line.width);
			block.lines.push_back(line);
		}
		block.height = block.lineHeight * (float)block.lines.size();
		return block;
	}

	float Fonts::Width(const TextStyle& style, std::string_view utf8)
	{
		float width = 0;
		for (char32_t c : Decode(utf8))
			width += Advance(style.upper ? Upper(c) : c, style);
		return std::max(0.0f, width - style.tracking);
	}

	float Fonts::Fit(TextStyle style, std::string_view utf8, float maxWidth, int maxLines, float smallest, float step)
	{
		for (;; style.size -= step)
		{
			if (style.size <= smallest)
				return smallest;
			const TextBlock block = Layout(style, utf8, maxWidth, maxLines);
			if (!block.truncated)
				return style.size;
		}
	}

	std::u32string Decode(std::string_view utf8)
	{
		std::u32string out;
		out.reserve(utf8.size());
		for (size_t i = 0; i < utf8.size();)
		{
			const unsigned char lead = (unsigned char)utf8[i];
			int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
			if (length == 0 || i + length > utf8.size())
			{
				out.push_back(0xfffd);
				i++;
				continue;
			}
			char32_t c = length == 1 ? lead : lead & (0x7f >> length);
			bool ok = true;
			for (int k = 1; k < length; k++)
			{
				const unsigned char next = (unsigned char)utf8[i + k];
				ok = ok && (next & 0xc0) == 0x80;
				c = c << 6 | (next & 0x3f);
			}
			out.push_back(ok ? c : 0xfffd);
			i += ok ? length : 1;
		}
		return out;
	}

	std::string Encode(std::u32string_view text)
	{
		std::string out;
		for (char32_t c : text)
		{
			if (c < 0x80)
				out += (char)c;
			else if (c < 0x800)
				out += {(char)(0xc0 | c >> 6), (char)(0x80 | (c & 0x3f))};
			else if (c < 0x10000)
				out += {(char)(0xe0 | c >> 12), (char)(0x80 | (c >> 6 & 0x3f)), (char)(0x80 | (c & 0x3f))};
			else
				out += {(char)(0xf0 | c >> 18), (char)(0x80 | (c >> 12 & 0x3f)), (char)(0x80 | (c >> 6 & 0x3f)), (char)(0x80 | (c & 0x3f))};
		}
		return out;
	}

	char32_t Upper(char32_t c)
	{
		if (c >= 'a' && c <= 'z')
			return c - 32;
		if (c >= 0xe0 && c <= 0xfe && c != 0xf7)
			return c - 32;
		if (c == 0xff)
			return 0x178;
		if ((c >= 0x100 && c <= 0x137) || (c >= 0x14a && c <= 0x177))
			return (c & 1) ? c - 1 : c;
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
			return (c & 1) ? c : c - 1;
		return c;
	}

	std::string Upper(std::string_view utf8)
	{
		std::u32string text = Decode(utf8);
		for (char32_t& c : text)
			c = Upper(c);
		return Encode(text);
	}
}
