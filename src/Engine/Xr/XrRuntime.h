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
#include "XrSettings.h"
#include <string>

namespace OpenXcom
{
namespace Xr
{

/// Size of the virtual display the game believes it runs on in immersive mode.
const int VirtualWidth = 1280;
const int VirtualHeight = 800;

/// Requests from the XR thread to the game (see takeMenuRequest).
enum MenuRequest { MENU_REQUEST_NONE = 0, MENU_REQUEST_TOGGLE = 1, MENU_REQUEST_RESUME = 2 };

/**
 * Starts the OpenXR thread (Meta Quest). Must be called on the game thread
 * after SDL_Init and before the Screen is created. Returns false (and leaves
 * the classic SDL renderer in charge) when no OpenXR runtime is available.
 */
bool start(const std::string &configFolder);
/// True while the immersive mode owns display and input.
bool active();
/// Stops the XR thread and releases all OpenXR resources.
void stop();
/// Where the game image sits in virtual display coordinates (for mouse mapping).
void setOutputRect(int x, int y, int w, int h);
/// Hands over a finished game frame in the given SDL pixel format. Called by XrRenderer::flip().
void submitFrame(const void *pixels, int width, int height, int pitch, unsigned int sdlPixelFormat);
/// The game drew a frame identical to the last one (counts for the frame rate, nothing to upload).
void frameUnchanged();
/// True exactly once per installation: time to apply the VR defaults to the game options.
bool takeFirstRunDefaults();

/// False while the game should pause: headset off or Quest system menu open.
bool gameShouldRun();
/// The game reports where it is (GameContext: main menu, Geoscape, battle).
void setContext(int context);
/// Pending VR menu request from the controllers (MenuRequest), cleared on read.
int takeMenuRequest();
/// The game reports whether a VR menu page is on top (ray and target dot then stay visible).
void setMenuOpen(bool open);
/// Two short pulses on the pointer hand (quit button armed).
void hapticDoublePulse();
/// Writes xr.cfg on the next XR frame (stop() writes it again after the XR thread ended).
void saveSettingsNow();

/// Thread-safe access to the VR options; setSettings applies them at once and saves shortly after.
VrSettings settings();
void setSettings(const VrSettings &settings);
VrInfo info();
/// Places the canvas in front of the current head pose.
void recenter();
/// Presses and releases a key as if typed (game thread).
void tapKey(int sdlScancode);

/// Quest self-test (file xr-selftest.txt in the config folder): drives the same input path
/// as the controllers, without anybody wearing the headset. The game runs while the session
/// is visible, not only when focused.
bool selfTestActive();
/// Moves the pointer like the controller ray hitting the canvas at u/v (0..1).
void debugPointer(float u, float v);
/// Left click at the pointer: press now, release one XR frame later.
void debugClick();
/// Same as pressing the left menu button.
void debugMenuButton();
/// Types UTF-8 text like the VR keyboard.
void debugText(const std::string &utf8);
/// True while the VR keyboard layer is shown.
bool keyboardVisible();
/// Canvas coordinates u/v -> virtual display pixel (the mapping the pointer uses).
void virtualPoint(float u, float v, int &x, int &y);

}
}
