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

namespace OpenXcom
{
class Game;
namespace Xr
{

/**
 * Quest self-test, active when xr-selftest.txt exists in the config folder.
 * Walks main menu -> VR menu -> VR options -> table view -> passthrough ->
 * back, checks pointer mapping, the VR keyboard and frame rates, all through
 * the controller input path. Results go to logcat (tag OXCE-XR, "SELFTEST"),
 * "SELFTEST SHOT <name>" marks moments for screenshots.
 */
void selfTestBegin();
void selfTestTick(Game *game);

}
}
