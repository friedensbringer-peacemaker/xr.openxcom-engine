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
#include "VrMenuState.h"
#include "VrOptionsState.h"
#include "PauseState.h"
#include "SaveGameState.h"
#include "../Engine/Action.h"
#include "../Engine/Game.h"
#include "../Engine/Options.h"
#include "../Engine/Xr/XrRuntime.h"
#include "../Interface/TextButton.h"
#include "../Interface/Window.h"
#include "../Interface/Text.h"
#include "../Savegame/SavedGame.h"
#include "../Savegame/SavedBattleGame.h"
#include "../Battlescape/BattlescapeGame.h"

namespace OpenXcom
{

namespace
{
// Layout grid of the quick menu (game pixels): one column of full-width rows, paired rows split in two.
const int MenuW = 216, MenuH = 184, MenuTop = 8, RowFirst = 40, RowStep = 18, RowH = 16;
const int ColX = 18, FullW = 180, HalfW = 88, HalfX2 = 110, CloseY = 168;
}

/**
 * Initializes all the elements of the VR menu.
 * @param origin Game section that originated this state.
 * @param resumePrompt True when shown after a break ("Pause").
 */
VrMenuState::VrMenuState(OptionsOrigin origin, bool resumePrompt) : _origin(origin), _resumePrompt(resumePrompt), _quitPending(false),
	_btnResume(0), _selTempo(0), _selQuit(0), _lastTicks(0)
{
	_screen = false;
	const int x = _origin == OPT_GEOSCAPE ? 20 : 52;
	const bool inGame = _origin != OPT_MENU;

	// Create objects
	_window = new Window(this, MenuW, MenuH, x, MenuTop, POPUP_BOTH);
	_txtTitle = new Text(MenuW - 10, 17, x + 5, 18);
	int y = RowFirst;
	if (_resumePrompt)
	{
		_btnResume = new TextButton(FullW, RowH, x + ColX, y);
		y += RowStep;
	}
	// Rows that do not apply here (no game loaded, no Geoscape clock) take no space.
	_btnQuickSave = new TextButton(HalfW, RowH, x + ColX, y);
	_btnQuickLoad = new TextButton(HalfW, RowH, x + HalfX2, y);
	y += inGame ? RowStep : 0;
	// Same row: Geoscape clock or battle tempo.
	for (int i = 0; i < 6; ++i)
	{
		_btnSpeed[i] = new TextButton(28, RowH, x + ColX + i * 30, y);
	}
	for (int i = 0; i < 3; ++i)
	{
		_btnTempo[i] = new TextButton(58, RowH, x + ColX + i * 61, y);
	}
	y += inGame ? RowStep : 0;
	_btnGameMenu = new TextButton(FullW, RowH, x + ColX, y);
	y += inGame ? RowStep : 0;
	_btnRecenter = new TextButton(HalfW, RowH, x + ColX, y);
	_btnTable = new TextButton(HalfW, RowH, x + HalfX2, y);
	y += RowStep;
	_btnOptions = new TextButton(FullW, RowH, x + ColX, y);
	y += RowStep;
	_btnQuit = new TextButton(FullW, RowH, x + ColX, y);
	_btnCancel = new TextButton(FullW, RowH, x + ColX, CloseY);

	// Set palette
	setInterface("pauseMenu", false, _game->getSavedGame() ? _game->getSavedGame()->getSavedBattle() : 0);

	add(_window, "window", "pauseMenu");
	add(_txtTitle, "text", "pauseMenu");
	if (_btnResume)
	{
		add(_btnResume, "button", "pauseMenu");
	}
	add(_btnQuickSave, "button", "pauseMenu");
	add(_btnQuickLoad, "button", "pauseMenu");
	for (int i = 0; i < 6; ++i)
	{
		add(_btnSpeed[i], "button", "pauseMenu");
	}
	for (int i = 0; i < 3; ++i)
	{
		add(_btnTempo[i], "button", "pauseMenu");
	}
	add(_btnGameMenu, "button", "pauseMenu");
	add(_btnRecenter, "button", "pauseMenu");
	add(_btnTable, "button", "pauseMenu");
	add(_btnOptions, "button", "pauseMenu");
	add(_btnQuit, "button", "pauseMenu");
	add(_btnCancel, "button", "pauseMenu");

	centerAllSurfaces();

	// Set up objects
	setWindowBackground(_window, "pauseMenu");

	_txtTitle->setAlign(ALIGN_CENTER);
	_txtTitle->setBig();
	_txtTitle->setText(_resumePrompt ? vrText(this, "STR_XR_PAUSE", "PAUSE") : vrText(this, "STR_XR_VR_MENU", "VR-MEN\xC3\x9C"));

	if (_btnResume)
	{
		// A (Return) works through the stick focus, which starts on this button.
		_btnResume->setText(vrText(this, "STR_XR_RESUME", "WEITERSPIELEN"));
		_btnResume->onMouseClick((ActionHandler)&VrMenuState::btnCancelClick);
	}

	_btnQuickSave->setText(vrText(this, "STR_XR_SAVE", "SPEICHERN"));
	_btnQuickSave->onMouseClick((ActionHandler)&VrMenuState::btnClick);
	_btnQuickSave->setVisible(inGame);
	_btnQuickLoad->setText(vrText(this, "STR_XR_LOAD", "LADEN"));
	_btnQuickLoad->onMouseClick((ActionHandler)&VrMenuState::btnClick);
	_btnQuickLoad->setVisible(inGame);

	static const char *const speeds[5] = {"5S", "1M", "5M", "30M", "1H"};
	for (int i = 0; i < 6; ++i)
	{
		_btnSpeed[i]->setText(i < 5 ? std::string(speeds[i]) : vrText(this, "STR_XR_SPEED_DAY", "1T"));
		_btnSpeed[i]->onMouseClick((ActionHandler)&VrMenuState::btnClick);
		_btnSpeed[i]->setVisible(_origin == OPT_GEOSCAPE);
	}
	const std::string tempos[3] = {vrText(this, "STR_XR_TEMPO_NORMAL", "NORMAL"), vrText(this, "STR_XR_TEMPO_FAST", "SCHNELL"),
		vrText(this, "STR_XR_TEMPO_TURBO", "TURBO")};
	for (int i = 0; i < 3; ++i)
	{
		_btnTempo[i]->setText(tempos[i]);
		_btnTempo[i]->setGroup(&_selTempo);
		_btnTempo[i]->onMouseClick((ActionHandler)&VrMenuState::btnClick);
		_btnTempo[i]->setVisible(_origin == OPT_BATTLESCAPE);
	}
	updateTempoButtons();

	_btnGameMenu->setText(vrText(this, "STR_XR_GAME_MENU", "SPIELMEN\xC3\x9C"));
	_btnGameMenu->onMouseClick((ActionHandler)&VrMenuState::btnClick);
	_btnGameMenu->setVisible(inGame);

	_btnRecenter->setText(vrText(this, "STR_XR_RECENTER", "ZENTRIEREN"));
	_btnRecenter->onMouseClick((ActionHandler)&VrMenuState::btnClick);

	_btnTable->onMouseClick((ActionHandler)&VrMenuState::btnClick);
	updateTableButton();

	_btnOptions->setText(vrText(this, "STR_XR_VR_OPTIONS", "VR-OPTIONEN"));
	_btnOptions->onMouseClick((ActionHandler)&VrMenuState::btnClick);

	// Quit: dangerous action, two clicks within 3 s. Armed = drawn filled (group trick of TextButton).
	_btnQuit->setGroup(&_selQuit);
	_btnQuit->onMouseClick((ActionHandler)&VrMenuState::btnQuitClick);
	updateQuitButton();

	_btnCancel->setText(vrText(this, "STR_XR_CLOSE", "SCHLIESSEN"));
	_btnCancel->onMouseClick((ActionHandler)&VrMenuState::btnCancelClick);
	_btnCancel->onKeyboardPress((ActionHandler)&VrMenuState::btnCancelClick, Options::keyCancel);

	if (_origin == OPT_BATTLESCAPE)
	{
		applyBattlescapeTheme("pauseMenu");
	}

	// Stick/A/B: focus frame over the buttons, in reading order.
	_focus.create(this, _game);
	if (_btnResume)
	{
		_focus.add(_btnResume);
	}
	_focus.add(_btnQuickSave);
	_focus.add(_btnQuickLoad);
	for (int i = 0; i < 6; ++i)
	{
		_focus.add(_btnSpeed[i]);
	}
	for (int i = 0; i < 3; ++i)
	{
		_focus.add(_btnTempo[i]);
	}
	_focus.add(_btnGameMenu);
	_focus.add(_btnRecenter);
	_focus.add(_btnTable);
	_focus.add(_btnOptions);
	_focus.add(_btnQuit);
	_focus.add(_btnCancel);
	if (_btnResume)
	{
		_focus.showFirst();
	}
	_lastTicks = SDL_GetTicks();
}

VrMenuState::~VrMenuState()
{
}

OptionsOrigin VrMenuState::currentOrigin(Game *game)
{
	SavedGame *save = game->getSavedGame();
	if (!save)
	{
		return OPT_MENU;
	}
	return save->getSavedBattle() ? OPT_BATTLESCAPE : OPT_GEOSCAPE;
}

bool VrMenuState::canOpen(Game *game)
{
	SavedGame *save = game->getSavedGame();
	if (!save || !save->getSavedBattle())
	{
		return true;
	}
	SavedBattleGame *battle = save->getSavedBattle();
	BattlescapeGame *battleGame = battle->getBattleGame();
	return battleGame && battleGame->getStates().empty() && (battle->getSide() == FACTION_PLAYER || battle->getDebugMode());
}

namespace
{
/// Battle tempo presets: delays of own units, aliens and shots (OXCE options, smaller = faster).
const int TempoPresets[3][3] = {{30, 30, 6}, {15, 15, 4}, {4, 4, 2}};
}

void VrMenuState::init()
{
	State::init();
	if (_quitPending)
	{
		// Back from the autosave screen: everything is written, leave now.
		_game->quit();
		return;
	}
	// Never carry an armed quit button into a new context (back from the options page).
	_quitConfirm.cancel();
	updateQuitButton();
	updateTableButton();
	updateTempoButtons();
	_lastTicks = SDL_GetTicks();
}

void VrMenuState::handle(Action *action)
{
	if (_focus.handle(action))
	{
		return;
	}
	State::handle(action);
}

void VrMenuState::think()
{
	State::think();
	_focus.think();
	const Uint32 now = SDL_GetTicks();
	const float dt = (now - _lastTicks) / 1000.0f;
	_lastTicks = now;
	if (_quitConfirm.update(dt))
	{
		updateQuitButton(); // 3 s without a second click: back to idle
	}
}

/**
 * Marks the preset that matches the game's current speed options (read from
 * the engine, so changes in the game's own options menu show up here too).
 */
void VrMenuState::updateTempoButtons()
{
	_selTempo = 0;
	for (int i = 0; i < 3; ++i)
	{
		if (Options::battleXcomSpeed == TempoPresets[i][0] && Options::battleAlienSpeed == TempoPresets[i][1])
		{
			_selTempo = _btnTempo[i];
		}
	}
	for (int i = 0; i < 3; ++i)
	{
		_btnTempo[i]->draw();
	}
}

void VrMenuState::updateTableButton()
{
	_btnTable->setText(Xr::settings().view == Xr::VIEW_TABLE ? vrText(this, "STR_XR_TABLE_ON", "TISCH: AN") : vrText(this, "STR_XR_TABLE_OFF", "TISCH: AUS"));
}

void VrMenuState::updateQuitButton()
{
	const bool armed = _quitConfirm.armed();
	_selQuit = armed ? _btnQuit : 0;
	_btnQuit->setText(armed ? vrText(this, "STR_XR_QUIT_CONFIRM", "SICHER? NOCHMAL KLICKEN") : vrText(this, "STR_XR_QUIT", "BEENDEN"));
	_btnQuit->draw();
}

/**
 * Handles every button except quit and cancel; actions that belong to the game are
 * sent as its own hotkeys after the menu closed, so the game's rules apply.
 * @param action Pointer to an action.
 */
void VrMenuState::btnClick(Action *action)
{
	if (_quitConfirm.armed())
	{
		_quitConfirm.cancel(); // any other button disarms quit
		updateQuitButton();
	}
	InteractiveSurface *sender = action->getSender();
	if (sender == _btnQuickSave || sender == _btnQuickLoad)
	{
		const SDL_Keycode key = sender == _btnQuickSave ? Options::keyQuickSave : Options::keyQuickLoad;
		_game->popState();
		Xr::tapKey(SDL_GetScancodeFromKey(key));
		return;
	}
	const SDL_Keycode speedKeys[6] = {Options::keyGeoSpeed1, Options::keyGeoSpeed2, Options::keyGeoSpeed3,
		Options::keyGeoSpeed4, Options::keyGeoSpeed5, Options::keyGeoSpeed6};
	for (int i = 0; i < 6; ++i)
	{
		if (sender == _btnSpeed[i])
		{
			_game->popState();
			Xr::tapKey(SDL_GetScancodeFromKey(speedKeys[i]));
			return;
		}
	}
	for (int i = 0; i < 3; ++i)
	{
		if (sender == _btnTempo[i])
		{
			Options::battleXcomSpeed = TempoPresets[i][0];
			Options::battleAlienSpeed = TempoPresets[i][1];
			Options::battleFireSpeed = TempoPresets[i][2];
			Options::save();
			updateTempoButtons();
			return;
		}
	}
	if (sender == _btnGameMenu)
	{
		const OptionsOrigin origin = _origin;
		_game->popState();
		_game->pushState(new PauseState(origin));
	}
	else if (sender == _btnRecenter)
	{
		Xr::recenter();
	}
	else if (sender == _btnTable)
	{
		Xr::VrSettings settings = Xr::settings();
		settings.view = settings.view == Xr::VIEW_TABLE ? Xr::VIEW_CANVAS : Xr::VIEW_TABLE;
		Xr::setSettings(settings);
		updateTableButton();
	}
	else if (sender == _btnOptions)
	{
		_game->pushState(new VrOptionsState(_origin));
	}
}

/**
 * First click arms the quit button (visible + double pulse), the second within 3 s quits.
 * @param action Pointer to an action.
 */
void VrMenuState::btnQuitClick(Action *)
{
	if (_quitConfirm.click() == Xr::ConfirmAction::CONFIRMED)
	{
		quit();
		return;
	}
	updateQuitButton();
	Xr::hapticDoublePulse();
}

/**
 * Saves the settings (options.cfg, xr.cfg) and - in a running game - the game itself
 * by OXCE's own autosave rules: ironman saves its slot, otherwise only with the
 * autosave option on; never during the alien turn or a battle preview. The game
 * quits once the save screen is gone (init()).
 */
void VrMenuState::quit()
{
	_quitConfirm.cancel();
	Options::save();
	Xr::saveSettingsNow();
	SavedGame *save = _game->getSavedGame();
	if (save && _origin != OPT_MENU && save->getEnding() == END_NONE)
	{
		SavedBattleGame *battle = save->getSavedBattle();
		const bool safeMoment = !battle || (!battle->isPreview() && battle->getSide() == FACTION_PLAYER && canOpen(_game));
		if (safeMoment && (save->isIronman() || Options::autosave))
		{
			const SaveType type = save->isIronman() ? SAVE_IRONMAN : battle ? SAVE_AUTO_BATTLESCAPE : SAVE_AUTO_GEOSCAPE;
			_quitPending = true;
			_game->pushState(new SaveGameState(_origin, type, _palette, battle ? battle->getTurn() : save->getDaysPassed()));
			return;
		}
	}
	_game->quit();
}

/**
 * Closes the menu and continues the game.
 * @param action Pointer to an action.
 */
void VrMenuState::btnCancelClick(Action *)
{
	_game->popState();
}

}
