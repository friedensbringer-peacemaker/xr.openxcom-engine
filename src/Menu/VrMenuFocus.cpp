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
#include "VrMenuFocus.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Screen.h"
#include "../Engine/State.h"
#include "../Engine/Surface.h"
#include "../Interface/TextButton.h"

namespace OpenXcom
{

std::string vrText(const State *state, const char *id, const char *fallback)
{
	const std::string text = state->tr(id);
	return text == id ? std::string(fallback) : text;
}

VrMenuFocus::VrMenuFocus() : _state(0), _game(0), _frame(0), _focus(-1), _shown(false), _heldDx(0), _heldDy(0),
	_heldKey(SDL_SCANCODE_UNKNOWN), _lastTicks(0)
{
}

void VrMenuFocus::create(State *state, Game *game)
{
	_state = state;
	_game = game;
	SDL_Surface *screen = game->getScreen()->getSurface();
	// One transparent overlay over the whole page: the frame can jump between buttons of any size.
	_frame = new Surface(screen->w, screen->h, 0, 0);
	state->add(_frame);
	_frame->setVisible(false);
	_lastTicks = SDL_GetTicks();
}

void VrMenuFocus::add(TextButton *button)
{
	_items.push_back(button);
}

TextButton *VrMenuFocus::focused() const
{
	return _shown && _focus >= 0 && _focus < static_cast<int>(_items.size()) ? _items[_focus] : 0;
}

void VrMenuFocus::reset()
{
	_shown = false;
	_focus = -1;
	if (_frame)
		_frame->setVisible(false);
}

void VrMenuFocus::move(int dx, int dy)
{
	std::vector<Xr::FocusRect> rects(_items.size());
	for (size_t i = 0; i < _items.size(); ++i)
	{
		rects[i].x = _items[i]->getX();
		rects[i].y = _items[i]->getY();
		rects[i].w = _items[i]->getWidth();
		rects[i].h = _items[i]->getHeight();
		rects[i].visible = _items[i]->getVisible();
	}
	const int count = static_cast<int>(rects.size());
	if (!_shown || _focus < 0 || _focus >= count || !rects[_focus].visible)
		_focus = Xr::nextFocus(rects.data(), count, -1, 0, 0); // first press only shows the focus
	else
		_focus = Xr::nextFocus(rects.data(), count, _focus, dx, dy);
	_shown = _focus >= 0;
	redraw();
}

void VrMenuFocus::redraw()
{
	if (!_frame)
		return;
	TextButton *button = focused();
	_frame->setVisible(button != 0);
	if (!button)
		return;
	_frame->clear();
	// Two-pixel outline just outside the button, in the button's brightest colour.
	const Uint8 color = button->getColor();
	const int x = button->getX() - 2, y = button->getY() - 2, w = button->getWidth() + 4, h = button->getHeight() + 4;
	_frame->drawRect(x, y, w, 2, color);
	_frame->drawRect(x, y + h - 2, w, 2, color);
	_frame->drawRect(x, y, 2, h, color);
	_frame->drawRect(x + w - 2, y, 2, h, color);
}

bool VrMenuFocus::handle(Action *action)
{
	SDL_Event *ev = action->getDetails();
	if (ev->type != SDL_KEYDOWN || ev->key.repeat)
		return false;
	int dx = 0, dy = 0;
	switch (ev->key.keysym.sym)
	{
	case SDLK_UP: dy = -1; break;
	case SDLK_DOWN: dy = 1; break;
	case SDLK_LEFT: dx = -1; break;
	case SDLK_RIGHT: dx = 1; break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:
	{
		TextButton *button = focused();
		if (!button)
		{
			move(0, 0); // first A: show where the focus is
			return true;
		}
		click(_state, _game, button);
		return true;
	}
	default:
		return false;
	}
	_heldDx = dx;
	_heldDy = dy;
	_heldKey = ev->key.keysym.scancode;
	_repeat.update(false, 0.0f); // restart the hold timer
	move(dx, dy);
	return true;
}

void VrMenuFocus::think()
{
	const Uint32 now = SDL_GetTicks();
	const float dt = (now - _lastTicks) / 1000.0f;
	_lastTicks = now;
	const Uint8 *keys = SDL_GetKeyboardState(0);
	const bool held = _heldKey != SDL_SCANCODE_UNKNOWN && keys && keys[_heldKey];
	if (!held)
		_heldKey = SDL_SCANCODE_UNKNOWN;
	const int steps = _repeat.update(held, dt);
	for (int i = 0; i < steps; ++i)
		move(_heldDx, _heldDy);
	// A button that disappeared (e.g. tempo row) must not keep the frame.
	TextButton *button = focused();
	if (button && !button->getVisible())
		move(0, 0);
}

void VrMenuFocus::click(State *state, Game *game, TextButton *button)
{
	if (!button || !button->getVisible())
		return;
	SDL_Event ev;
	SDL_zero(ev);
	ev.type = SDL_MOUSEBUTTONDOWN;
	ev.button.button = SDL_BUTTON_LEFT;
	Screen *screen = game->getScreen();
	Action action(&ev, screen->getXScale(), screen->getYScale(), screen->getCursorTopBlackBand(), screen->getCursorLeftBlackBand());
	action.setSender(button);
	button->mousePress(&action, state);
	ev.type = SDL_MOUSEBUTTONUP;
	button->mouseRelease(&action, state);
	button->mouseClick(&action, state);
}

}
