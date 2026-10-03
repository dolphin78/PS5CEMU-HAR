// SPDX-License-Identifier: GPL-3.0-or-later
#include "PS5PadController.h"
#include "../app/ingame.h"
#include "../ps5/pad.h"
#include "../stream/gamepad.h"

#include <array>
#include <atomic>

namespace
{
	// The Wii Remote pointer's reach: how far the DualSense turns (radians) to cross the screen
	constexpr float kAimAcross = 0.70f, kAimDown = 0.42f;
	std::array<std::atomic<bool>, ps5pad::kMaxPlayers> s_wiiRemote{};

	float StickAxis(uint8 raw)
	{
		// 0..255 with 128 at rest, down and right positive (as SDL reports them)
		return std::clamp((static_cast<int>(raw) - 128) / (raw < 128 ? 128.0f : 127.0f), -1.0f, 1.0f);
	}
}

PS5PadController::PS5PadController(int player)
	: base_type(fmt::format("{}", player), fmt::format("DualSense (player {})", player + 1)), m_player(player)
{
}

PS5PadController::PS5PadController(std::string_view uuid, std::string_view display_name)
	: base_type(uuid, display_name), m_player(std::clamp(ConvertString<int>(uuid), 0, ps5pad::kMaxPlayers - 1))
{
}

bool PS5PadController::is_connected()
{
	return ps5pad::IsConnected(m_player);
}

ControllerState PS5PadController::raw_state()
{
	ControllerState result{};
	ps5pad::Data data;
	if (!ps5pad::Read(m_player, data))
	{
		m_touching = false;
		return result;
	}

	const ps5pad::Filtered filtered = ps5pad::FilterShortcuts(m_player, data.buttons);

	// the touchpad as the GamePad's touch screen: the finger places the cursor, a click touches
	const bool finger = data.touchCount > 0;
	if (finger)
	{
		float width, height;
		ps5pad::TouchResolution(m_player, width, height);
		m_cursor = {std::clamp(data.touch[0].x / width, 0.0f, 1.0f), std::clamp(data.touch[0].y / height, 0.0f, 1.0f)};
	}
	if (m_player == 0)
		ps5ingame::SetGamePadPointer(finger, m_cursor.x, m_cursor.y, filtered.touch);

	// the menu and Cemu's keyboard take the controller while they are up
	if (ps5ingame::OverlayTakesInput())
	{
		m_touching = false;
		return result;
	}

	const uint32 buttons = filtered.buttons;
	static constexpr std::pair<uint32, uint64> kButtons[] = {
		{ps5pad::kCross, kCross}, {ps5pad::kCircle, kCircle}, {ps5pad::kSquare, kSquare}, {ps5pad::kTriangle, kTriangle},
		{ps5pad::kCreate, kCreate}, {ps5pad::kOptions, kOptions}, {ps5pad::kL3, kL3}, {ps5pad::kR3, kR3},
		{ps5pad::kL1, kL1}, {ps5pad::kR1, kR1}, {ps5pad::kUp, kDpadUp}, {ps5pad::kDown, kDpadDown},
		{ps5pad::kLeft, kDpadLeft}, {ps5pad::kRight, kDpadRight},
	};
	for (const auto& [mask, id] : kButtons)
		result.buttons.SetButtonState((uint32)id, (buttons & mask) != 0);

	result.axis = {StickAxis(data.leftX), StickAxis(data.leftY)};
	result.rotation = {StickAxis(data.rightX), StickAxis(data.rightY)};
	result.trigger = {data.l2 / 255.0f, data.r2 / 255.0f};

	m_previousTouch = m_touch;
	m_touching = filtered.touch;
	if (m_touching)
		m_touch = m_cursor;
	// a Wii Remote's pointer: where a finger rests, else aimed from there by the gyroscope (below)
	m_previousAim = m_aim;
	if (finger)
		m_aim = m_cursor;

	if (m_player == 0)
	{
		float x, y;
		if (ps5stream::Touch(x, y))
		{
			m_touching = true;
			m_touch = {x, y};
		}
	}

	// motion, in the axes and units Cemu's SDL gamepads use (acceleration in g, rotation in
	// radians per second). The DualSense axes as libScePad reports them need checking on a console.
	if (data.timestampUs != m_lastMotionTimestamp)
	{
		std::lock_guard lock(m_motionMutex);
		const float deltaTime = m_lastMotionTimestamp ? (data.timestampUs - m_lastMotionTimestamp) / 1000000.0f : 0.0f;
		m_lastMotionTimestamp = data.timestampUs;
		if (deltaTime > 0.0f && deltaTime < 0.5f)
		{
			const glm::vec3 acc{-data.acceleration[0], -data.acceleration[1], -data.acceleration[2]};
			const glm::vec3 gyro{data.angularVelocity[0], -data.angularVelocity[1], -data.angularVelocity[2]};
			m_motion.processMotionSample(deltaTime, gyro.x, gyro.y, gyro.z, acc.x, -acc.y, -acc.z);
			m_motionSample = m_motion.getMotionSample();
			// the pointer, without a finger on the touchpad: turning left (about the controller's up
			// axis) moves it left, tilting its front up (about its right axis) moves it up
			if (!finger && IsPointer())
			{
				m_aim.x = std::clamp(m_aim.x - data.angularVelocity[1] * deltaTime / kAimAcross, 0.0f, 1.0f);
				m_aim.y = std::clamp(m_aim.y - data.angularVelocity[0] * deltaTime / kAimDown, 0.0f, 1.0f);
			}
		}
	}
	return result;
}

MotionSample PS5PadController::get_motion_sample()
{
	std::lock_guard lock(m_motionMutex);
	return m_motionSample;
}

void PS5PadController::SetWiiRemote(int player, bool wiiRemote)
{
	if (player >= 0 && player < ps5pad::kMaxPlayers)
		s_wiiRemote[player] = wiiRemote;
}

bool PS5PadController::IsPointer()
{
	const bool pointer = s_wiiRemote[m_player];
	if (pointer && !m_pointer)
		m_aim = m_previousAim = {0.5f, 0.5f}; // a Wii Remote now: it points at the middle
	m_pointer = pointer;
	return pointer;
}

bool PS5PadController::has_position()
{
	// a Wii Remote always points somewhere on the screen; the GamePad's screen is touched only
	// while the touchpad is clicked
	return IsPointer() || m_touching;
}

glm::vec2 PS5PadController::get_position()
{
	return IsPointer() ? m_aim : m_touch;
}

glm::vec2 PS5PadController::get_prev_position()
{
	return IsPointer() ? m_previousAim : m_previousTouch;
}

PositionVisibility PS5PadController::GetPositionVisibility()
{
	return IsPointer() || m_touching ? PositionVisibility::FULL : PositionVisibility::NONE;
}

void PS5PadController::start_rumble()
{
	const float strength = std::clamp(get_settings().rumble, 0.0f, 1.0f);
	if (strength <= 0.0f)
		return;
	const auto motor = static_cast<uint8>(strength * 255.0f);
	ps5pad::SetVibration(m_player, motor, motor);
}

void PS5PadController::stop_rumble()
{
	ps5pad::SetVibration(m_player, 0, 0);
}

std::string PS5PadController::get_button_name(uint64 button) const
{
	switch (button)
	{
	case kCross: return "Cross";
	case kCircle: return "Circle";
	case kSquare: return "Square";
	case kTriangle: return "Triangle";
	case kCreate: return "Create";
	case kOptions: return "Options";
	case kL3: return "L3";
	case kR3: return "R3";
	case kL1: return "L1";
	case kR1: return "R1";
	case kDpadUp: return "D-pad up";
	case kDpadDown: return "D-pad down";
	case kDpadLeft: return "D-pad left";
	case kDpadRight: return "D-pad right";
	case kTriggerXP: return "L2";
	case kTriggerYP: return "R2";
	case kAxisYN: return "Left stick up";
	case kAxisYP: return "Left stick down";
	case kAxisXN: return "Left stick left";
	case kAxisXP: return "Left stick right";
	case kRotationYN: return "Right stick up";
	case kRotationYP: return "Right stick down";
	case kRotationXN: return "Right stick left";
	case kRotationXP: return "Right stick right";
	default: return ControllerBase::get_button_name(button);
	}
}
