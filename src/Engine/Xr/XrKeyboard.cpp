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
#include "XrKeyboard.h"

namespace OpenXcom
{
namespace Xr
{

namespace
{
const int Margin = 12, Gap = 8, KeyH = 68, KeyW = 76;
}

Keyboard::Keyboard() : _hover(-1), _pressed(-1), _shift(false), _dirty(true)
{
	static const char *const row0[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\xC3\x9F"};
	static const char *const row0u[] = {"!", "\"", "\xC2\xA7", "$", "%", "&", "/", "(", ")", "=", "?"};
	static const char *const row1[] = {"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\xC3\xBC"};
	static const char *const row1u[] = {"Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\xC3\x9C"};
	static const char *const row2[] = {"a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB6", "\xC3\xA4"};
	static const char *const row2u[] = {"A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x96", "\xC3\x84"};
	static const char *const row3[] = {"y", "x", "c", "v", "b", "n", "m", ",", ".", "-"};
	static const char *const row3u[] = {"Y", "X", "C", "V", "B", "N", "M", ";", ":", "_"};

	const int pitchY = KeyH + Gap, pitchX = KeyW + Gap;
	addRow(Margin, Margin, row0, row0, row0u, 11);
	addKey(Margin + 11 * pitchX, Margin, Width - Margin - (Margin + 11 * pitchX), std::string(1, GlyphBack), "", "", ACTION_BACKSPACE);
	addRow(Margin + pitchY, Margin + pitchX / 2, row1, row1, row1u, 11);
	addRow(Margin + 2 * pitchY, Margin + pitchX * 3 / 4, row2, row2, row2u, 11);
	const int shiftW = KeyW * 3 / 2;
	addKey(Margin, Margin + 3 * pitchY, shiftW, std::string(1, GlyphShift), "", "", ACTION_SHIFT);
	addRow(Margin + 3 * pitchY, Margin + shiftW + Gap, row3, row3, row3u, 10);
	const int y4 = Margin + 4 * pitchY;
	addKey(Margin, y4, 180, "ESC", "", "", ACTION_ESCAPE);
	addKey(Margin + 180 + Gap, y4, 560, "", " ", " ", ACTION_TEXT);
	addKey(Margin + 740 + 2 * Gap, y4, Width - Margin - (Margin + 740 + 2 * Gap), "OK", "", "", ACTION_ENTER);
}

void Keyboard::addRow(int y, int x, const char *const *labels, const char *const *lower, const char *const *upper, int count)
{
	for (int i = 0; i < count; ++i)
		addKey(x + i * (KeyW + Gap), y, KeyW, labels[i], lower[i], upper[i], ACTION_TEXT);
}

void Keyboard::addKey(int x, int y, int w, const std::string &label, const std::string &lower, const std::string &upper, ActionType type)
{
	_buttons.push_back({x, y, w, KeyH, label, static_cast<int>(_keys.size()), false, true});
	_keys.push_back({lower, upper, type});
}

int Keyboard::keyAt(float u, float v) const
{
	return buttonAt(_buttons, Width, Height, u, v);
}

void Keyboard::setHover(int key)
{
	if (key != _hover)
	{
		_hover = key;
		_dirty = true;
	}
}

void Keyboard::setPressed(int key)
{
	if (key != _pressed)
	{
		_pressed = key;
		_dirty = true;
	}
}

Keyboard::Action Keyboard::activate(int key)
{
	Action action;
	if (key < 0 || key >= static_cast<int>(_keys.size()))
		return action;
	const Key &k = _keys[key];
	action.type = k.type;
	if (k.type == ACTION_SHIFT)
	{
		_shift = !_shift;
		_dirty = true;
	}
	else if (k.type == ACTION_TEXT)
	{
		action.text = _shift ? k.upper : k.lower;
		if (_shift)
		{
			_shift = false;
			_dirty = true;
		}
	}
	return action;
}

void Keyboard::reset()
{
	_hover = _pressed = -1;
	_shift = false;
	_dirty = true;
}

bool Keyboard::render(Canvas &canvas)
{
	if (!_dirty)
		return false;
	_dirty = false;
	canvas.fill(0, 0, Width, Height, Colors::Background);
	for (size_t i = 0; i < _buttons.size(); ++i)
	{
		const UiButton &b = _buttons[i];
		const int idx = static_cast<int>(i);
		uint32_t color = Colors::Button;
		if (_keys[i].type == ACTION_SHIFT && _shift)
			color = Colors::Selected;
		if (idx == _hover)
			color = Colors::Hover;
		if (idx == _pressed)
			color = Colors::Pressed;
		canvas.fill(b.x, b.y, b.w, b.h, color);
		canvas.text(b.label, b.x + b.w / 2.0f, b.y + b.h / 2.0f, 6, b.w - 12.0f, Colors::Text, true);
	}
	return true;
}

}
}
