// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: QR codes (docs/UI-REDESIGN.md, 6.7 and 9.2), so a phone can open the part of
// the guide a screen is about. Byte mode at error correction level M, the smallest of versions 1 to
// 10 the text fits (213 bytes at most), with the mask the standard's penalty rules choose. The canvas
// draws it as squares, a row's dark run as one: nothing to rasterise or keep.

#pragma once

#include "canvas.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace ui
{
	class QrCode
	{
	public:
		// Size() is 0 when the text is too long
		explicit QrCode(std::string_view text);

		int Size() const { return m_size; }
		int Version() const { return m_version; }
		int Mask() const { return m_mask; }
		bool Dark(int x, int y) const { return m_modules[(size_t)y * m_size + x] != 0; }

		// As made with this mask instead of the one the penalty rules chose (to check it against
		// another encoder)
		static QrCode WithMask(std::string_view text, int mask);

	private:
		QrCode() = default;
		bool Encode(std::string_view text, int mask);

		int m_size = 0, m_version = 0, m_mask = -1;
		std::vector<uint8_t> m_modules;
	};

	// The code on a white card filling box (a square), its quiet zone included
	void DrawQr(Canvas& canvas, const QrCode& code, const Box& box, float radius, uint32_t dark = 0xff0d0705, uint32_t light = 0xffffffff);
}
