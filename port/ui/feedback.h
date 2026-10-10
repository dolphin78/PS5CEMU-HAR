// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: what the UI answers with besides the picture (docs/UI-REDESIGN.md, 8.2 and
// 8.3): the menu's sounds (frontend/sound.h), the DualSense's rumble, and its light bar in the side's
// colour. Rumble is a light pulse for a refusal and a stronger one for a launch or a completed hold,
// never for ordinary navigation, and none when the vibration setting is off (ps5pad).

#pragma once

#include <cstdint>

namespace ui
{
	enum class Cue
	{
		Focus,	// the focus moved, or a value stepped
		Select, // opened, chose
		Back,	// closed, cancelled
		Denied, // Cross on what does nothing
		Edge,	// pushed past a list's end (once; silent while held)
		Toggle, // a switch flipped
		Sheet,	// a sheet or menu opened
		Notify, // a toast
		Launch, // a game starts
		Hold,	// a hold completed
	};

	class Feedback
	{
	public:
		void Play(Cue cue, bool repeat = false);
		// The light bar in a colour (RGBA, R lowest), on every controller
		void SetLight(uint32_t colour);
		// Stops a rumble once its time is up; now in seconds
		void Update(double now);
		// Motors still and the light bar left as it is (before a game)
		void Stop();

	private:
		void Rumble(uint8_t large, uint8_t small, double seconds);

		double m_now = 0, m_rumbleUntil = 0;
		bool m_rumbling = false;
	};
}
