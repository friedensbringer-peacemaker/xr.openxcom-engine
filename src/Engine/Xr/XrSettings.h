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
#include <algorithm>

namespace OpenXcom
{
namespace Xr
{

enum ViewMode { VIEW_CANVAS = 0, VIEW_TABLE = 1 };
enum GameContext { CONTEXT_MENU = 0, CONTEXT_GEOSCAPE = 1, CONTEXT_BATTLESCAPE = 2 };

/// All persistent VR options (xr.cfg). Width and distance are kept per view.
struct VrSettings
{
	int pointerHand = 1;   // 0 left, 1 right
	int smoothing = 1;     // 0 off, 1 light (25 ms), 2 strong (55 ms)
	int view = VIEW_CANVAS;
	float width[2] = {2.0f, 1.4f};
	float distance[2] = {1.8f, 0.8f};
	int tableTilt = 55;    // degrees from upright, 30..75
	int tableHeight = 40;  // cm below eye height, 0..90
	bool stickAdjust = true;
	int passthrough = 0;   // percent, 0 = black room
	bool showRay = true;        // laser ray (always shown while the VR menu or keyboard is open)
	bool showMarker = true;     // target dot at the ray hit (same rule)
	bool showGameCursor = true; // the game's own mouse cursor

	/// Keeps every value inside the limits of the XR port guidelines.
	void clamp()
	{
		pointerHand = pointerHand ? 1 : 0;
		smoothing = std::max(0, std::min(2, smoothing));
		view = view == VIEW_TABLE ? VIEW_TABLE : VIEW_CANVAS;
		distance[VIEW_CANVAS] = std::max(0.7f, std::min(3.5f, distance[VIEW_CANVAS]));
		const float d = distance[VIEW_CANVAS];
		width[VIEW_CANVAS] = std::max(std::max(1.0f, 0.55f * d), std::min(std::min(3.2f, 1.8f * d), width[VIEW_CANVAS]));
		distance[VIEW_TABLE] = std::max(0.4f, std::min(1.5f, distance[VIEW_TABLE]));
		width[VIEW_TABLE] = std::max(0.8f, std::min(2.2f, width[VIEW_TABLE]));
		tableTilt = std::max(30, std::min(75, tableTilt));
		tableHeight = std::max(0, std::min(90, tableHeight));
		passthrough = std::max(0, std::min(100, passthrough));
	}
};

/// Live numbers shown in the VR options.
struct VrInfo
{
	bool passthroughAvailable = false;
	int gameWidth = 0, gameHeight = 0, gameFps = 0, xrHz = 0;
};

}
}
