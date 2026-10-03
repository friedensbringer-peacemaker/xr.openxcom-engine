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
#include "../Engine/State.h"
#include "../Engine/Xr/XrMenuLogic.h"
#include "OptionsBaseState.h"
#include "VrMenuFocus.h"

namespace OpenXcom
{

class Game;
class TextButton;
class Window;
class Text;

/**
 * VR quick menu of the Quest port (left menu button). A regular game dialog
 * in the style of the pause menu, so it looks and behaves like the game's own
 * menus; while it is open the Geoscape clock and the battle stand still.
 * Mandatory entries (skill xr-vr-menu P3): continue, recenter, VR options
 * (pointer switches, passthrough) and quit with a visible two-click confirmation.
 */
class VrMenuState : public State
{
private:
	OptionsOrigin _origin;
	bool _resumePrompt, _quitPending;
	Window *_window;
	Text *_txtTitle;
	TextButton *_btnResume, *_btnQuickSave, *_btnQuickLoad, *_btnSpeed[6], *_btnTempo[3], *_btnGameMenu, *_btnRecenter, *_btnTable, *_btnOptions, *_btnQuit, *_btnCancel;
	TextButton *_selTempo, *_selQuit;
	Xr::ConfirmAction _quitConfirm;
	Uint32 _lastTicks;
	VrMenuFocus _focus;
	void updateTableButton();
	void updateTempoButtons();
	/// Shows the quit button idle or armed (filled, "SURE? CLICK AGAIN").
	void updateQuitButton();
	/// Second click: save settings, autosave by the game's rules, then quit.
	void quit();
public:
	/// Creates the VR menu; resumePrompt = shown after a break (headset off).
	VrMenuState(OptionsOrigin origin, bool resumePrompt);
	~VrMenuState();
	/// The origin that fits the current game state.
	static OptionsOrigin currentOrigin(Game *game);
	/// False while a battle is busy (alien turn, animations) - like the pause menu's save rule.
	static bool canOpen(Game *game);
	/// Refreshes the table button (settings may change in the options page); finishes a pending quit.
	void init() override;
	/// Stick/A focus and the quit confirmation timeout.
	void handle(Action *action) override;
	void think() override;
	void btnClick(Action *action);
	void btnQuitClick(Action *action);
	void btnCancelClick(Action *action);
	/// Buttons the Quest self-test clicks through the controller input path.
	TextButton *getOptionsButton() const { return _btnOptions; }
	TextButton *getCancelButton() const { return _btnCancel; }
	TextButton *getQuitButton() const { return _btnQuit; }
};

}
