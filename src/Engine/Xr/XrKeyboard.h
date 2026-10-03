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
#include "XrUi.h"
#include <string>
#include <vector>

namespace OpenXcom
{
namespace Xr
{

/**
 * On-screen QWERTZ keyboard for the immersive mode. It is shown while the
 * game has text input active (TextEdit enables SDL_TEXTINPUT) and is drawn
 * on the CPU into a Canvas that the XR thread uploads to its own layer.
 */
class Keyboard
{
public:
	static constexpr int Width = 1024;
	static constexpr int Height = 400;

	enum ActionType { ACTION_NONE, ACTION_TEXT, ACTION_BACKSPACE, ACTION_ENTER, ACTION_ESCAPE, ACTION_SHIFT };
	struct Action
	{
		ActionType type = ACTION_NONE;
		std::string text;
	};

	Keyboard();
	/// Key index under panel coordinates (0..1), or -1.
	int keyAt(float u, float v) const;
	void setHover(int key);
	void setPressed(int key);
	/// Triggers a key; shift is released after one character.
	Action activate(int key);
	/// Clears shift and highlight state (keyboard hidden).
	void reset();
	/// Redraws into the canvas if anything changed.
	bool render(Canvas &canvas);

private:
	struct Key
	{
		std::string lower, upper;
		ActionType type;
	};
	std::vector<UiButton> _buttons; // command = index into _keys
	std::vector<Key> _keys;
	int _hover, _pressed;
	bool _shift, _dirty;

	void addRow(int y, int x, const char *const *labels, const char *const *lower, const char *const *upper, int count);
	void addKey(int x, int y, int w, const std::string &label, const std::string &lower, const std::string &upper, ActionType type);
};

}
}
