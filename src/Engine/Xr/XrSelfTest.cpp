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
#include "XrSelfTest.h"
#include "XrRuntime.h"
#include "../Game.h"
#include "../Options.h"
#include "../Screen.h"
#include "../State.h"
#include "../Surface.h"
#include "../../Menu/MainMenuState.h"
#include "../../Menu/VrMenuState.h"
#include "../../Menu/VrOptionsState.h"
#include "../../Interface/TextButton.h"
#include <SDL.h>
#include <android/log.h>
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#define STLOG(...) __android_log_print(ANDROID_LOG_INFO, "OXCE-XR", __VA_ARGS__)

namespace OpenXcom
{
namespace Xr
{

namespace
{

struct Step
{
	const char *name;
	Uint32 delay;                        // ms after the previous step before this one runs
	std::function<bool(Game *)> run;     // true = done, false = try again next frame
};

std::vector<Step> g_steps;
size_t g_index = 0;
Uint32 g_stepStart = 0, g_testStart = 0;
int g_pass = 0, g_fail = 0;
bool g_started = false, g_done = false, g_originalIntro = true;
std::mutex g_textMutex;
std::string g_textSeen;
int g_passthroughClicks = 0, g_passthroughStart = 0;

void check(bool ok, const char *what, const std::string &detail = "")
{
	++(ok ? g_pass : g_fail);
	STLOG("SELFTEST %s %s %s", ok ? "PASS" : "FAIL", what, detail.c_str());
}

void shot(const char *name)
{
	STLOG("SELFTEST SHOT %s", name);
}

Uint32 sinceStep() { return SDL_GetTicks() - g_stepStart; }

template <class T> T *top(Game *game) { return dynamic_cast<T *>(game->getTopState()); }

/// Points at the middle of a widget and clicks, exactly like ray + trigger.
bool click(Game *game, Surface *surface)
{
	if (!surface)
		return false;
	SDL_Surface *screen = game->getScreen()->getSurface();
	const float u = (surface->getX() + surface->getWidth() / 2.0f) / screen->w;
	const float v = (surface->getY() + surface->getHeight() / 2.0f) / screen->h;
	debugPointer(u, v);
	debugClick();
	return true;
}

int SDLCALL watchText(void *, SDL_Event *event)
{
	if (event->type == SDL_TEXTINPUT)
	{
		std::lock_guard<std::mutex> lock(g_textMutex);
		g_textSeen += event->text.text;
	}
	return 1;
}

void buildSteps()
{
	g_steps = {
		{"main_menu", 0, [](Game *game) {
			if (top<MainMenuState>(game))
			{
				check(true, "main_menu", "reached after " + std::to_string((SDL_GetTicks() - g_testStart) / 1000) + " s");
				shot("main_menu");
				return true;
			}
			if (sinceStep() > 180000)
			{
				check(false, "main_menu", "not reached within 180 s");
				g_index = g_steps.size() - 3; // continue with the summary
				return true;
			}
			return false;
		}},
		{"upscale_check", 1000, [](Game *game) {
			const int baseW = game->getScreen()->getSurface()->w, baseH = game->getScreen()->getSurface()->h;
			const VrInfo i = info();
			const std::string detail = "game " + std::to_string(baseW) + "x" + std::to_string(baseH) + " -> canvas input "
				+ std::to_string(i.gameWidth) + "x" + std::to_string(i.gameHeight)
				+ (Options::useXBRZFilter ? " (xBRZ)" : Options::useHQXFilter ? " (HQx)" : " (no upscaler)");
			if (Options::useXBRZFilter || Options::useHQXFilter)
			{
				const int factor = baseW > 0 ? i.gameWidth / baseW : 0;
				check(factor >= 2 && i.gameWidth == baseW * factor && i.gameHeight == baseH * factor
					&& i.gameWidth <= VirtualWidth && i.gameHeight <= VirtualHeight, "upscale_integer_factor", detail);
			}
			else
				check(i.gameWidth == baseW && i.gameHeight == baseH, "no_upscale_passthrough", detail);
			shot("upscaled");
			return true;
		}},
		{"pointer_move", 1500, [](Game *) {
			debugPointer(0.25f, 0.75f);
			return true;
		}},
		{"pointer_check", 400, [](Game *game) {
			int x, y, ex, ey;
			SDL_GetMouseState(&x, &y);
			virtualPoint(0.25f, 0.75f, ex, ey);
			Screen *screen = game->getScreen();
			const int gameX = static_cast<int>((x - screen->getCursorLeftBlackBand()) / screen->getXScale());
			const int gameY = static_cast<int>((y - screen->getCursorTopBlackBand()) / screen->getYScale());
			const int expectX = static_cast<int>(0.25f * screen->getSurface()->w), expectY = static_cast<int>(0.75f * screen->getSurface()->h);
			check(x == ex && y == ey, "pointer_sdl", "mouse " + std::to_string(x) + "," + std::to_string(y) + " expected " + std::to_string(ex) + "," + std::to_string(ey));
			check(std::abs(gameX - expectX) <= 1 && std::abs(gameY - expectY) <= 1, "pointer_game",
				"game " + std::to_string(gameX) + "," + std::to_string(gameY) + " expected " + std::to_string(expectX) + "," + std::to_string(expectY));
			return true;
		}},
		{"menu_open", 300, [](Game *) {
			debugMenuButton();
			return true;
		}},
		{"menu_check", 800, [](Game *game) {
			check(top<VrMenuState>(game) != 0, "vr_menu_opens");
			shot("vr_menu");
			return true;
		}},
		{"options_click", 1500, [](Game *game) {
			VrMenuState *menu = top<VrMenuState>(game);
			return click(game, menu ? menu->getOptionsButton() : 0) || true;
		}},
		{"options_check", 800, [](Game *game) {
			check(top<VrOptionsState>(game) != 0, "vr_options_open_by_click");
			shot("vr_options");
			return true;
		}},
		{"table_click", 1500, [](Game *game) {
			VrOptionsState *options = top<VrOptionsState>(game);
			return click(game, options ? options->getViewButton(VIEW_TABLE) : 0) || true;
		}},
		{"table_check", 800, [](Game *) {
			check(settings().view == VIEW_TABLE, "table_view_on");
			shot("table_view");
			return true;
		}},
		{"canvas_click", 2500, [](Game *game) {
			VrOptionsState *options = top<VrOptionsState>(game);
			return click(game, options ? options->getViewButton(VIEW_CANVAS) : 0) || true;
		}},
		{"canvas_check", 800, [](Game *) {
			check(settings().view == VIEW_CANVAS, "table_view_off");
			return true;
		}},
		{"passthrough_up", 600, [](Game *game) {
			if (!info().passthroughAvailable)
			{
				STLOG("SELFTEST SKIP passthrough (runtime offers no XR_FB_passthrough)");
				return true;
			}
			if (g_passthroughClicks == 0)
				g_passthroughStart = settings().passthrough; // the user may already use passthrough
			if (sinceStep() < static_cast<Uint32>(600 * g_passthroughClicks))
				return false;
			VrOptionsState *options = top<VrOptionsState>(game);
			click(game, options ? options->getPassthroughButton(true) : 0);
			return ++g_passthroughClicks >= 2;
		}},
		{"passthrough_check", 900, [](Game *) {
			if (info().passthroughAvailable)
			{
				check(settings().passthrough == std::min(100, g_passthroughStart + 50), "passthrough_up_50", std::to_string(g_passthroughStart) + " -> " + std::to_string(settings().passthrough) + " %");
				shot("passthrough");
				g_passthroughClicks = 0;
			}
			return true;
		}},
		{"passthrough_down", 1500, [](Game *game) {
			if (!info().passthroughAvailable)
				return true;
			if (sinceStep() < static_cast<Uint32>(600 * g_passthroughClicks))
				return false;
			VrOptionsState *options = top<VrOptionsState>(game);
			click(game, options ? options->getPassthroughButton(false) : 0);
			return ++g_passthroughClicks >= 2;
		}},
		{"passthrough_off_check", 900, [](Game *) {
			if (info().passthroughAvailable)
				check(settings().passthrough == std::max(0, std::min(100, g_passthroughStart + 50) - 50), "passthrough_down_50", std::to_string(settings().passthrough) + " %");
			return true;
		}},
		{"ok_click", 600, [](Game *game) {
			VrOptionsState *options = top<VrOptionsState>(game);
			return click(game, options ? options->getOkButton() : 0) || true;
		}},
		{"ok_check", 800, [](Game *game) {
			check(top<VrMenuState>(game) != 0, "options_ok_back_to_menu");
			return true;
		}},
		{"cancel_click", 600, [](Game *game) {
			VrMenuState *menu = top<VrMenuState>(game);
			return click(game, menu ? menu->getCancelButton() : 0) || true;
		}},
		{"cancel_check", 800, [](Game *game) {
			check(top<MainMenuState>(game) != 0, "vr_menu_closes");
			return true;
		}},
		{"toggle_open", 600, [](Game *) {
			debugMenuButton();
			return true;
		}},
		{"toggle_close", 800, [](Game *game) {
			check(top<VrMenuState>(game) != 0, "menu_button_opens");
			debugMenuButton();
			return true;
		}},
		{"toggle_check", 800, [](Game *game) {
			check(top<MainMenuState>(game) != 0, "menu_button_closes");
			return true;
		}},
		{"keyboard_show", 600, [](Game *) {
			SDL_AddEventWatch(watchText, 0);
			SDL_EventState(SDL_TEXTINPUT, SDL_ENABLE);
			return true;
		}},
		{"keyboard_check", 1000, [](Game *) {
			check(keyboardVisible(), "vr_keyboard_shown");
			shot("vr_keyboard");
			debugText("Xr\xC3\xA4");
			return true;
		}},
		{"keyboard_text", 800, [](Game *) {
			std::string seen;
			{
				std::lock_guard<std::mutex> lock(g_textMutex);
				seen = g_textSeen;
			}
			check(seen == "Xr\xC3\xA4", "vr_keyboard_text", "got '" + seen + "'");
			SDL_EventState(SDL_TEXTINPUT, SDL_DISABLE);
			SDL_DelEventWatch(watchText, 0);
			return true;
		}},
		{"keyboard_hidden", 1000, [](Game *) {
			check(!keyboardVisible(), "vr_keyboard_hidden");
			return true;
		}},
		{"summary", 500, [](Game *) {
			const VrInfo i = info();
			check(i.gameWidth > 0 && i.gameHeight > 0, "canvas_has_frames", std::to_string(i.gameWidth) + "x" + std::to_string(i.gameHeight));
			check(i.gameFps >= 20, "game_fps", std::to_string(i.gameFps) + " fps");
			check(i.xrHz >= 60, "xr_rate", std::to_string(i.xrHz) + " Hz");
			return true;
		}},
		{"result", 0, [](Game *) {
			Options::playIntro = g_originalIntro;
			STLOG("SELFTEST RESULT pass=%d fail=%d", g_pass, g_fail);
			g_done = true;
			return true;
		}},
	};
}

}

void selfTestBegin()
{
	if (!selfTestActive())
		return;
	// The intro video cannot be skipped without input; restored at the end of the test.
	g_originalIntro = Options::playIntro;
	Options::playIntro = false;
	g_testStart = SDL_GetTicks();
	STLOG("SELFTEST start");
}

void selfTestTick(Game *game)
{
	if (g_done || !selfTestActive())
		return;
	if (!g_started)
	{
		buildSteps();
		g_started = true;
		g_index = 0;
		g_stepStart = SDL_GetTicks();
	}
	if (g_index >= g_steps.size())
		return;
	Step &step = g_steps[g_index];
	if (sinceStep() < step.delay && g_index > 0)
		return;
	if (step.run(game))
	{
		++g_index;
		g_stepStart = SDL_GetTicks();
	}
}

}
}
