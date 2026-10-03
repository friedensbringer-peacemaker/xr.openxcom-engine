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

class TextButton;
class ToggleTextButton;
class Window;
class Text;

/**
 * VR options of the Quest port in the style of the game's option pages:
 * pointer hand, smoothing, canvas/table view and geometry, grip+stick
 * adjustment, passthrough, upscaling and the visibility of ray, target dot
 * and the game's mouse cursor. Every change applies at once.
 */
class VrOptionsState : public State
{
private:
	enum Stepper { STEP_WIDTH, STEP_DISTANCE, STEP_TILT, STEP_HEIGHT, STEP_PASSTHROUGH, STEP_COUNT };
	enum Pointer { SHOW_RAY, SHOW_MARKER, SHOW_CURSOR, SHOW_COUNT };
	static const int LabelCount = 11;
	OptionsOrigin _origin;
	Window *_window;
	Text *_txtTitle, *_txtLabels[LabelCount], *_txtValue[STEP_COUNT], *_txtInfo;
	TextButton *_btnPointer[2], *_btnSmooth[3], *_btnView[2], *_btnStick[2], *_btnUpscale[3];
	TextButton *_selPointer, *_selSmooth, *_selView, *_selStick, *_selUpscale;
	ToggleTextButton *_btnShow[SHOW_COUNT];
	TextButton *_btnDec[STEP_COUNT], *_btnInc[STEP_COUNT];
	TextButton *_btnOk;
	TextButton *_holdButton;
	Xr::HoldRepeat _hold;
	Uint32 _infoTime, _lastTicks;
	VrMenuFocus _focus;
	void refresh();
	/// One step of the stepper behind `button` (- or +).
	void step(TextButton *button);
public:
	VrOptionsState(OptionsOrigin origin);
	~VrOptionsState();
	/// Stick/A focus.
	void handle(Action *action) override;
	/// Updates the live numbers (frame rates) twice a second; repeats a held stepper.
	void think() override;
	void btnClick(Action *action);
	/// - and + react on press and repeat while held.
	void btnStepPress(Action *action);
	void btnStepRelease(Action *action);
	void btnOkClick(Action *action);
	/// Buttons the Quest self-test clicks through the controller input path.
	TextButton *getViewButton(int view) const { return _btnView[view ? 1 : 0]; }
	TextButton *getPassthroughButton(bool increase) const { return increase ? _btnInc[STEP_PASSTHROUGH] : _btnDec[STEP_PASSTHROUGH]; }
	TextButton *getOkButton() const { return _btnOk; }
};

}
