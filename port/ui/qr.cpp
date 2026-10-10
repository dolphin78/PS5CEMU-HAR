// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: QR codes (qr.h), as ISO/IEC 18004 lays them out: the data in byte mode with
// its Reed-Solomon codewords in blocks, interleaved, placed in two-column zigzags around the finder,
// timing and alignment patterns, masked, then the format (and from version 7 the version) bits.

#include "qr.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace ui
{
	namespace
	{
		constexpr int kMaxVersion = 10;
		// level M, versions 1 to 10: error correction codewords in each block, and how many blocks
		constexpr int kEccPerBlock[kMaxVersion + 1] = {0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26};
		constexpr int kBlocks[kMaxVersion + 1] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5};
		constexpr uint32_t kFormatM = 0; // level M's two format bits

		// the modules left for data and error correction once the patterns have theirs
		int RawModules(int version)
		{
			int modules = (16 * version + 128) * version + 64;
			if (version >= 2)
			{
				const int alignments = version / 7 + 2;
				modules -= (25 * alignments - 10) * alignments - 55;
				if (version >= 7)
					modules -= 36;
			}
			return modules;
		}

		int DataCodewords(int version)
		{
			return RawModules(version) / 8 - kEccPerBlock[version] * kBlocks[version];
		}

		std::vector<int> AlignmentPositions(int version)
		{
			if (version == 1)
				return {};
			const int count = version / 7 + 2, size = version * 4 + 17;
			const int step = (version * 4 + count * 2 + 1) / (count * 2 - 2) * 2;
			std::vector<int> positions(count);
			positions[0] = 6;
			for (int i = count - 1, at = size - 7; i >= 1; i--, at -= step)
				positions[i] = at;
			return positions;
		}

		// GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1
		uint8_t Multiply(uint8_t x, uint8_t y)
		{
			uint32_t z = 0;
			for (int i = 7; i >= 0; i--)
			{
				z = (z << 1) ^ ((z >> 7) * 0x11d);
				z ^= ((y >> i) & 1u) * x;
			}
			return (uint8_t)z;
		}

		// the generator polynomial of the given degree, its leading 1 left out
		std::vector<uint8_t> Divisor(int degree)
		{
			std::vector<uint8_t> result(degree, 0);
			result[degree - 1] = 1;
			uint8_t root = 1;
			for (int i = 0; i < degree; i++)
			{
				for (int j = 0; j < degree; j++)
				{
					result[j] = Multiply(result[j], root);
					if (j + 1 < degree)
						result[j] ^= result[j + 1];
				}
				root = Multiply(root, 0x02);
			}
			return result;
		}

		std::vector<uint8_t> Remainder(const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor)
		{
			std::vector<uint8_t> result(divisor.size(), 0);
			for (uint8_t byte : data)
			{
				const uint8_t factor = byte ^ result[0];
				result.erase(result.begin());
				result.push_back(0);
				for (size_t i = 0; i < result.size(); i++)
					result[i] ^= Multiply(divisor[i], factor);
			}
			return result;
		}

		bool Masked(int mask, int x, int y)
		{
			switch (mask)
			{
			case 0: return (x + y) % 2 == 0;
			case 1: return y % 2 == 0;
			case 2: return x % 3 == 0;
			case 3: return (x + y) % 3 == 0;
			case 4: return (x / 3 + y / 2) % 2 == 0;
			case 5: return x * y % 2 + x * y % 3 == 0;
			case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
			default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
			}
		}

		// A matrix being built: the modules and which of them the patterns own
		struct Matrix
		{
			int size;
			std::vector<uint8_t> dark, fixed;

			explicit Matrix(int s) : size(s), dark((size_t)s * s, 0), fixed((size_t)s * s, 0) {}
			uint8_t& Dark(int x, int y) { return dark[(size_t)y * size + x]; }
			bool Get(int x, int y) const { return dark[(size_t)y * size + x] != 0; }
			bool Fixed(int x, int y) const { return fixed[(size_t)y * size + x] != 0; }
			void Set(int x, int y, bool on)
			{
				dark[(size_t)y * size + x] = on;
				fixed[(size_t)y * size + x] = 1;
			}
		};

		void Finder(Matrix& m, int cx, int cy)
		{
			for (int dy = -4; dy <= 4; dy++)
				for (int dx = -4; dx <= 4; dx++)
				{
					const int x = cx + dx, y = cy + dy, distance = std::max(std::abs(dx), std::abs(dy));
					if (x >= 0 && x < m.size && y >= 0 && y < m.size)
						m.Set(x, y, distance != 2 && distance != 4);
				}
		}

		void FormatBits(Matrix& m, int mask)
		{
			const uint32_t data = kFormatM << 3 | (uint32_t)mask;
			uint32_t remainder = data;
			for (int i = 0; i < 10; i++)
				remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
			const uint32_t bits = (data << 10 | remainder) ^ 0x5412;
			auto bit = [bits](int i) { return ((bits >> i) & 1) != 0; };
			const int size = m.size;
			// around the top left finder
			for (int i = 0; i <= 5; i++)
				m.Set(8, i, bit(i));
			m.Set(8, 7, bit(6));
			m.Set(8, 8, bit(7));
			m.Set(7, 8, bit(8));
			for (int i = 9; i < 15; i++)
				m.Set(14 - i, 8, bit(i));
			// and again beside the other two
			for (int i = 0; i < 8; i++)
				m.Set(size - 1 - i, 8, bit(i));
			for (int i = 8; i < 15; i++)
				m.Set(8, size - 15 + i, bit(i));
			m.Set(8, size - 8, true); // the dark module
		}

		void VersionBits(Matrix& m, int version)
		{
			if (version < 7)
				return;
			uint32_t remainder = (uint32_t)version;
			for (int i = 0; i < 12; i++)
				remainder = (remainder << 1) ^ ((remainder >> 11) * 0x1f25);
			const uint32_t bits = (uint32_t)version << 12 | remainder;
			for (int i = 0; i < 18; i++)
			{
				const bool on = ((bits >> i) & 1) != 0;
				const int a = m.size - 11 + i % 3, b = i / 3;
				m.Set(a, b, on);
				m.Set(b, a, on);
			}
		}

		// The standard's penalty for a masked matrix: runs of five or more, 2 x 2 blocks, patterns
		// like a finder's, and an unbalanced share of dark modules
		long Penalty(const Matrix& m)
		{
			const int size = m.size;
			long penalty = 0;
			auto addHistory = [size](int run, std::array<int, 7>& history) {
				if (history[0] == 0)
					run += size; // the light border before the first run
				std::copy_backward(history.begin(), history.end() - 1, history.end());
				history[0] = run;
			};
			auto finders = [](const std::array<int, 7>& history) {
				const int n = history[1];
				const bool core = n > 0 && history[2] == n && history[3] == n * 3 && history[4] == n && history[5] == n;
				return (core && history[0] >= n * 4 && history[6] >= n ? 1 : 0) + (core && history[6] >= n * 4 && history[0] >= n ? 1 : 0);
			};
			for (int pass = 0; pass < 2; pass++)
				for (int a = 0; a < size; a++)
				{
					bool colour = false;
					int run = 0;
					std::array<int, 7> history{};
					for (int b = 0; b < size; b++)
					{
						const bool dark = pass == 0 ? m.Get(b, a) : m.Get(a, b);
						if (dark == colour)
						{
							run++;
							if (run == 5)
								penalty += 3;
							else if (run > 5)
								penalty++;
						}
						else
						{
							addHistory(run, history);
							if (!colour)
								penalty += finders(history) * 40;
							colour = dark;
							run = 1;
						}
					}
					// the light border after the last run
					if (colour)
					{
						addHistory(run, history);
						run = 0;
					}
					addHistory(run + size, history);
					penalty += finders(history) * 40;
				}
			int dark = 0;
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
				{
					dark += m.Get(x, y);
					if (x + 1 < size && y + 1 < size)
					{
						const bool c = m.Get(x, y);
						if (c == m.Get(x + 1, y) && c == m.Get(x, y + 1) && c == m.Get(x + 1, y + 1))
							penalty += 3;
					}
				}
			const long total = (long)size * size;
			const long k = (std::labs(dark * 20L - total * 10L) + total - 1) / total - 1;
			return penalty + k * 10;
		}
	}

	QrCode::QrCode(std::string_view text)
	{
		Encode(text, -1);
	}

	QrCode QrCode::WithMask(std::string_view text, int mask)
	{
		QrCode code;
		code.Encode(text, std::clamp(mask, 0, 7));
		return code;
	}

	bool QrCode::Encode(std::string_view text, int chosenMask)
	{
		// the smallest version the text fits
		int version = 1;
		for (; version <= kMaxVersion; version++)
		{
			const int countBits = version < 10 ? 8 : 16;
			if (4 + countBits + (int)text.size() * 8 <= DataCodewords(version) * 8)
				break;
		}
		if (version > kMaxVersion)
			return false;

		// the data: byte mode, the length, the bytes, the terminator, then the pad codewords
		std::vector<uint8_t> data;
		uint32_t buffer = 0;
		int buffered = 0;
		auto put = [&](uint32_t value, int bits) {
			for (int i = bits - 1; i >= 0; i--)
			{
				buffer = buffer << 1 | ((value >> i) & 1);
				if (++buffered == 8)
				{
					data.push_back((uint8_t)buffer);
					buffer = 0;
					buffered = 0;
				}
			}
		};
		const int capacity = DataCodewords(version);
		put(0x4, 4);
		put((uint32_t)text.size(), version < 10 ? 8 : 16);
		for (char c : text)
			put((uint8_t)c, 8);
		const int used = (int)data.size() * 8 + buffered;
		put(0, std::min(4, capacity * 8 - used));
		if (buffered)
			put(0, 8 - buffered);
		for (uint8_t pad = 0xec; (int)data.size() < capacity; pad ^= 0xec ^ 0x11)
			data.push_back(pad);

		// in blocks, each with its error correction, then interleaved
		const int blocks = kBlocks[version], ecc = kEccPerBlock[version];
		const int raw = RawModules(version) / 8;
		const int shortBlocks = blocks - raw % blocks, shortLength = raw / blocks;
		const std::vector<uint8_t> divisor = Divisor(ecc);
		std::vector<std::vector<uint8_t>> parts;
		for (int i = 0, at = 0; i < blocks; i++)
		{
			const int length = shortLength - ecc + (i < shortBlocks ? 0 : 1);
			std::vector<uint8_t> block(data.begin() + at, data.begin() + at + length);
			at += length;
			const std::vector<uint8_t> correction = Remainder(block, divisor);
			if (i < shortBlocks)
				block.push_back(0); // a place holder, skipped below, so every block is as long
			block.insert(block.end(), correction.begin(), correction.end());
			parts.push_back(std::move(block));
		}
		std::vector<uint8_t> codewords;
		for (size_t i = 0; i < parts[0].size(); i++)
			for (int j = 0; j < blocks; j++)
				if (i != (size_t)(shortLength - ecc) || j >= shortBlocks)
					codewords.push_back(parts[j][i]);

		// the patterns
		const int size = version * 4 + 17;
		Matrix m(size);
		for (int i = 0; i < size; i++)
		{
			m.Set(6, i, i % 2 == 0);
			m.Set(i, 6, i % 2 == 0);
		}
		Finder(m, 3, 3);
		Finder(m, size - 4, 3);
		Finder(m, 3, size - 4);
		const std::vector<int> alignment = AlignmentPositions(version);
		const int count = (int)alignment.size();
		for (int i = 0; i < count; i++)
			for (int j = 0; j < count; j++)
			{
				if ((i == 0 && j == 0) || (i == 0 && j == count - 1) || (i == count - 1 && j == 0))
					continue; // where the finders are
				for (int dy = -2; dy <= 2; dy++)
					for (int dx = -2; dx <= 2; dx++)
						m.Set(alignment[i] + dx, alignment[j] + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
			}
		FormatBits(m, 0); // reserves the format's modules; written again with the mask
		VersionBits(m, version);

		// the codewords, in two-column zigzags from the bottom right, skipping the timing column
		size_t bit = 0;
		for (int right = size - 1; right >= 1; right -= 2)
		{
			if (right == 6)
				right = 5;
			for (int vertical = 0; vertical < size; vertical++)
				for (int j = 0; j < 2; j++)
				{
					const int x = right - j;
					const bool upward = ((right + 1) & 2) == 0;
					const int y = upward ? size - 1 - vertical : vertical;
					if (m.Fixed(x, y) || bit >= codewords.size() * 8)
						continue;
					m.Dark(x, y) = (codewords[bit >> 3] >> (7 - (bit & 7))) & 1;
					bit++;
				}
		}

		// the mask: the one asked for, else the one with the lowest penalty
		auto apply = [&m, size](int mask) {
			for (int y = 0; y < size; y++)
				for (int x = 0; x < size; x++)
					if (!m.Fixed(x, y) && Masked(mask, x, y))
						m.Dark(x, y) ^= 1;
		};
		int mask = chosenMask;
		if (mask < 0)
		{
			long best = -1;
			for (int candidate = 0; candidate < 8; candidate++)
			{
				apply(candidate);
				FormatBits(m, candidate);
				const long penalty = Penalty(m);
				if (best < 0 || penalty < best)
				{
					best = penalty;
					mask = candidate;
				}
				apply(candidate); // undone (XOR)
			}
		}
		apply(mask);
		FormatBits(m, mask);

		m_size = size;
		m_version = version;
		m_mask = mask;
		m_modules = std::move(m.dark);
		return true;
	}

	void DrawQr(Canvas& canvas, const QrCode& code, const Box& box, float radius, uint32_t dark, uint32_t light)
	{
		const int size = code.Size();
		canvas.Rect(box, radius, light);
		if (size == 0)
			return;
		// four modules of quiet zone around it, as the standard asks
		const float module = std::min(box.w, box.h) / (size + 8);
		const float x0 = box.x + (box.w - module * size) * 0.5f, y0 = box.y + (box.h - module * size) * 0.5f;
		for (int y = 0; y < size; y++)
			for (int x = 0; x < size;)
			{
				if (!code.Dark(x, y))
				{
					x++;
					continue;
				}
				int end = x;
				while (end < size && code.Dark(end, y))
					end++;
				// a hair taller and wider, so neighbouring squares meet without a seam
				canvas.Rect({x0 + x * module, y0 + y * module, (end - x) * module + 0.25f, module + 0.25f}, 0, dark);
				x = end;
			}
	}
}
