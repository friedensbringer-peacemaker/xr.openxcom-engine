#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <cmath>

namespace OpenXcom
{
namespace Xr
{

// Logic of the mandatory VR menu entries (quit with visible confirmation, pointer visibility).
// No drawing, no SDL: unit-tested on the host (tests/xr_unit_tests.cpp).
// TODO xrc/menu.h nach Tag: same behaviour as xrc::ConfirmAction / xrc::PointerVisibility in
// _shared/xr-common (not tagged yet) - replace this copy once the library is synced.

/// Seconds the quit button stays armed after the first click.
const float ConfirmTimeout = 3.0f;

/**
 * Two-click confirmation for a dangerous action (quit). The first click arms the
 * button (shown filled, "SICHER? NOCHMAL KLICKEN", double haptic pulse), a second
 * click within ConfirmTimeout confirms. Without a second click the button falls back;
 * update() reports that so the menu redraws it.
 */
class ConfirmAction
{
public:
	enum Result { NONE, ARMED, CONFIRMED };

	Result click()
	{
		if (_armed)
		{
			_armed = false;
			_left = 0.0f;
			return CONFIRMED;
		}
		_armed = true;
		_left = ConfirmTimeout;
		return ARMED;
	}
	/// Advances the timer; true = fell back from armed to idle in this call (redraw).
	bool update(float dt)
	{
		if (!_armed)
			return false;
		if (!(dt > 0.0f) || !std::isfinite(dt))
			dt = 0.0f;
		_left -= dt;
		if (_left > 0.0f)
			return false;
		_armed = false;
		_left = 0.0f;
		return true;
	}
	/// Menu closed, other page, other button: never carry the armed state into a new context.
	void cancel()
	{
		_armed = false;
		_left = 0.0f;
	}
	bool armed() const { return _armed; }
	float remaining() const { return _armed && _left > 0.0f ? _left : 0.0f; }

private:
	float _left = 0.0f;
	bool _armed = false;
};

/// Ray and target dot stay visible while a VR overlay needs them (VR menu, VR keyboard).
inline bool showRay(bool setting, bool overlayOpen) { return overlayOpen || setting; }
inline bool showMarker(bool setting, bool overlayOpen) { return overlayOpen || setting; }
/// The game's own mouse cursor only follows its switch.
inline bool showGameCursor(bool setting, bool suppressed) { return setting && !suppressed; }

/// A focusable control in menu pixels (for stick navigation).
struct FocusRect
{
	int x = 0, y = 0, w = 0, h = 0;
	bool visible = true;
};

/**
 * Stick/arrow navigation between the controls of a menu page: the nearest
 * visible control in direction (dx, dy) (one of them is 0, the other +-1).
 * Rows win over columns: moving down prefers the next row, then the control
 * closest to the current column. Returns `current` when nothing lies that way,
 * the first visible control when `current` is invalid, -1 when nothing is visible.
 */
inline int nextFocus(const FocusRect *rects, int count, int current, int dx, int dy)
{
	if (current < 0 || current >= count || !rects[current].visible)
	{
		for (int i = 0; i < count; ++i)
			if (rects[i].visible)
				return i;
		return -1;
	}
	const FocusRect &c = rects[current];
	const int cx = 2 * c.x + c.w, cy = 2 * c.y + c.h; // doubled centres (integer)
	int best = current;
	long bestScore = 0;
	for (int i = 0; i < count; ++i)
	{
		const FocusRect &r = rects[i];
		if (i == current || !r.visible)
			continue;
		const int rx = 2 * r.x + r.w, ry = 2 * r.y + r.h;
		const int along = dx != 0 ? (rx - cx) * dx : (ry - cy) * dy;   // distance in the wanted direction
		const int across = dx != 0 ? (ry - cy) : (rx - cx);             // sideways offset
		if (along <= 0)
			continue;
		const long score = static_cast<long>(along) * 4 + static_cast<long>(across < 0 ? -across : across);
		if (best == current || score < bestScore)
		{
			best = i;
			bestScore = score;
		}
	}
	return best;
}

/// Hold-to-repeat for steppers and stick navigation: the caller does the first step, then after
/// `delay` every `interval` seconds while held.
class HoldRepeat
{
public:
	HoldRepeat(float delay = 0.45f, float interval = 0.12f) : _delay(delay), _interval(interval), _next(delay) {}
	/// Number of extra steps due in this frame (0 or more) while `held`; resets when released.
	int update(bool held, float dt)
	{
		if (!held)
		{
			_time = 0.0f;
			_next = _delay;
			return 0;
		}
		if (!(dt > 0.0f) || !std::isfinite(dt))
			dt = 0.0f;
		_time += dt;
		int steps = 0;
		while (_time >= _next && steps < 8)
		{
			++steps;
			_next += _interval;
		}
		return steps;
	}

private:
	float _delay, _interval, _time = 0.0f, _next = 0.0f;
};

}
}
