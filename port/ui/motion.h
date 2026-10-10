// SPDX-License-Identifier: GPL-3.0-or-later
// PS5CEMU-HAR's UI kit: motion (docs/UI-REDESIGN.md, 8.1). Springs, solved exactly so any frame time
// is stable, and easings for what has a set duration. Everything advances by the measured frame
// time, clamped to 50 ms by the caller, so a hitch costs one late frame and no jump.

#pragma once

#include <algorithm>
#include <cmath>

namespace ui
{
	// Critically damped by default; retargeting keeps the velocity, so a held direction stays smooth.
	struct Spring
	{
		float value = 0, velocity = 0, target = 0, omega = 20;

		void Update(float dt)
		{
			// x(t) = target + (x0 + c t) e^(-wt), c = v0 + w x0, with x0 the distance from the target
			const float x0 = value - target;
			const float c = velocity + omega * x0;
			const float decay = std::exp(-omega * dt);
			value = target + (x0 + c * dt) * decay;
			velocity = (c - omega * (x0 + c * dt)) * decay;
			if (std::fabs(value - target) < 1e-3f && std::fabs(velocity) < 1e-2f)
			{
				value = target;
				velocity = 0;
			}
		}

		void Snap(float to)
		{
			value = target = to;
			velocity = 0;
		}

		bool Settled() const { return value == target && velocity == 0; }
	};

	namespace ease
	{
		inline float Clamp(float t) { return std::clamp(t, 0.0f, 1.0f); }
		inline float QuintOut(float t)
		{
			t = 1 - Clamp(t);
			return 1 - t * t * t * t * t;
		}
		inline float CubicOut(float t)
		{
			t = 1 - Clamp(t);
			return 1 - t * t * t;
		}
		inline float CubicIn(float t)
		{
			t = Clamp(t);
			return t * t * t;
		}
		inline float InOut(float t)
		{
			t = Clamp(t);
			return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
		}
		// a small overshoot (the toggles' thumbs)
		inline float BackOut(float t)
		{
			t = Clamp(t) - 1;
			constexpr float s = 1.70158f;
			return 1 + t * t * ((s + 1) * t + s);
		}
	}

	// A value going from one to another over a time, eased
	struct Tween
	{
		float from = 0, to = 0;
		double start = -1e9;
		float duration = 0.2f;
		float (*curve)(float) = ease::QuintOut;

		void Go(float target, double now, float seconds, float (*easing)(float) = ease::QuintOut)
		{
			from = Value(now);
			to = target;
			start = now;
			duration = seconds;
			curve = easing;
		}
		void Snap(float value)
		{
			from = to = value;
			start = -1e9;
		}
		float Value(double now) const
		{
			if (duration <= 0 || now - start >= duration)
				return to;
			return from + (to - from) * curve((float)((now - start) / duration));
		}
		bool Done(double now) const { return now - start >= duration; }
	};
}
