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
#include "VrOptionsState.h"
#include <cstdio>
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Xr/XrRuntime.h"
#include "../Interface/TextButton.h"
#include "../Interface/ToggleTextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Savegame/SavedGame.h"

namespace OpenXcom
{

namespace
{
// Layout grid (game pixels): label column, control column, one row per option, bottom line info + close.
const int WinX = 32, WinY = 2, WinW = 256, WinH = 196;
const int RowTop = 28, RowStep = 14, RowH = 12;
const int LabelX = WinX + 8, LabelW = 104, ControlX = WinX + 114;
const int BottomY = 182, BottomH = 14;
int row(int i) { return RowTop + i * RowStep; }
}

/**
 * Initializes all the elements of the VR options page.
 * @param origin Game section that originated this state.
 */
VrOptionsState::VrOptionsState(OptionsOrigin origin) : _origin(origin), _selPointer(0), _selSmooth(0), _selView(0), _selStick(0), _selUpscale(0),
	_holdButton(0), _infoTime(0), _lastTicks(0)
{
	_screen = false;

	// Create objects
	_window = new Window(this, WinW, WinH, WinX, WinY, POPUP_BOTH);
	_txtTitle = new Text(WinW - 10, 17, WinX + 5, 10);
	for (int i = 0; i < LabelCount; ++i)
	{
		_txtLabels[i] = new Text(LabelW, RowH, LabelX, row(i) + 2);
	}
	for (int i = 0; i < 2; ++i)
	{
		_btnPointer[i] = new TextButton(65, RowH, ControlX + i * 69, row(0));
		_btnView[i] = new TextButton(65, RowH, ControlX + i * 69, row(2));
		_btnStick[i] = new TextButton(65, RowH, ControlX + i * 69, row(7));
	}
	for (int i = 0; i < 3; ++i)
	{
		_btnSmooth[i] = new TextButton(42, RowH, ControlX + i * 46, row(1));
		_btnUpscale[i] = new TextButton(42, RowH, ControlX + i * 46, row(9));
	}
	for (int i = 0; i < SHOW_COUNT; ++i)
	{
		_btnShow[i] = new ToggleTextButton(42, RowH, ControlX + i * 46, row(10));
	}
	static const int stepperRows[STEP_COUNT] = {3, 4, 5, 6, 8};
	for (int i = 0; i < STEP_COUNT; ++i)
	{
		const int y = row(stepperRows[i]);
		_btnDec[i] = new TextButton(18, RowH, ControlX, y);
		_txtValue[i] = new Text(94, RowH, ControlX + 20, y + 2);
		_btnInc[i] = new TextButton(18, RowH, ControlX + 116, y);
	}
	// Recenter lives in the quick menu only (one entry, one place).
	_txtInfo = new Text(130, 9, LabelX, BottomY + 3);
	_btnOk = new TextButton(104, BottomH, WinX + 144, BottomY);

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	for (int i = 0; i < LabelCount; ++i)
	{
		add(_txtLabels[i], "text", "pauseMenu");
	}
	for (int i = 0; i < 2; ++i)
	{
		add(_btnPointer[i], "button", "pauseMenu");
		add(_btnView[i], "button", "pauseMenu");
		add(_btnStick[i], "button", "pauseMenu");
	}
	for (int i = 0; i < 3; ++i)
	{
		add(_btnSmooth[i], "button", "pauseMenu");
		add(_btnUpscale[i], "button", "pauseMenu");
	}
	for (int i = 0; i < SHOW_COUNT; ++i)
	{
		add(_btnShow[i], "button", "pauseMenu");
	}
	for (int i = 0; i < STEP_COUNT; ++i)
	{
		add(_btnDec[i], "button", "pauseMenu");
		add(_txtValue[i], "text", "pauseMenu");
		add(_btnInc[i], "button", "pauseMenu");
	}
	add(_txtInfo, "text", "pauseMenu");
	add(_btnOk, "button", "pauseMenu");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "pauseMenu");

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText(vrText(this, "STR_XR_VR_OPTIONS", "VR-OPTIONEN"));
	static const char *const labelIds[LabelCount][2] = {
		{"STR_XR_POINTER_HAND", "ZEIGERHAND"}, {"STR_XR_SMOOTHING", "GL\xC3\x84TTUNG"}, {"STR_XR_VIEW", "ANSICHT"},
		{"STR_XR_WIDTH", "BREITE"}, {"STR_XR_DISTANCE", "ABSTAND"}, {"STR_XR_TABLE_TILT", "TISCHNEIGUNG"},
		{"STR_XR_TABLE_HEIGHT", "TISCHH\xC3\x96HE"}, {"STR_XR_GRIP_STICK", "GRIFF+STICK"}, {"STR_XR_PASSTHROUGH", "PASSTHROUGH"},
		{"STR_XR_UPSCALING", "HOCHSKALIERUNG"}, {"STR_XR_SHOW", "ANZEIGEN"}};
	for (int i = 0; i < LabelCount; ++i)
	{
		_txtLabels[i]->setText(vrText(this, labelIds[i][0], labelIds[i][1]));
	}

	const std::string on = vrText(this, "STR_XR_ON", "AN"), off = vrText(this, "STR_XR_OFF", "AUS");
	const std::string pointerNames[2] = {vrText(this, "STR_XR_LEFT", "LINKS"), vrText(this, "STR_XR_RIGHT", "RECHTS")};
	const std::string viewNames[2] = {vrText(this, "STR_XR_CANVAS", "LEINWAND"), vrText(this, "STR_XR_TABLE", "TISCH")};
	const std::string stickNames[2] = {on, off};
	const std::string smoothNames[3] = {off, vrText(this, "STR_XR_LIGHT", "LEICHT"), vrText(this, "STR_XR_STRONG", "STARK")};
	for (int i = 0; i < 2; ++i)
	{
		_btnPointer[i]->setText(pointerNames[i]);
		_btnPointer[i]->setGroup(&_selPointer);
		_btnPointer[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
		_btnView[i]->setText(viewNames[i]);
		_btnView[i]->setGroup(&_selView);
		_btnView[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
		_btnStick[i]->setText(stickNames[i]);
		_btnStick[i]->setGroup(&_selStick);
		_btnStick[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
	}
	// The upscaler is applied to every frame (XrRenderer::flip): takes effect at once, no restart.
	const std::string upscaleNames[3] = {off, "XBRZ", "HQX"};
	for (int i = 0; i < 3; ++i)
	{
		_btnSmooth[i]->setText(smoothNames[i]);
		_btnSmooth[i]->setGroup(&_selSmooth);
		_btnSmooth[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
		_btnUpscale[i]->setText(upscaleNames[i]);
		_btnUpscale[i]->setGroup(&_selUpscale);
		_btnUpscale[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
	}
	// Pointer switches (xr-vr-menu P3): filled = shown. Ray and dot stay visible on the VR menu pages.
	const std::string showNames[SHOW_COUNT] = {vrText(this, "STR_XR_SHOW_RAY", "STRAHL"), vrText(this, "STR_XR_SHOW_MARKER", "PUNKT"),
		vrText(this, "STR_XR_SHOW_CURSOR", "MAUS")};
	for (int i = 0; i < SHOW_COUNT; ++i)
	{
		_btnShow[i]->setText(showNames[i]);
		_btnShow[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick);
		_btnShow[i]->onMouseClick((ActionHandler)&VrOptionsState::btnClick, SDL_BUTTON_RIGHT); // toggles on both buttons
	}
	for (int i = 0; i < STEP_COUNT; ++i)
	{
		_btnDec[i]->setText("-");
		_btnDec[i]->onMousePress((ActionHandler)&VrOptionsState::btnStepPress, SDL_BUTTON_LEFT);
		_btnDec[i]->onMouseRelease((ActionHandler)&VrOptionsState::btnStepRelease, SDL_BUTTON_LEFT);
		_btnInc[i]->setText("+");
		_btnInc[i]->onMousePress((ActionHandler)&VrOptionsState::btnStepPress, SDL_BUTTON_LEFT);
		_btnInc[i]->onMouseRelease((ActionHandler)&VrOptionsState::btnStepRelease, SDL_BUTTON_LEFT);
		_txtValue[i]->setAlign(ALIGN_CENTER);
	}

	_btnOk->setText(vrText(this, "STR_XR_CLOSE", "SCHLIESSEN"));
	_btnOk->onMouseClick((ActionHandler)&VrOptionsState::btnOkClick);
	// B closes; A goes through the stick focus.
	_btnOk->onKeyboardPress((ActionHandler)&VrOptionsState::btnOkClick, Options::keyCancel);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("pauseMenu");
	}

	_focus.create(this, _game);
	for (int i = 0; i < 2; ++i)
		_focus.add(_btnPointer[i]);
	for (int i = 0; i < 3; ++i)
		_focus.add(_btnSmooth[i]);
	for (int i = 0; i < 2; ++i)
		_focus.add(_btnView[i]);
	for (int i = 0; i < STEP_COUNT; ++i)
	{
		_focus.add(_btnDec[i]);
		_focus.add(_btnInc[i]);
	}
	for (int i = 0; i < 2; ++i)
		_focus.add(_btnStick[i]);
	for (int i = 0; i < 3; ++i)
		_focus.add(_btnUpscale[i]);
	for (int i = 0; i < SHOW_COUNT; ++i)
		_focus.add(_btnShow[i]);
	_focus.add(_btnOk);
	_lastTicks = SDL_GetTicks();
	refresh();
}

VrOptionsState::~VrOptionsState()
{
}

/**
 * Shows the current settings: selected choices and stepper values.
 */
void VrOptionsState::refresh()
{
	const Xr::VrSettings s = Xr::settings();
	const Xr::VrInfo info = Xr::info();
	_selPointer = _btnPointer[s.pointerHand ? 1 : 0];
	_selSmooth = _btnSmooth[s.smoothing];
	_selView = _btnView[s.view];
	_selStick = _btnStick[s.stickAdjust ? 0 : 1];
	// Upscaler state lives in the game's own options (read from the engine, not mirrored).
	_selUpscale = _btnUpscale[Options::useXBRZFilter ? 1 : Options::useHQXFilter ? 2 : 0];
	_btnShow[SHOW_RAY]->setPressed(s.showRay);
	_btnShow[SHOW_MARKER]->setPressed(s.showMarker);
	_btnShow[SHOW_CURSOR]->setPressed(s.showGameCursor);

	char text[64];
	std::snprintf(text, sizeof(text), "%.1f M", s.width[s.view]);
	_txtValue[STEP_WIDTH]->setText(text);
	std::snprintf(text, sizeof(text), "%.1f M", s.distance[s.view]);
	_txtValue[STEP_DISTANCE]->setText(text);
	std::snprintf(text, sizeof(text), "%d %s", s.tableTilt, vrText(this, "STR_XR_DEGREES", "GRAD").c_str());
	_txtValue[STEP_TILT]->setText(text);
	std::snprintf(text, sizeof(text), "%d %s", s.tableHeight, vrText(this, "STR_XR_CM_LOWER", "CM TIEFER").c_str());
	_txtValue[STEP_HEIGHT]->setText(text);
	if (info.passthroughAvailable)
	{
		std::snprintf(text, sizeof(text), "%d %%", s.passthrough);
		_txtValue[STEP_PASSTHROUGH]->setText(text);
	}
	else
	{
		_txtValue[STEP_PASSTHROUGH]->setText(vrText(this, "STR_XR_NOT_AVAILABLE", "NICHT VERF\xC3\x9CGBAR"));
	}
	_btnDec[STEP_PASSTHROUGH]->setVisible(info.passthroughAvailable);
	_btnInc[STEP_PASSTHROUGH]->setVisible(info.passthroughAvailable);

	for (int i = 0; i < 2; ++i)
	{
		_btnPointer[i]->draw();
		_btnView[i]->draw();
		_btnStick[i]->draw();
	}
	for (int i = 0; i < 3; ++i)
	{
		_btnSmooth[i]->draw();
		_btnUpscale[i]->draw();
	}
	for (int i = 0; i < SHOW_COUNT; ++i)
	{
		_btnShow[i]->draw();
	}
}

void VrOptionsState::handle(Action *action)
{
	if (_focus.handle(action))
	{
		return;
	}
	State::handle(action);
}

void VrOptionsState::think()
{
	State::think();
	_focus.think();
	const Uint32 now = SDL_GetTicks();
	const float dt = (now - _lastTicks) / 1000.0f;
	_lastTicks = now;
	// Held - or + (trigger on the button): repeat after a short delay.
	const bool held = _holdButton && (SDL_GetMouseState(0, 0) & SDL_BUTTON_LMASK) != 0;
	if (!held)
	{
		_holdButton = 0;
	}
	const int steps = _hold.update(held, dt);
	for (int i = 0; i < steps; ++i)
	{
		step(_holdButton);
	}

	if (now - _infoTime < 500)
	{
		return;
	}
	_infoTime = now;
	const Xr::VrInfo info = Xr::info();
	char text[80];
	std::snprintf(text, sizeof(text), "%s %d FPS  XR %d HZ", vrText(this, "STR_XR_GAME", "SPIEL").c_str(), info.gameFps, info.xrHz);
	_txtInfo->setText(text);
}

/**
 * Applies the clicked choice at once.
 * @param action Pointer to an action.
 */
void VrOptionsState::btnClick(Action *action)
{
	InteractiveSurface *sender = action->getSender();
	Xr::VrSettings s = Xr::settings();
	for (int i = 0; i < 2; ++i)
	{
		if (sender == _btnPointer[i])
			s.pointerHand = i;
		else if (sender == _btnView[i])
			s.view = i;
		else if (sender == _btnStick[i])
			s.stickAdjust = i == 0;
	}
	for (int i = 0; i < 3; ++i)
	{
		if (sender == _btnSmooth[i])
			s.smoothing = i;
		if (sender == _btnUpscale[i])
		{
			Options::useXBRZFilter = i == 1;
			Options::useHQXFilter = i == 2;
			Options::save();
		}
	}
	if (sender == _btnShow[SHOW_RAY])
		s.showRay = _btnShow[SHOW_RAY]->getPressed();
	else if (sender == _btnShow[SHOW_MARKER])
		s.showMarker = _btnShow[SHOW_MARKER]->getPressed();
	else if (sender == _btnShow[SHOW_CURSOR])
		s.showGameCursor = _btnShow[SHOW_CURSOR]->getPressed();
	s.clamp();
	Xr::setSettings(s);
	refresh();
}

void VrOptionsState::step(TextButton *button)
{
	Xr::VrSettings s = Xr::settings();
	for (int i = 0; i < STEP_COUNT; ++i)
	{
		const int sign = button == _btnInc[i] ? 1 : button == _btnDec[i] ? -1 : 0;
		if (!sign)
			continue;
		switch (i)
		{
		case STEP_WIDTH: s.width[s.view] += 0.1f * sign; break;
		case STEP_DISTANCE: s.distance[s.view] += 0.1f * sign; break;
		case STEP_TILT: s.tableTilt += 5 * sign; break;
		case STEP_HEIGHT: s.tableHeight += 10 * sign; break;
		case STEP_PASSTHROUGH: s.passthrough += 25 * sign; break;
		default: break;
		}
	}
	s.clamp();
	Xr::setSettings(s);
	refresh();
}

/**
 * - or + pressed: one step at once, more while held (think()).
 * @param action Pointer to an action.
 */
void VrOptionsState::btnStepPress(Action *action)
{
	TextButton *button = dynamic_cast<TextButton*>(action->getSender());
	step(button);
	_holdButton = button;
	_hold.update(false, 0.0f);
}

void VrOptionsState::btnStepRelease(Action *)
{
	_holdButton = 0;
}

/**
 * Returns to the VR menu.
 * @param action Pointer to an action.
 */
void VrOptionsState::btnOkClick(Action *)
{
	_game->popState();
}

}
