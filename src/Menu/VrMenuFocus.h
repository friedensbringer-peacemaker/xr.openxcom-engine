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
#include <SDL.h>
#include <string>
#include <vector>
#include "../Engine/Xr/XrMenuLogic.h"

namespace OpenXcom
{

class Action;
class Game;
class State;
class Surface;
class TextButton;

/// Text of the VR menus from the language files (Language/XR/*.yml); the German
/// fallback keeps the menu readable if the data folder was not updated yet.
std::string vrText(const State *state, const char *id, const char *fallback);

/**
 * Stick/A/B operation of a VR menu page (xr-vr-menu P2). The pointer stick
 * arrives as arrow keys, A as Return, B as Escape (XrRuntime face buttons).
 * Arrows move a visible focus frame between the buttons (hold = repeat), A
 * clicks the focused button through the normal mouse handlers; the first A
 * only shows the focus, so a stray A never triggers an action unseen.
 * B stays bound to the page's cancel button (keyCancel).
 */
class VrMenuFocus
{
private:
	State *_state;
	Game *_game;
	Surface *_frame;
	std::vector<TextButton*> _items;
	int _focus;
	bool _shown;
	int _heldDx, _heldDy;
	SDL_Scancode _heldKey;
	Xr::HoldRepeat _repeat;
	Uint32 _lastTicks;
	void move(int dx, int dy);
	void redraw();
public:
	VrMenuFocus();
	/// Creates the focus frame; call after add()/centerAllSurfaces() of all buttons.
	void create(State *state, Game *game);
	/// Buttons in reading order; the first visible one gets the focus.
	void add(TextButton *button);
	/// Handles arrow keys and A (Return); true = the event was used.
	bool handle(Action *action);
	/// Hold repeat of the arrows; keeps the frame on a visible button.
	void think();
	/// Hides the frame (and drops the focus to the first button) e.g. after buttons changed.
	void reset();
	/// Shows the focus on the first visible button at once (pause prompt: A = continue).
	void showFirst() { move(0, 0); }
	/// Clicks a button like the left mouse button (press, release, click).
	static void click(State *state, Game *game, TextButton *button);
	/// The button with the visible focus, 0 while the frame is hidden.
	TextButton *focused() const;
};

}
