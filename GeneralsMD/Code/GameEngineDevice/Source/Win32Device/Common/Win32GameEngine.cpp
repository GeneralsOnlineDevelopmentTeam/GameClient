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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DGameEngine.cpp ////////////////////////////////////////////////////////////////////////
// Author: Colin Day, April 2001
// Description:
//   Implementation of the Win32 game engine, this is the highest level of
//   the game application, it creates all the devices we will use for the game
///////////////////////////////////////////////////////////////////////////////////////////////////

#include <windows.h>

#include "Win32Device/Common/Win32GameEngine.h"
#include "Common/PerfTimer.h"
#include "Common/FramePacer.h"
#include "Common/MessageStream.h"
#include "GameClient/InGameUI.h"
#include "GameClient/Keyboard.h"
#include "GameClient/Mouse.h"
#include "GameClient/SelectionXlat.h"
#include "GameClient/View.h"

#include "GameNetwork/LANAPICallbacks.h"
#include "../OnlineServices_Init.h"

extern DWORD TheMessageTime;

//-------------------------------------------------------------------------------------------------
/** Constructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::Win32GameEngine()
{
	m_isAtFrameBoundary = FALSE;
	m_isInModalStep = FALSE;

	// Stop blue screen
	m_previousErrorMode = SetErrorMode( SEM_FAILCRITICALERRORS );
}

//-------------------------------------------------------------------------------------------------
/** Destructor for Win32GameEngine */
//-------------------------------------------------------------------------------------------------
Win32GameEngine::~Win32GameEngine()
{
	// restore it (this isn't really necessary, but feels good.)
	SetErrorMode( m_previousErrorMode );
}


//-------------------------------------------------------------------------------------------------
/** Initialize the game engine */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::init()
{

	// extending functionality
	GameEngine::init();

}

//-------------------------------------------------------------------------------------------------
/** Reset the system */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::reset()
{

	// extending functionality
	GameEngine::reset();

}

//-------------------------------------------------------------------------------------------------
/** Network games and network lobbies never stop for the window state, only the drawing may pause. */
//-------------------------------------------------------------------------------------------------
Bool Win32GameEngine::shouldKeepRunningInBackground()
{
	if (TheNetwork != nullptr || TheLAN != nullptr)
	{
		return TRUE;
	}

	if (TheGameLogic != nullptr)
	{
		if (TheGameLogic->isInMultiplayerGame())
		{
			return TRUE;
		}

		// a single player, skirmish or replay game pauses, also when logged in online
		if (TheGameLogic->isInGame() && !TheGameLogic->isInShellGame())
		{
			return FALSE;
		}
	}

	if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
	{
		return TRUE;
	}

	return FALSE;
}

//-------------------------------------------------------------------------------------------------
/** Runs one main loop iteration from a Windows modal loop (window move or size, menu).
	* Only from the message service that ends a frame, other callers are mid-operation. */
//-------------------------------------------------------------------------------------------------
Bool Win32GameEngine::stepFrameFromModalLoop()
{
	if (m_isInModalStep || !m_isAtFrameBoundary || getQuitting() || !shouldKeepRunningInBackground())
	{
		return FALSE;
	}

	m_isInModalStep = TRUE;
	update();
	TheFramePacer->update();
	m_isInModalStep = FALSE;
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
/** The modal loop eats the button releases and key releases, so nothing may stay held or be delivered late.
	* Scrolling and area selection end without a command being issued. */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::releaseInputForModalLoop()
{
	if (TheInGameUI != nullptr)
	{
		TheInGameUI->setScrolling(FALSE);
		TheInGameUI->setSelecting(FALSE);
		TheInGameUI->endAreaSelectHint(nullptr);
	}

	if (TheSelectionTranslator != nullptr)
	{
		TheSelectionTranslator->setLeftMouseButton(FALSE);
		TheSelectionTranslator->setDragSelecting(FALSE);
	}

	if (TheTacticalView != nullptr)
	{
		TheTacticalView->setMouseLock(FALSE);
	}

	if (TheMouse != nullptr)
	{
		TheMouse->resetButtons();
	}

	if (TheKeyboard != nullptr && TheMessageStream != nullptr)
	{
		// reads and drops the keys buffered by the device meanwhile
		TheKeyboard->UPDATE();
		TheKeyboard->resetKeys();
	}
}

//-------------------------------------------------------------------------------------------------
/** Update the game engine by updating the GameClient and
	* GameLogic singletons. */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::update()
{


	// call the engine normal update
	GameEngine::update();

	if (m_isInModalStep)
	{
		return;
	}

	extern HWND ApplicationHWnd;
	if (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
		while (ApplicationHWnd && ::IsIconic(ApplicationHWnd)) {
			// We are alt-tabbed out here.  Sleep a bit, & process windows
			// so that we can become un-alt-tabbed out.
			Sleep(5);
			serviceWindowsOS();

			if (TheLAN != nullptr) {
				// BGC - need to update TheLAN so we can process and respond to other
				// people's messages who may not be alt-tabbed out like we are.
				TheLAN->setIsActive(isActive());
				TheLAN->update();
			}

			if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
			{
				NGMP_OnlineServicesManager::GetInstance()->Tick();
			}

			// Network games and lobbies keep running, the client skips the redraw while iconic.
			if (TheGameEngine->getQuitting() || shouldKeepRunningInBackground()) {
				break; // keep running.
			}
		}

    // When we are alt-tabbed out... the MilesAudioManager seems to go into a coma sometimes
    // and not regain focus properly when we come back. This seems to wake it up nicely.
    AudioAffect aa = (AudioAffect)0x10;
		TheAudio->setVolume(TheAudio->getVolume( aa ), aa );

	}

	// allow windows to perform regular windows maintenance stuff like msgs
	m_isAtFrameBoundary = TRUE;
	serviceWindowsOS();
	m_isAtFrameBoundary = FALSE;

}

//-------------------------------------------------------------------------------------------------
/** This function may be called from within this application to let
  * Microsoft Windows do its message processing and dispatching.  Presumably
	* we would call this at least once each time around the game loop to keep
	* Windows services from backing up */
//-------------------------------------------------------------------------------------------------
void Win32GameEngine::serviceWindowsOS()
{
	MSG msg;
  Int returnValue;

	//
	// see if we have any messages to process, a nullptr window handle tells the
	// OS to look at the main window associated with the calling thread, us!
	//
	// W variants: an A pump would convert the Unicode window's WM_CHAR back to the code page
	while( PeekMessageW( &msg, nullptr, 0, 0, PM_NOREMOVE ) )
	{

		// get the message
		returnValue = GetMessageW( &msg, nullptr, 0, 0 );

		// this is one possible way to check for quitting conditions as a message
		// of WM_QUIT will cause GetMessage() to return 0
/*
		if( returnValue == 0 )
		{

			setQuitting( true );
			break;

		}
*/

		TheMessageTime = msg.time;
		// translate and dispatch the message
		TranslateMessage( &msg );
		DispatchMessageW( &msg );
		TheMessageTime = 0;

	}

}

