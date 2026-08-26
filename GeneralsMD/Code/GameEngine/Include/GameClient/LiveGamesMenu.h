/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

///////////////////////////////////////////////////////////////////////////////////////
// FILE: LiveGamesMenu.h
// Description: The Watch Live browser. It reuses the ReplayMenu.wnd layout in a "LIVE GAMES"
// mode, and this module owns that mode. Free functions + file statics, like the other menu
// modules in Source/GameClient/GUI/GUICallbacks/Menus.
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "GameClient/GameWindow.h"	// WindowMsgData

#if defined(GENERALS_ONLINE)

/// Arm live-games mode before pushing ReplayMenu.wnd (WOLWelcomeMenu calls this).
void LiveGamesMenuEnterLiveGamesMode(void);

/// TRUE while the replay menu is running in live-games mode.
Bool LiveGamesMenuIsLiveGamesMode(void);

/// Live-mode half of ReplayMenuInit: retitle, repurpose the buttons, first fetch.
void LiveGamesMenuInit(void);

/// Live-mode half of ReplayMenuShutdown: restore the layout, clear all state.
void LiveGamesMenuShutdown(void);

/// Live-mode half of ReplayMenuUpdate: fetch poll, auto-refresh, CONNECT/OBSERVE label.
void LiveGamesMenuUpdate(void);

/// Returns TRUE when the system message was consumed by live-games mode (the caller then
/// returns MSG_HANDLED); FALSE lets the legacy replay handling run.
Bool LiveGamesMenuHandleSystemMessage(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2);

#endif // defined(GENERALS_ONLINE)
