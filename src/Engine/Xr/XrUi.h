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
#include <cstdint>
#include <string>
#include <vector>

namespace OpenXcom
{
namespace Xr
{

/// Glyph codes below 0x20 for symbols of the VR UIs.
const char GlyphAe = '\x01', GlyphOe = '\x02', GlyphUe = '\x03', GlyphSz = '\x04', GlyphShift = '\x05', GlyphBack = '\x06', GlyphDegree = '\x07';

/// Colours shared by the VR keyboard and the VR menu.
namespace Colors
{
const uint32_t Background = 0x10141C, Button = 0x2E3644, Hover = 0x4A5872, Pressed = 0x3D8FE0, Selected = 0x2F6FB0,
	Disabled = 0x1C212B, Text = 0xE8ECF2, TextDim = 0x7E8796, Title = 0x9FD3FF;
}

/**
 * RGBA pixel buffer with a built-in 5x7 bitmap font (upper case, digits,
 * German umlauts). CPU-drawn, uploaded by the XR thread to a quad layer.
 */
class Canvas
{
public:
	Canvas(int width, int height);
	int width() const { return _width; }
	int height() const { return _height; }
	const uint8_t *data() const { return _pixels.data(); }
	void fill(int x, int y, int w, int h, uint32_t rgb);
	/// UTF-8 text, upper-cased; shrinks the scale until it fits maxWidth.
	void text(const std::string &utf8, float x, float centerY, int maxScale, float maxWidth, uint32_t rgb, bool centered);

private:
	int _width, _height;
	std::vector<uint8_t> _pixels;
};

/// A clickable rectangle of a VR UI page (pixel coordinates).
struct UiButton
{
	int x, y, w, h;
	std::string label;
	int command;
	bool selected, enabled;
};

/// Index of the button under panel coordinates u/v (0..1), or -1; small slack between buttons.
int buttonAt(const std::vector<UiButton> &buttons, int width, int height, float u, float v);

}
}
