// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace ps5stream { std::vector<uint8_t> EncodeConnectionQR(const std::string& url); std::string ConnectionQR(const std::string& url); }
