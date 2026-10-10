// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: text (docs/UI-REDESIGN.md, 7.2 and 9.6). Lexend's four weights come baked
// into a signed-distance atlas (fonts/lexend.sdf, tools/render-sdf-font.sh), so nothing is rendered
// as the launcher starts; a character Lexend lacks (a Japanese title's) is rendered on first use with
// FreeType from the fonts in the fallback folders, into the same atlas's free rows.
//
// Layout is UTF-8 throughout: lines break at spaces, between CJK characters and at a newline; text
// past its last line ends in an ellipsis that replaces whole words; changing numbers can take
// tabular figures. Lines are placed as CSS places them: the line box's height is the size times the
// line height, the font's ascent and descent centred in it.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ui
{
	enum class Weight : uint8_t
	{
		Regular,
		Medium,
		SemiBold,
		Bold,
	};

	struct TextStyle
	{
		float size = 26;		// pixels a em, in the layout
		Weight weight = Weight::Regular;
		float lineHeight = 1.45f; // the line box, times the size
		float tracking = 0;		// added after each character, in pixels
		bool upper = false;		// in capitals
		bool tabular = false;	// figures all as wide
	};

	// A glyph as laid out: its quad (relative to the text's top left) and its rectangle in the atlas
	struct PlacedGlyph
	{
		float x, y, width, height;
		float u0, v0, u1, v1;
		float weight; // how much the shader thickens it (a fallback glyph drawn bolder)
	};

	struct TextBlock
	{
		std::vector<PlacedGlyph> glyphs;
		struct Line
		{
			uint32_t first, count; // in glyphs
			float width;
		};
		std::vector<Line> lines;
		float width = 0, height = 0; // the widest line, and the lines' boxes
		float lineHeight = 0;
		float baseline = 0;			 // the first line's, from the top
		bool truncated = false;		 // an ellipsis took the rest
	};

	class Fonts
	{
	public:
		Fonts();
		~Fonts();
		// The baked atlas. False, with the reason, when it cannot be read.
		bool Load(const std::string& path, std::string& error);
		// Where fonts for the characters Lexend lacks are looked for (.ttf, .otf, .ttc), in order;
		// read the first time one is needed.
		void SetFallbackFolders(std::vector<std::string> folders);

		// text laid out in lines no wider than maxWidth (0: one line per paragraph, as long as it is),
		// at most maxLines of them (0: all)
		TextBlock Layout(const TextStyle& style, std::string_view utf8, float maxWidth = 0, int maxLines = 0);
		// one line's width
		float Width(const TextStyle& style, std::string_view utf8);
		// the size that fits text on its lines, from style's down to smallest in steps of step
		float Fit(TextStyle style, std::string_view utf8, float maxWidth, int maxLines, float smallest, float step = 4);

		// The atlas, one byte a texel (for Gfx::SetAtlas), and the rows changed since the last call
		uint32_t AtlasWidth() const { return m_width; }
		uint32_t AtlasHeight() const { return m_height; }
		const uint8_t* Atlas() const { return m_atlas.data(); }
		bool TakeChanged(uint32_t& firstRow, uint32_t& rows);

	private:
		struct Glyph
		{
			uint16_t x = 0, y = 0, width = 0, height = 0;
			int16_t left = 0, top = 0;
			float advance = 0;
			float weight = 0;
			bool found = false;
		};
		struct Fallback;

		const Glyph& Find(uint32_t codepoint, Weight weight);
		const Glyph& Render(uint32_t codepoint, Weight weight);
		float Advance(uint32_t codepoint, const TextStyle& style);

		uint32_t m_size = 48, m_spread = 6;
		float m_ascender = 1, m_descender = -0.25f; // a em
		uint32_t m_width = 0, m_height = 0;
		uint32_t m_bakedRows = 0;
		std::vector<uint8_t> m_atlas;
		std::unordered_map<uint64_t, Glyph> m_glyphs; // codepoint << 8 | weight
		float m_digitAdvance[4] = {};
		// the free rows' shelf for the glyphs rendered on demand
		uint32_t m_shelfX = 0, m_shelfY = 0, m_shelfHeight = 0;
		uint32_t m_changedFirst = 0, m_changedEnd = 0;
		std::vector<std::string> m_folders;
		std::unique_ptr<Fallback> m_fallback;
	};

	// UTF-8 to code points (a malformed byte is U+FFFD)
	std::u32string Decode(std::string_view utf8);
	std::string Encode(std::u32string_view text);
	// Capitals of Latin letters (the overline's)
	char32_t Upper(char32_t c);
	std::string Upper(std::string_view utf8);
}
