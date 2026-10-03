// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>
namespace ps5stream
{
// Encodes at quality 75, bounding the picture to the GamePad's native 854x480.
std::vector<uint8_t> EncodeJPEG(const std::vector<uint8_t>& rgb, int width, int height);
}
