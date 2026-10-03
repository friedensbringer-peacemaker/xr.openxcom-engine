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
#include "XrRenderer.h"
#include "XrRuntime.h"
#include "../Logger.h"
#include "../Options.h"
#include "../Scalers/hqx.h"
#include "../Scalers/xbrz.h"
#include "../../lodepng.h"
#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace OpenXcom
{

namespace
{
enum Filter { FILTER_NONE, FILTER_XBRZ, FILTER_HQX };
const int UpscaleThreads = 4; // xBRZ slices; Quest 3 has enough cores next to game and XR thread
}

XrRenderer::XrRenderer() : _format(SDL_PIXELFORMAT_ARGB8888), _lastSurface(0), _previousW(0), _previousH(0),
	_previousFilter(-1), _previousFactor(0), _hqxReady(false)
{
	Log(LOG_INFO) << "[XrRenderer] Immersive renderer created";
}

XrRenderer::~XrRenderer()
{
}

void XrRenderer::setPixelFormat(Uint32 format)
{
	_format = format;
	Log(LOG_INFO) << "[XrRenderer] Surface pixel format " << SDL_GetPixelFormatName(format);
}

void XrRenderer::setInternalRect(SDL_Rect *srcRect)
{
	Log(LOG_INFO) << "[XrRenderer] Game surface " << srcRect->w << "x" << srcRect->h;
}

void XrRenderer::setOutputRect(SDL_Rect *dstRect)
{
	Xr::setOutputRect(dstRect->x, dstRect->y, dstRect->w, dstRect->h);
	Log(LOG_INFO) << "[XrRenderer] Output rect " << dstRect->x << "," << dstRect->y << " " << dstRect->w << "x" << dstRect->h;
}

void XrRenderer::flip(SDL_Surface *srcSurface)
{
	_lastSurface = srcSurface;
	const int width = srcSurface->w, height = srcSurface->h;
	const int filter = Options::useXBRZFilter ? FILTER_XBRZ : Options::useHQXFilter ? FILTER_HQX : FILTER_NONE;
	// Largest integer factor that fits the canvas: 320x200 -> 4 (1280x800), 640x400 -> 2.
	const int factor = std::min(std::min(Xr::VirtualWidth / width, Xr::VirtualHeight / height), filter == FILTER_XBRZ ? 6 : 4);
	if (filter == FILTER_NONE || factor < 2 || srcSurface->format->BytesPerPixel != 4)
	{
		_previousFilter = -1;
		Xr::submitFrame(srcSurface->pixels, width, height, srcSurface->pitch, srcSurface->format->format);
		return;
	}

	// The game redraws every frame even when nothing changed; upscale only new pictures.
	_source.resize(static_cast<size_t>(width) * height);
	for (int y = 0; y < height; ++y)
		std::memcpy(&_source[static_cast<size_t>(y) * width], static_cast<const uint8_t *>(srcSurface->pixels) + static_cast<size_t>(y) * srcSurface->pitch,
			static_cast<size_t>(width) * 4);
	if (width == _previousW && height == _previousH && filter == _previousFilter && factor == _previousFactor && _source == _previous)
	{
		Xr::frameUnchanged();
		return;
	}
	_previous.swap(_source);
	_previousW = width;
	_previousH = height;
	_previousFilter = filter;
	_previousFactor = factor;

	upscale(filter, factor, width, height);
	Xr::submitFrame(_scaled.data(), width * factor, height * factor, width * factor * 4, srcSurface->format->format);
}

/**
 * Upscales _previous (the current frame, width x height, 32 bit) into _scaled.
 * xBRZ runs in horizontal slices on several threads (allowed by xBRZ as long as
 * the slices do not overlap); HQx has no slice interface and runs in one go.
 */
void XrRenderer::upscale(int filter, int factor, int width, int height)
{
	_scaled.resize(static_cast<size_t>(width) * height * factor * factor);
	if (filter == FILTER_XBRZ)
	{
		const int rows = (height + UpscaleThreads - 1) / UpscaleThreads;
		std::vector<std::thread> workers;
		for (int first = 0; first < height; first += rows)
		{
			const int last = std::min(height, first + rows);
			workers.emplace_back([this, factor, width, height, first, last] {
				xbrz::scale(factor, _previous.data(), _scaled.data(), width, height, xbrz::RGB, xbrz::ScalerCfg(), first, last);
			});
		}
		for (std::thread &worker : workers)
			worker.join();
		return;
	}
	if (!_hqxReady)
	{
		hqxInit();
		_hqxReady = true;
	}
	if (factor == 2)
		hq2x_32(_previous.data(), _scaled.data(), width, height);
	else if (factor == 3)
		hq3x_32(_previous.data(), _scaled.data(), width, height);
	else
		hq4x_32(_previous.data(), _scaled.data(), width, height);
}

void XrRenderer::screenshot(const std::string &filename) const
{
	if (!_lastSurface)
		return;
	const int width = _lastSurface->w, height = _lastSurface->h;
	std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
	if (SDL_ConvertPixels(width, height, _lastSurface->format->format, _lastSurface->pixels, _lastSurface->pitch,
	                      SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4) != 0)
		return;
	for (size_t i = 3; i < pixels.size(); i += 4)
		pixels[i] = 0xFF;
	unsigned error = lodepng::encode(filename, pixels.data(), width, height, LCT_RGBA);
	if (error)
	{
		Log(LOG_ERROR) << "Saving to PNG failed: " << lodepng_error_text(error);
	}
}

}
