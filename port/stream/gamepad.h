// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace ps5stream {
void Configure(bool enabled);
std::string Status();
bool WantFrame(); // render thread: active viewers or snapshot demand, limited to 30 Hz
void SubmitFrame(const std::vector<uint8_t>& rgb, int width, int height);
bool Touch(float& x, float& y); // expires after one second without a heartbeat
}
