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
#include "../Renderer.h"
#include <cstdint>
#include <vector>

namespace OpenXcom
{

/**
 * Renderer for the immersive Quest mode: hands every finished frame to the
 * OpenXR thread (Xr::submitFrame) instead of drawing into the SDL window.
 * With the game's xBRZ/HQx filter option the frame is first upscaled by the
 * largest integer factor that fits the canvas (320x200 -> 1280x800), which
 * turns the blocky pixel font into smooth edges.
 */
class XrRenderer : public Renderer
{
private:
	Uint32 _format;
	SDL_Surface *_lastSurface;
	std::vector<uint32_t> _source, _previous, _scaled;
	int _previousW, _previousH, _previousFilter, _previousFactor;
	bool _hqxReady;
	void upscale(int filter, int factor, int width, int height);
public:
	XrRenderer();
	~XrRenderer();
	void setPixelFormat(Uint32 format) override;
	void setInternalRect(SDL_Rect *srcRect) override;
	void setOutputRect(SDL_Rect *dstRect) override;
	void flip(SDL_Surface *srcSurface) override;
	void screenshot(const std::string &filename) const override;
	RendererType getRendererType() override { return RENDERER_XR; }
};

}
