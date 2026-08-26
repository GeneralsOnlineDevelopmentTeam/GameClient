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

// FILE: PopupJoinGame.cpp /////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Electronic Arts Pacific.
//
//                       Confidential Information
//                Copyright (C) 2002 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
//	created:	Jul 2002
//
//	Filename: 	PopupJoinGame.cpp
//
//	author:		Matthew D. Campbell
//
//	purpose:	Contains the Callbacks for the Join Game Popup
//
//-----------------------------------------------------------------------------
///////////////////////////////////////////////////////////////////////////////

//-----------------------------------------------------------------------------
// SYSTEM INCLUDES ////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// USER INCLUDES //////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GlobalData.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GUICallbacks.h"	// liveWatchOpenPasswordPopup
#include "GameClient/LiveObserverSession.h"	// StartLiveObserverSession
#include "GameClient/LobbyObserverMenu.h"	// SetLobbyObserverModeWithPassword
#include "GameClient/Shell.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "../ngmp_include.h"
#include "../ngmp_interfaces.h"


//-----------------------------------------------------------------------------
// DEFINES ////////////////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static NameKeyType parentPopupID = NAMEKEY_INVALID;
static NameKeyType textEntryGamePasswordID = NAMEKEY_INVALID;
static NameKeyType buttonCancelID = NAMEKEY_INVALID;

static GameWindow *parentPopup = nullptr;
static GameWindow *textEntryGamePassword = nullptr;

static void joinGame( AsciiString password );

// Watch-live mode: this popup doubles as the password gate for a password-protected livestream,
// where the "join" queues an observer session instead of joining the lobby. Observe mode is the
// pre-game variant, which opens the read-only lobby view carrying the password.
static Bool s_watchLiveMode = FALSE;
static Bool s_watchLiveObserveMode = FALSE;
static Bool s_watchLivePopShellOnSubmit = FALSE;
static AsciiString s_watchLiveLobbyId;
static AsciiString s_watchLiveDisplayName;

void liveWatchOpenPasswordPopup(const AsciiString& lobbyId, const AsciiString& displayName,
	Bool bPopShellOnSubmit)
{
	s_watchLiveMode = TRUE;
	s_watchLiveObserveMode = FALSE;
	s_watchLivePopShellOnSubmit = bPopShellOnSubmit;
	s_watchLiveLobbyId = lobbyId;
	s_watchLiveDisplayName = displayName;

	GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
}

void liveWatchOpenObservePasswordPopup(const AsciiString& lobbyId, const AsciiString& displayName)
{
	s_watchLiveMode = TRUE;
	s_watchLiveObserveMode = TRUE;
	s_watchLivePopShellOnSubmit = FALSE;
	s_watchLiveLobbyId = lobbyId;
	s_watchLiveDisplayName = displayName;

	GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
}

//-----------------------------------------------------------------------------
// PUBLIC FUNCTIONS ///////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

//-------------------------------------------------------------------------------------------------
/** Initialize the PopupHostGameInit menu */
//-------------------------------------------------------------------------------------------------
void PopupJoinGameInit( WindowLayout *layout, void *userData )
{
	parentPopupID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:ParentJoinPopUp");
	parentPopup = TheWindowManager->winGetWindowFromId(nullptr, parentPopupID);

	textEntryGamePasswordID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:TextEntryGamePassword");
	textEntryGamePassword = TheWindowManager->winGetWindowFromId(parentPopup, textEntryGamePasswordID);
	GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);

	// Mask the password input; secretText makes the gadget draw asterisks while keeping the real
	// text. Applies to the lobby join and the watch-live gate alike - both share this popup.
	EntryData *entryData = (EntryData *)textEntryGamePassword->winGetUserData();
	if (entryData)
		entryData->secretText = TRUE;

	NameKeyType staticTextGameNameID = TheNameKeyGenerator->nameToKey("PopupJoinGame.wnd:StaticTextGameName");
	GameWindow *staticTextGameName = TheWindowManager->winGetWindowFromId(parentPopup, staticTextGameNameID);
	GadgetStaticTextSetText(staticTextGameName, UnicodeString::TheEmptyString);

	buttonCancelID = NAMEKEY("PopupJoinGame.wnd:ButtonCancel");

	if (s_watchLiveMode)
	{
		// Skip the lobby-join setup entirely; the submit handler queues an observer session.
		UnicodeString lobbyName(from_utf8(s_watchLiveDisplayName.str()).c_str());
		if (lobbyName.isEmpty())
			lobbyName = UnicodeString(L"Enter password to watch");
		GadgetStaticTextSetText(staticTextGameName, lobbyName);

		TheWindowManager->winSetFocus(textEntryGamePassword);
		TheWindowManager->winSetModal( parentPopup );
		return;
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		DEBUG_LOG(("NGMP_OnlineServices_LobbyInterface is not initialized!"));
		return;
	}

	LobbyEntry lobbyTryingToJoin = pLobbyInterface->GetLobbyTryingToJoin();
	UnicodeString lobbyName(from_utf8(lobbyTryingToJoin.name).c_str());
	GadgetStaticTextSetText(staticTextGameName, lobbyName);

	TheWindowManager->winSetFocus(textEntryGamePassword);
	TheWindowManager->winSetModal( parentPopup );

}

//-------------------------------------------------------------------------------------------------
/** PopupHostGameInput callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupJoinGameInput( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
//			if (buttonPushed)
//				break;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
						if (s_watchLiveMode)
						{
							s_watchLiveMode = FALSE;
							s_watchLiveObserveMode = FALSE;
							s_watchLivePopShellOnSubmit = FALSE;
							s_watchLiveLobbyId.clear();
							s_watchLiveDisplayName.clear();
						}
						else
						{
							SetLobbyAttemptHostJoin( FALSE );
						}
						parentPopup = nullptr;
					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** PopupHostGameSystem callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType PopupJoinGameSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 )
{
  switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{

			break;

		}
    //---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();
			if (controlID == buttonCancelID)
			{
				GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
				if (s_watchLiveMode)
				{
					s_watchLiveMode = FALSE;
					s_watchLiveObserveMode = FALSE;
					s_watchLivePopShellOnSubmit = FALSE;
					s_watchLiveLobbyId.clear();
					s_watchLiveDisplayName.clear();
				}
				else
				{
					SetLobbyAttemptHostJoin( FALSE );
				}
				parentPopup = nullptr;
			}
			break;
		}

    //----------------------------------------------------------------------------------------------
    case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			break;

		}
    //---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

      if( controlID == textEntryGamePasswordID )
			{
				// read the user's input and clear the entry box
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryGamePassword ));
				GadgetTextEntrySetText(textEntryGamePassword, UnicodeString::TheEmptyString);
				txtInput.trim();
				if (!txtInput.isEmpty())
				{
					AsciiString munkee;
					munkee.translate(txtInput);
					joinGame(munkee);
				}
			}
			break;
		}
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}


//-----------------------------------------------------------------------------
// PRIVATE FUNCTIONS //////////////////////////////////////////////////////////
//-----------------------------------------------------------------------------

static void joinGame( AsciiString password )
{
	if (s_watchLiveMode)
	{
		// Snapshot before closing the overlay, which resets the mode statics. Popping the shell
		// hands the pending session to the Welcome screen's pump; the pre-game lobby view stays
		// up and pumps its own handoff, so it does not pop.
		const AsciiString lobbyId = s_watchLiveLobbyId;
		const AsciiString displayName = s_watchLiveDisplayName;
		const Bool bPopShellOnSubmit = s_watchLivePopShellOnSubmit;
		const Bool bObserveMode = s_watchLiveObserveMode;

		s_watchLiveMode = FALSE;
		s_watchLiveObserveMode = FALSE;
		s_watchLivePopShellOnSubmit = FALSE;
		s_watchLiveLobbyId.clear();
		s_watchLiveDisplayName.clear();

		GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
		parentPopup = nullptr;

		if (bObserveMode)
		{
			// The lobby view holds the password until the stream goes live; a wrong one
			// reprompts from there.
			SetLobbyObserverModeWithPassword(lobbyId.str(), password.str());
			TheShell->push("Menus/GameSpyGameOptionsMenu.wnd");
			return;
		}

		StartLiveObserverSession(lobbyId, password, displayName);
		if (bPopShellOnSubmit)
			TheShell->pop();
		return;
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		DEBUG_LOG(("NGMP_OnlineServices_LobbyInterface is not initialized!"));
		GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
		SetLobbyAttemptHostJoin(FALSE);
		parentPopup = nullptr;
		return;
	}

	LobbyEntry lobbyTryingToJoin = pLobbyInterface->GetLobbyTryingToJoin();

	if (lobbyTryingToJoin.lobbyID == -1)
	{
		GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
		SetLobbyAttemptHostJoin(FALSE);
		parentPopup = NULL;
		return;
	}

#if defined(GENERALS_ONLINE)
	pLobbyInterface->JoinLobby(lobbyTryingToJoin, password.str());
	
	DEBUG_LOG(("Attempting to join game %d(%s) with password [%s]\n", lobbyTryingToJoin.lobbyID, lobbyTryingToJoin.name.c_str(), password.str()));
#else
	PeerRequest req;
	req.peerRequestType = PeerRequest::PEERREQUEST_JOINSTAGINGROOM;
	req.text = ourRoom->getGameName().str();
	req.stagingRoom.id = ourRoom->getID();
	req.password = password.str();
	TheGameSpyPeerMessageQueue->addRequest(req);
	DEBUG_LOG(("Attempting to join game %d(%ls) with password [%s]", ourRoom->getID(), ourRoom->getGameName().str(), password.str()));
#endif

	GameSpyCloseOverlay(GSOVERLAY_GAMEPASSWORD);
	parentPopup = nullptr;
}
