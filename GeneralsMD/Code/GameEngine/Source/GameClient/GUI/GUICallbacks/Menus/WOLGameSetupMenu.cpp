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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: WOLGameSetupMenu.cpp
// Author: Matt Campbell, December 2001
// Description: WOL Game Options Menu
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "GameClient/GameText.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/CustomMatchPreferences.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/InGameUI.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Mouse.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/MapUtil.h"
#include "GameClient/EstablishConnectionsMenu.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"

#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/NAT.h"
#include "GameNetwork/GUIUtil.h"
#include "GameNetwork/GameSpy/GSConfig.h"

#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include <ws2ipdef.h>
#include <format>
#include "../OnlineServices_Init.h"
NGMPGame* TheNGMPGame = NULL;

void WOLDisplaySlotList( void );

#ifdef RTS_INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

extern std::list<PeerResponse> TheLobbyQueuedUTMs;
extern void MapSelectorTooltip(GameWindow *window, WinInstanceData *instData,	UnsignedInt mouse);


#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
extern Bool g_debugSlots;
void slotListDebugLog(const char *fmt, ...)
{
	static char buf[1024];
	va_list va;
	va_start( va, fmt );
	vsnprintf(buf, 1024, fmt, va );
	va_end( va );

	DEBUG_LOG(("%s", buf));
	if (g_debugSlots)
	{
		UnicodeString msg;
		msg.translate(buf);
		// TODO_NGMP: Impl again
		TheGameSpyInfo->addText(msg, GameSpyColor[GSCOLOR_DEFAULT], NULL);
	}
}
#define SLOTLIST_DEBUG_LOG(x) slotListDebugLog x
#else
#define SLOTLIST_DEBUG_LOG(x) DEBUG_LOG(x)
#endif

// TODO_NGMP: Remove this, make others get it from the service
void SendStatsToOtherPlayers(const GameInfo *game)
{
	PeerRequest req;
	req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
	req.UTM.isStagingRoom = TRUE;
	req.id = "STATS/";
	AsciiString fullStr;
	PSPlayerStats fullStats = TheGameSpyPSMessageQueue->findPlayerStatsByID(TheGameSpyInfo->getLocalProfileID());
	PSPlayerStats subStats;
	subStats.id = fullStats.id;
	subStats.wins = fullStats.wins;
	subStats.losses = fullStats.losses;
	subStats.discons = fullStats.discons;
	subStats.desyncs = fullStats.desyncs;
	subStats.games = fullStats.games;
	subStats.locale = fullStats.locale;
	subStats.gamesAsRandom = fullStats.gamesAsRandom;
	GetAdditionalDisconnectsFromUserFile(&subStats);
	fullStr.format("%d %s", TheGameSpyInfo->getLocalProfileID(), TheGameSpyPSMessageQueue->formatPlayerKVPairs( subStats ));
	req.options = fullStr.str();

	Int localIndex = game->getLocalSlotNum();
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot->isHuman() && i != localIndex)
		{
			AsciiString hostName;
			hostName.translate(slot->getName());
			req.nick = hostName.str();
			DEBUG_LOG(("SendStatsToOtherPlayers() - sending to '%s', data of\n\t'%s'\n", hostName.str(), req.options.c_str()));
			TheGameSpyPeerMessageQueue->addRequest(req);
		}
	}
}

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static Bool isShuttingDown = false;
static Bool buttonPushed = false;
static const char *nextScreen = NULL;
static Bool raiseMessageBoxes = false;
static Bool launchGameNext = FALSE;

// window ids ------------------------------------------------------------------------------
static NameKeyType parentWOLGameSetupID = NAMEKEY_INVALID;

static NameKeyType comboBoxPlayerID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType staticTextPlayerID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID,
																											NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonAcceptID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID,
																									NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxColorID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxPlayerTemplateID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType comboBoxTeamID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
//static NameKeyType buttonStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID,
//																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType buttonMapStartPositionID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };
static NameKeyType genericPingWindowID[MAX_SLOTS] = { NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID,
																										NAMEKEY_INVALID,NAMEKEY_INVALID };

static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType textEntryMapDisplayID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonStartID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType buttonSelectMapID = NAMEKEY_INVALID;
static NameKeyType windowMapID = NAMEKEY_INVALID;

static NameKeyType windowMapSelectMapID = NAMEKEY_INVALID;
static NameKeyType checkBoxUseStatsID = NAMEKEY_INVALID;
static NameKeyType checkBoxLimitSuperweaponsID = NAMEKEY_INVALID;
static NameKeyType comboBoxStartingCashID = NAMEKEY_INVALID;
static NameKeyType checkBoxLimitArmiesID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentWOLGameSetup = NULL;
static GameWindow *buttonBack = NULL;
static GameWindow *buttonStart = NULL;
static GameWindow *buttonSelectMap = NULL;
static GameWindow *buttonEmote = NULL;
static GameWindow *textEntryChat = NULL;
static GameWindow *textEntryMapDisplay = NULL;
static GameWindow *windowMap = NULL;
static GameWindow *checkBoxUseStats = NULL;
static GameWindow *checkBoxLimitSuperweapons = NULL;
static GameWindow *comboBoxStartingCash = NULL;
static GameWindow *checkBoxLimitArmies = NULL;

static GameWindow *comboBoxPlayer[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																									 NULL,NULL,NULL,NULL };
static GameWindow *staticTextPlayer[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																									 NULL,NULL,NULL,NULL };
static GameWindow *buttonAccept[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

static GameWindow *comboBoxColor[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

static GameWindow *comboBoxPlayerTemplate[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

static GameWindow *comboBoxTeam[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

//static GameWindow *buttonStartPosition[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
//																								NULL,NULL,NULL,NULL };
//
static GameWindow *buttonMapStartPosition[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

static GameWindow *genericPingWindow[MAX_SLOTS] = {NULL,NULL,NULL,NULL,
																								NULL,NULL,NULL,NULL };

static const Image *pingImages[3] = { NULL, NULL, NULL };

WindowLayout *WOLMapSelectLayout = NULL;

void PopBackToLobby( void )
{
	// delete TheNAT, its no good for us anymore.
	if (TheNAT != nullptr)
	{
		delete TheNAT;
		TheNAT = NULL;
	}

	if (TheNGMPGame) // this can be blown away by a disconnect on the map transfer screen
	{
		TheNGMPGame->reset();


	}

	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->LeaveCurrentLobby();

	DEBUG_LOG(("PopBackToLobby() - parentWOLGameSetup is %X\n", parentWOLGameSetup));
	if (parentWOLGameSetup)
	{
		nextScreen = "Menus/WOLCustomLobby.wnd";
		TheShell->pop();


	}
}

void updateMapStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], Bool onLoadScreen = FALSE );
void positionStartSpots( GameInfo *myGame, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void positionStartSpots(AsciiString mapName, GameWindow *buttonMapStartPositions[], GameWindow *mapWindow);
void WOLPositionStartSpots( void )
{
	GameWindow *win = windowMap;
	if (WOLMapSelectLayout != NULL) {
		win = TheWindowManager->winGetWindowFromId(NULL, windowMapSelectMapID);

		// get the controls.
		NameKeyType listboxMapID = TheNameKeyGenerator->nameToKey( AsciiString("WOLMapSelectMenu.wnd:ListboxMap") );
		GameWindow *listboxMap = TheWindowManager->winGetWindowFromId( NULL, listboxMapID );

		if (listboxMap != NULL) {
			Int selected;
			UnicodeString map;
			
			// get the selected index
			GadgetListBoxGetSelected( listboxMap, &selected );

			if( selected != -1 )
			{

				// get text of the map to load
				map = GadgetListBoxGetText( listboxMap, selected, 0 );
				
				
				// set the map name in the global data map name
				AsciiString asciiMap;
				const char *mapFname = (const char *)GadgetListBoxGetItemData( listboxMap, selected );
				DEBUG_ASSERTCRASH(mapFname, ("No map item data"));
				if (mapFname) {
					asciiMap = mapFname;
				} else {
					asciiMap.translate( map );
				}

				positionStartSpots(asciiMap, buttonMapStartPosition, win);
			}			
		}

	} else {
		DEBUG_ASSERTCRASH(win != NULL, ("no map preview window"));

		AsciiString map = TheNGMPGame->getMap();
		positionStartSpots( map, buttonMapStartPosition, win);
	}
}
static void savePlayerInfo( void )
{
	if (TheNGMPGame)
	{
		Int slotNum = TheNGMPGame->getLocalSlotNum();
		if (slotNum >= 0)
		{
			NGMPGameSlot *slot = TheNGMPGame->getGameSpySlot(slotNum);
			if (slot)
			{
				// save off some prefs
				CustomMatchPreferences pref;
				pref.setPreferredColor(slot->getColor());
				pref.setPreferredFaction(slot->getPlayerTemplate());
				if (TheNGMPGame->amIHost())
				{
					pref.setPreferredMap(TheNGMPGame->getMap());
				}
				pref.write();
			}
		}
	}
}

// Tooltips -------------------------------------------------------------------------------

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	Int slotIdx = -1;
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if (window == comboBoxPlayer[i] || window == staticTextPlayer[i])
		{
			slotIdx = i;
			break;
		}
	}
	if (slotIdx < 0)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString, -1, NULL, 1.5f );
		return;
	}

	NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	if (!game)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString, -1, NULL, 1.5f );
		return;
	}

	NGMPGameSlot *slot = game->getGameSpySlot(slotIdx);
	if (!slot || !slot->isHuman())
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString, -1, NULL, 1.5f );
		return;
	}

	// for tooltip, we want:
	// * player name
	// * ping
	// * locale
	// * win/loss history
	// * discons/desyncs as one var
	// * favorite army
	// in that order.  got it?  good.

	UnicodeString uName = slot->getName();
// 
// 	AsciiString aName;
// 	aName.translate(uName);
// 	PlayerInfoMap::iterator pmIt = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
// 	if (pmIt == TheGameSpyInfo->getPlayerInfoMap()->end())
// 	{
// 		TheMouse->setCursorTooltip( uName, -1, NULL, 1.5f );
// 		return;
// 	}
	//Int profileID = pmIt->second.m_profileID;

	int64_t profileID = slot->m_userID;

	//TheMouse->setCursorTooltip(UnicodeString(L"Retrieving User Stats..."), -1, NULL, 1.5f);

	NGMP_OnlineServicesManager::GetInstance()->GetStatsInterface()->findPlayerStatsByID(profileID, [=](bool bSuccess, PSPlayerStats stats)
	{
			if (stats.id == 0)
			{
				TheMouse->setCursorTooltip(uName, -1, NULL, 1.5f);
				return;
			}

			Bool isLocalPlayer = slot == game->getGameSpySlot(game->getLocalSlotNum());

			//AsciiString localeIdentifier;
			//localeIdentifier.format("WOL:Locale%2.2d", stats.locale);
			UnicodeString	playerInfo;
			Int totalWins = 0, totalLosses = 0, totalDiscons = 0;
			PerGeneralMap::iterator it;

			for (it = stats.wins.begin(); it != stats.wins.end(); ++it)
			{
				totalWins += it->second;
			}
			for (it = stats.losses.begin(); it != stats.losses.end(); ++it)
			{
				totalLosses += it->second;
			}
			for (it = stats.discons.begin(); it != stats.discons.end(); ++it)
			{
				totalDiscons += it->second;
			}
			for (it = stats.desyncs.begin(); it != stats.desyncs.end(); ++it)
			{
				totalDiscons += it->second;
			}
			UnicodeString favoriteSide;
			Int numGames = 0;
			Int favorite = 0;
			for (it = stats.games.begin(); it != stats.games.end(); ++it)
			{
				if (it->second >= numGames)
				{
					numGames = it->second;
					favorite = it->first;
				}
			}
			if (numGames == 0)
				favoriteSide = TheGameText->fetch("GUI:None");
			else if (stats.gamesAsRandom >= numGames)
				favoriteSide = TheGameText->fetch("GUI:Random");
			else
			{
				const PlayerTemplate* fac = ThePlayerTemplateStore->getNthPlayerTemplate(favorite);
				if (fac)
				{
					AsciiString side;
					side.format("SIDE:%s", fac->getSide().str());

					favoriteSide = TheGameText->fetch(side);
				}
			}

			playerInfo.format(L"\nLatency: %d ms\nWins: %d\nLosses: %d\nDisconnects: %d\nFavorite Army: %s",
				slot->getPingAsInt(), totalWins, totalLosses, totalDiscons, favoriteSide.str());

			UnicodeString tooltip = UnicodeString::TheEmptyString;
			if (isLocalPlayer)
			{
				tooltip.format(TheGameText->fetch("TOOLTIP:LocalPlayer"), uName.str());
			}
			else
			{
				// not us
				// TODO_NGMP: Impl friends again
				bool bIsFriend = false;
				if (bIsFriend)
				//if (TheGameSpyInfo->getBuddyMap()->find(profileID) != TheGameSpyInfo->getBuddyMap()->end())
				{
					// buddy
					tooltip.format(TheGameText->fetch("TOOLTIP:BuddyPlayer"), uName.str());
				}
				else
				{
					if (profileID)
					{
						// non-buddy profiled player
						tooltip.format(TheGameText->fetch("TOOLTIP:ProfiledPlayer"), uName.str());
					}
					else
					{
						// non-profiled player
						tooltip.format(TheGameText->fetch("TOOLTIP:GenericPlayer"), uName.str());
					}
				}
			}

			tooltip.concat(playerInfo);

			TheMouse->setCursorTooltip(tooltip, -1, NULL, 1.5f); // the text and width are the only params used.  the others are the default values.

	}, EStatsRequestPolicy::RESPECT_CACHE_ALLOW_REQUEST);
}

void gameAcceptTooltip(GameWindow *window, WinInstanceData *instData, UnsignedInt mouse)
{
	Int x, y;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	Int winPosX, winPosY, winWidth, winHeight;

	window->winGetScreenPosition(&winPosX, &winPosY);

	window->winGetSize(&winWidth, &winHeight);

	if ((x > winPosX && x < (winPosX + winWidth)) && (y > winPosY && y < (winPosY + winHeight)))
	{
		TheMouse->setCursorTooltip(TheGameText->fetch("TOOLTIP:GameAcceptance"), -1, NULL);
	}
}

void pingTooltip(GameWindow *window, WinInstanceData *instData, UnsignedInt mouse)
{
	Int x, y;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	
	Int winPosX, winPosY, winWidth, winHeight;

	window->winGetScreenPosition(&winPosX, &winPosY);

	window->winGetSize(&winWidth, &winHeight);

	if ((x > winPosX && x < (winPosX + winWidth)) && (y > winPosY && y < (winPosY + winHeight)))
	{
		TheMouse->setCursorTooltip(TheGameText->fetch("TOOLTIP:ConnectionSpeed"), -1, NULL);
	}
}

//external declarations of the Gadgets the callbacks can use
GameWindow *listboxGameSetupChat = NULL;
NameKeyType listboxGameSetupChatID = NAMEKEY_INVALID;

static void handleColorSelection(int index)
{
	GameWindow *combo = comboBoxColor[index];
	Int color, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	color = (Int)GadgetComboBoxGetItemData(combo, selIndex);

	NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();

	if (myGame)
	{
		GameSlot * slot = myGame->getSlot(index);
		if (color == slot->getColor())
			return;

		if (color >= -1 && color < TheMultiplayerSettings->getNumColors())
		{
			Bool colorAvailable = TRUE;
			if(color != -1 )
			{
				for(Int i=0; i <MAX_SLOTS; i++)
				{
					GameSlot *checkSlot = myGame->getSlot(i);
					if(color == checkSlot->getColor() && slot != checkSlot)
					{
						colorAvailable = FALSE;
						break;
					}
				}
			}

			// TODO_NGMP: Enforce this on the service too
			if(!colorAvailable)
				return;
		}

		// NGMP: Dont set it locally / directly anymore, rely on the lobby service instead
		//slot->setColor(color);

		// NGMP: Update lobby (if local, for remote players we'll get it from the service)
		if (index == myGame->getLocalSlotNum())
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_MyColor(color);
		}
		else if (slot->getState() == SLOT_EASY_AI || slot->getState() == SLOT_MED_AI || slot->getState() == SLOT_BRUTAL_AI)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_AIColor(index, color);
		}
	}
}

static void handlePlayerTemplateSelection(int index)
{
	GameWindow *combo = comboBoxPlayerTemplate[index];
	Int playerTemplate, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	playerTemplate = (Int)GadgetComboBoxGetItemData(combo, selIndex);
	NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();

	if (myGame)
	{
		GameSlot * slot = myGame->getSlot(index);
		if (playerTemplate == slot->getPlayerTemplate())
			return;

		Int oldTemplate = slot->getPlayerTemplate();
		slot->setPlayerTemplate(playerTemplate);

		int updatedStartPos = slot->getStartPos();

		if (oldTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// was observer, so populate color & team with all, and enable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
			updatedStartPos = -1;
		}
		else if (playerTemplate == PLAYERTEMPLATE_OBSERVER)
		{
			// is becoming observer, so populate color & team with random only, and disable
			GadgetComboBoxSetSelectedPos(comboBoxColor[index], 0);
			GadgetComboBoxSetSelectedPos(comboBoxTeam[index], 0);
			slot->setStartPos(-1);
			updatedStartPos = -1;
		}

		// NGMP: Update lobby (if local, for remote players we'll get it from the service)
		if (index == myGame->getLocalSlotNum())
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_MySide(playerTemplate, updatedStartPos);
		}
		else if (slot->getState() == SLOT_EASY_AI || slot->getState() == SLOT_MED_AI || slot->getState() == SLOT_BRUTAL_AI)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_AISide(index, playerTemplate, updatedStartPos);
		}

		// NGMP: Dont set it locally / directly anymore, rely on the lobby service instead
		/*
		if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost())
		{
			// send around a new slotlist
			myGame->resetAccepted();

			// TODO_NGMP
			//TheGameSpyInfo->setGameOptions();
			WOLDisplaySlotList();
		}
		else
		{
			// request the playerTemplate from the host

			// TODO_NGMP:
			/*
			AsciiString options;
			options.format("PlayerTemplate=%d", playerTemplate);
			AsciiString hostName;
			hostName.translate(myGame->getSlot(0)->getName());
			PeerRequest req;
			req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
			req.UTM.isStagingRoom = TRUE;
			req.id = "REQ/";
			req.nick = hostName.str();
			req.options = options.str();
			TheGameSpyPeerMessageQueue->addRequest(req);
			*/
		/*
		}
			*/
	}
}


static void handleStartPositionSelection(Int player, int startPos)
{
	NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	
	if (myGame)
	{
		NGMPGameSlot * slot = myGame->getGameSpySlot(player);
		if (!slot)
			return;

		if (startPos == slot->getStartPos())
			return;
		Bool skip = FALSE;
		if (startPos < 0)
		{
			skip = TRUE;
		}

		if(!skip)
		{	
			Bool isAvailable = TRUE;
			for(Int i = 0; i < MAX_SLOTS; ++i)
			{
				if(i != player && myGame->getSlot(i)->getStartPos() == startPos)
				{
					isAvailable = FALSE;
					break;
				}
			}
			if( !isAvailable )
				return;
		}

		// NGMP: Dont set it locally / directly anymore, rely on the lobby service instead
		//slot->setStartPos(startPos);

		// NGMP: Update lobby (if local, for remote players we'll get it from the service)
		//		 We also allow host to set AI here
		if (player == myGame->getLocalSlotNum())
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_MyStartPos(startPos);
		}
		else if (myGame->amIHost() && slot->isAI()) // AI + Host
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_AIStartPos(player, startPos);
		}
	}
}



static void handleTeamSelection(int index)
{
	GameWindow *combo = comboBoxTeam[index];
	Int team, selIndex;
	GadgetComboBoxGetSelectedPos(combo, &selIndex);
	team = (Int)GadgetComboBoxGetItemData(combo, selIndex);

	NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();

	if (myGame)
	{
		GameSlot * slot = myGame->getSlot(index);
		if (team == slot->getTeamNumber())
			return;

		// NGMP: Dont set it locally / directly anymore, rely on the lobby service instead
		//slot->setTeamNumber(team);

		// NGMP: Update lobby (if local, for remote players we'll get it from the service)
		if (index == myGame->getLocalSlotNum())
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_MyTeam(team);
		}
		else if (slot->getState() == SLOT_EASY_AI || slot->getState() == SLOT_MED_AI || slot->getState() == SLOT_BRUTAL_AI)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_AITeam(index, team);
		}
	}
}

static void handleStartingCashSelection()
{
	// update it on the service
	Int selIndex;
	GadgetComboBoxGetSelectedPos(comboBoxStartingCash, &selIndex);

	UnsignedInt startingCashValue = (UnsignedInt)GadgetComboBoxGetItemData(comboBoxStartingCash, selIndex);

	Money startingCash;
	startingCash.deposit(startingCashValue, FALSE);
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_StartingCash(startingCashValue);
}

static void handleLimitSuperweaponsClick()
{
	// update it on the service
	bool bLimitSuperweapons = GadgetCheckBoxIsChecked(checkBoxLimitSuperweapons);
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_LimitSuperweapons(bLimitSuperweapons);
}


static void StartPressed(void)
{
	Bool isReady = TRUE;
	Bool allHaveMap = TRUE;
	Int playerCount = 0;
	Int humanCount = 0;

	NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	if (!myGame)
		return;

	// see if everyone's accepted and count the number of players in the game
	UnicodeString mapDisplayName;
	const MapMetaData *mapData = TheMapCache->findMap( myGame->getMap() );
	Bool willTransfer = TRUE;
	if (mapData)
	{
		mapDisplayName.format(L"%ls", mapData->m_displayName.str());
		willTransfer = !mapData->m_isOfficial;
	}
	else
	{
		mapDisplayName.format(L"%hs", myGame->getMap().str());
		willTransfer = WouldMapTransfer(myGame->getMap());
	}
	for( int i = 0; i < MAX_SLOTS; i++ )
	{
		bool bIsAccepted = myGame->getSlot(i)->isAccepted();
		bool bIsHuman = myGame->getSlot(i)->isHuman();
		if ((bIsAccepted == FALSE) && (bIsHuman == TRUE))
		{
			isReady = FALSE;
			if (!myGame->getSlot(i)->hasMap() && !willTransfer)
			{
				UnicodeString msg;
				msg.format(TheGameText->fetch("GUI:PlayerNoMap"), myGame->getSlot(i)->getName().str(), mapDisplayName.str());
				GadgetListBoxAddEntryText(listboxGameSetupChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
				allHaveMap = FALSE;
			}
		}
		if(myGame->getSlot(i)->isOccupied() && myGame->getSlot(i)->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			if (myGame->getSlot(i)->isHuman())
				humanCount++;
			playerCount++;
		}
	}

	// Check for too many players
	const MapMetaData *md = TheMapCache->findMap( myGame->getMap() );
	if (!md || md->m_numPlayers < playerCount)
	{
		if (myGame->amIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetch("LAN:TooManyPlayers"), (md)?md->m_numPlayers:0);
			GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		}
		return;
	}

	// Check for observer + AI players
	if (TheGlobalData->m_netMinPlayers && !humanCount)
	{
		if (myGame->amIHost())
		{
			UnicodeString text = TheGameText->fetch("GUI:NeedHumanPlayers");
			GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		}
		return;
	}

	// Check for too few players
	if (playerCount < TheGlobalData->m_netMinPlayers)
	{
		if (myGame->amIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetch("LAN:NeedMorePlayers"),playerCount);
			GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		}
		return;
	}

	// Check for too few teams
	int numRandom = 0;
	std::set<Int> teams; 
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		GameSlot *slot = myGame->getSlot(i);
		if (slot && slot->isOccupied() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		{
			if (slot->getTeamNumber() >= 0)
			{
				teams.insert(slot->getTeamNumber());
			}
			else
			{
				++numRandom;
			}
		}
	}
	if (numRandom + teams.size() < TheGlobalData->m_netMinPlayers)
	{
		if (myGame->amIHost())
		{
			UnicodeString text;
			text.format(TheGameText->fetch("LAN:NeedMoreTeams"));
			GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		}
		return;
	}

	if (numRandom + teams.size() < 2)
	{
		UnicodeString text;
		text.format(TheGameText->fetch("GUI:SandboxMode"));
		GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
	}

	if(isReady)
	{
		// reset autostart just incase
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ClearAutoReadyCountdown();

		// host mark as ready on backend
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->MarkCurrentGameAsStarted();

		//PeerRequest req;
		//req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAME;
		//TheGameSpyPeerMessageQueue->addRequest(req);

		Lobby_StartGamePacket startGamePacket;
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendToMesh(startGamePacket);

		// process locally too
		NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
		if (pMesh != nullptr)
		{
			Lobby_StartGamePacket startGamePacket2;
			pMesh->ProcessGameStart(startGamePacket2);
		}

		// TODO_NGMP
		//SendStatsToOtherPlayers(myGame);

		// we've started, there's no going back
		// i.e. disable the back button.
		if (buttonBack != nullptr)
		{
			buttonBack->winEnable(FALSE);
		}

		GameWindow *buttonBuddy = TheWindowManager->winGetWindowFromId(NULL, NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator"));
		if (buttonBuddy != nullptr)
		{
			buttonBuddy->winEnable(FALSE);
		}

		GameSpyCloseOverlay(GSOVERLAY_BUDDY);
	}
	else if (allHaveMap)
	{
		// send HWS chat message
		
		if (!NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->HasAutoReadyCountdown())
		{
			// local msg
			GadgetListBoxAddEntryText(listboxGameSetupChat, TheGameText->fetch("GUI:NotifiedStartIntent"), GameSpyColor[GSCOLOR_DEFAULT], -1, -1);

			// remote msg
			UnicodeString strInform = TheGameText->fetch("GUI:HostWantsToStart");
			UnicodeString strInform2 = UnicodeString(L"All players will be forced to ready up in 30 seconds");
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendAnnouncementMessageToCurrentLobby(strInform, false);
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendAnnouncementMessageToCurrentLobby(strInform2, true);

			// TODO_NGMP: Add the reverse too, if everyone is ready but the host wont start... just start it in X seconds

			// start a countdown to auto start
			// TODO_NGMP: Don't have this client driven...
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->StartAutoReadyCountdown();
		}
		else
		{
			UnicodeString strInform = UnicodeString(L"You have already informed players you want to start. A countdown has begun after which they will be marked as ready.");
			GadgetListBoxAddEntryText(listboxGameSetupChat, strInform, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		}
	}

}//void StartPressed(void)


//-------------------------------------------------------------------------------------------------
/** Update options on screen */
//-------------------------------------------------------------------------------------------------
void WOLDisplayGameOptions( void )
{
	if (!parentWOLGameSetup)
		return;

	NGMPGame* theGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();

	if (theGame == nullptr)
	{
		return;
	}

	const GameSlot *localSlot = NULL;
	if (theGame->getLocalSlotNum() >= 0)
		localSlot = theGame->getConstSlot(theGame->getLocalSlotNum());

	AsciiString map = theGame->getMap();
	const MapMetaData *md = TheMapCache->findMap(map);
	if (md && localSlot && localSlot->hasMap())
	{
		GadgetStaticTextSetText(textEntryMapDisplay, md->m_displayName);
	}
	else
	{
		// TODO_NGMP
		/*
		AsciiString s = TheGameSpyInfo->getCurrentStagingRoom()->getMap();
		if (s.reverseFind('\\'))
		{
			s = s.reverseFind('\\') + 1;
		}
		UnicodeString mapDisplay;
		mapDisplay.translate(s);
		GadgetStaticTextSetText(textEntryMapDisplay, mapDisplay);
		*/
	}
	WOLPositionStartSpots();
	updateMapStartSpots(theGame, buttonMapStartPosition);

#if defined(GENERALS_ONLINE)
	Bool isUsingStats = TheNGMPGame->getUseStats();
#else
  //If our display does not match the current state of game settings, update the checkbox.
  Bool isUsingStats = TheGameSpyInfo->getCurrentStagingRoom()->getUseStats() ? TRUE : FALSE;
#endif
  if (GadgetCheckBoxIsChecked(checkBoxUseStats) != isUsingStats)
  {
  	GadgetCheckBoxSetChecked(checkBoxUseStats, isUsingStats);
    checkBoxUseStats->winSetTooltip( TheGameText->fetch( isUsingStats ? "TOOLTIP:UseStatsOn" : "TOOLTIP:UseStatsOff" ) );
  }

  Bool oldFactionsOnly = theGame->oldFactionsOnly();
  if (GadgetCheckBoxIsChecked(checkBoxLimitArmies) != oldFactionsOnly)
  {
    GadgetCheckBoxSetChecked(checkBoxLimitArmies, oldFactionsOnly);
    // Repopulate the lists of available armies, since the old list is now wrong
    for (Int i = 0; i < MAX_SLOTS; i++)
    {
      PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, theGame, theGame->getAllowObservers() );      

      // Make sure selections are up to date on all machines
      handlePlayerTemplateSelection(i) ;
    }
  }

  // Note: must check if checkbox is already correct to avoid infinite recursion
  Bool limitSuperweapons = (theGame->getSuperweaponRestriction() != 0);
  if ( limitSuperweapons != GadgetCheckBoxIsChecked(checkBoxLimitSuperweapons))
    GadgetCheckBoxSetChecked( checkBoxLimitSuperweapons, limitSuperweapons );
  
  Int itemCount = GadgetComboBoxGetLength(comboBoxStartingCash);
  Int index = 0;
  for ( ; index < itemCount; index++ )
  {
    Int value  = (Int)GadgetComboBoxGetItemData(comboBoxStartingCash, index);
    if ( value == theGame->getStartingCash().countMoney() )
    {
      // Note: must check if combobox is already correct to avoid infinite recursion
      Int selectedIndex;
      GadgetComboBoxGetSelectedPos( comboBoxStartingCash, &selectedIndex );
      if ( index != selectedIndex )
        GadgetComboBoxSetSelectedPos(comboBoxStartingCash, index, TRUE);

      break;
    }
  }
  
  DEBUG_ASSERTCRASH( index < itemCount, ("Could not find new starting cash amount %d in list", theGame->getStartingCash().countMoney() ) );
}


//  -----------------------------------------------------------------------------------------
// The Bad munkee slot list displaying function
//-------------------------------------------------------------------------------------------------
void WOLDisplaySlotList( void )
{
	// TODO_NGMP
	//if (!parentWOLGameSetup || !TheGameSpyInfo->getCurrentStagingRoom())
	//	return;

	NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	if (game == nullptr || !game->isInGame())
		return;

	DEBUG_ASSERTCRASH(!game->getConstSlot(0)->isOpen(), ("Open host!"));

	UpdateSlotList( game, comboBoxPlayer, comboBoxColor,
		comboBoxPlayerTemplate, comboBoxTeam, buttonAccept, buttonStart, buttonMapStartPosition );

	WOLDisplayGameOptions();

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		NGMPGameSlot *slot = game->getGameSpySlot(i);
		if (slot && slot->isHuman())
		{
			if (i == game->getLocalSlotNum())
			{
				LobbyMemberEntry member = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetRoomMemberFromID(slot->m_userID);

				std::string strPingString = "";
				std::string strConnectionState = "Not Connected";
				int latency = -1;

				if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh() != nullptr)
				{
					PlayerConnection* pConnection = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh()->GetConnectionForUser(slot->m_userID);

					if (pConnection != nullptr)
					{
						strConnectionState = "Connected";
						latency = pConnection->latency;

					}
				}
				
				strPingString = std::format("Display Name: {}\nUser ID: {}\nConnection Type: {}\nLatency: {}",
					member.display_name.c_str(),
					member.user_id,
					strConnectionState.c_str(),
					latency);

				slot->setPingString(strPingString.c_str());
			}

			if (genericPingWindow[i])
			{
				genericPingWindow[i]->winHide(FALSE);

				genericPingWindow[i]->winSetEnabledImage(0, pingImages[0]);

				Int ping = slot->getPingAsInt();
				if (ping < 100)
				{
					genericPingWindow[i]->winSetEnabledImage(0, pingImages[0]);
				}
				else if (ping < 200)
				{
					genericPingWindow[i]->winSetEnabledImage(0, pingImages[1]);
				}
				else
				{
					genericPingWindow[i]->winSetEnabledImage(0, pingImages[2]);
				}
			}
		}
		else
		{
			if (genericPingWindow[i])
				genericPingWindow[i]->winHide(TRUE);
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the Gadgets Options Menu */
//-------------------------------------------------------------------------------------------------
void InitWOLGameGadgets( void )
{
	ClearGSMessageBoxes();

	NGMPGame* theGameInfo = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	pingImages[0] = TheMappedImageCollection->findImageByName("Ping03");
	pingImages[1] = TheMappedImageCollection->findImageByName("Ping02");
	pingImages[2] = TheMappedImageCollection->findImageByName("Ping01");
	DEBUG_ASSERTCRASH(pingImages[0], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[1], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[2], ("Can't find ping image!"));

	//Initialize the gadget IDs
	parentWOLGameSetupID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:GameSpyGameOptionsMenuParent" ) );
	buttonBackID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:ButtonBack" ) );
	buttonStartID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:ButtonStart" ) );
	textEntryChatID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:TextEntryChat" ) );
	textEntryMapDisplayID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:TextEntryMapDisplay" ) );
	listboxGameSetupChatID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:ListboxChatWindowGameSpyGameSetup" ) );
	buttonEmoteID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:ButtonEmote" ) );
	buttonSelectMapID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:ButtonSelectMap" ) );
	checkBoxUseStatsID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:CheckBoxUseStats" ) );
	windowMapID = TheNameKeyGenerator->nameToKey( AsciiString( "GameSpyGameOptionsMenu.wnd:MapWindow" ) );
  checkBoxLimitSuperweaponsID = TheNameKeyGenerator->nameToKey(AsciiString("GameSpyGameOptionsMenu.wnd:CheckboxLimitSuperweapons"));
  comboBoxStartingCashID = TheNameKeyGenerator->nameToKey(AsciiString("GameSpyGameOptionsMenu.wnd:ComboBoxStartingCash"));
  checkBoxLimitArmiesID = TheNameKeyGenerator->nameToKey(AsciiString("GameSpyGameOptionsMenu.wnd:CheckBoxLimitArmies"));
	windowMapSelectMapID = TheNameKeyGenerator->nameToKey(AsciiString("WOLMapSelectMenu.wnd:WinMapPreview"));

	NameKeyType staticTextTitleID = NAMEKEY("GameSpyGameOptionsMenu.wnd:StaticTextGameName");

	// Initialize the pointers to our gadgets
	parentWOLGameSetup = TheWindowManager->winGetWindowFromId( NULL, parentWOLGameSetupID );
	buttonEmote = TheWindowManager->winGetWindowFromId( parentWOLGameSetup,buttonEmoteID  );
	buttonSelectMap = TheWindowManager->winGetWindowFromId( parentWOLGameSetup,buttonSelectMapID  );
	checkBoxUseStats = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, checkBoxUseStatsID );
	buttonStart = TheWindowManager->winGetWindowFromId( parentWOLGameSetup,buttonStartID  );
	buttonBack = TheWindowManager->winGetWindowFromId( parentWOLGameSetup,  buttonBackID);
	listboxGameSetupChat = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, listboxGameSetupChatID );
	textEntryChat = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, textEntryChatID );
	textEntryMapDisplay = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, textEntryMapDisplayID );
	windowMap = TheWindowManager->winGetWindowFromId( parentWOLGameSetup,windowMapID  );
  DEBUG_ASSERTCRASH(windowMap, ("Could not find the parentWOLGameSetup.wnd:MapWindow" ));

  checkBoxLimitSuperweapons = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, checkBoxLimitSuperweaponsID );
  DEBUG_ASSERTCRASH(windowMap, ("Could not find the GameSpyGameOptionsMenu.wnd:CheckboxLimitSuperweapons" ));
  comboBoxStartingCash = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, comboBoxStartingCashID );
  DEBUG_ASSERTCRASH(windowMap, ("Could not find the GameSpyGameOptionsMenu.wnd:ComboBoxStartingCash" ));

#if defined(GENERALS_ONLINE)
  PopulateStartingCashComboBox(comboBoxStartingCash, theGameInfo);
#else
  PopulateStartingCashComboBox( comboBoxStartingCash, TheGameSpyGame );
#endif
  checkBoxLimitArmies = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, checkBoxLimitArmiesID );
  DEBUG_ASSERTCRASH(windowMap, ("Could not find the GameSpyGameOptionsMenu.wnd:CheckBoxLimitArmies" ));

  // Limit Armies can ONLY be set in the Host Game window (PopupHostGame.wnd)
  checkBoxLimitArmies->winEnable( false );
  // Ditto use stats
  checkBoxUseStats->winEnable( false );

#if defined(GENERALS_ONLINE)
  Int isUsingStats = theGameInfo->getUseStats();
#else
  Int isUsingStats = TheGameSpyGame->getUseStats();
#endif

	
  GadgetCheckBoxSetChecked(checkBoxUseStats, isUsingStats );
  checkBoxUseStats->winSetTooltip( TheGameText->fetch( isUsingStats ? "TOOLTIP:UseStatsOn" : "TOOLTIP:UseStatsOff" ) );

#if defined(GENERALS_ONLINE)
  if (!NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost())
#else
  if ( !TheGameSpyGame->amIHost() )
#endif
  {
    checkBoxLimitSuperweapons->winEnable( false );
    comboBoxStartingCash->winEnable( false );
		NameKeyType labelID = TheNameKeyGenerator->nameToKey(AsciiString("GameSpyGameOptionsMenu.wnd:StartingCashLabel"));
		TheWindowManager->winGetWindowFromId(parentWOLGameSetup, labelID)->winEnable( FALSE );
  }
#if defined(GENERALS_ONLINE)
  else
  {
	  checkBoxLimitSuperweapons->winEnable(true);
	  comboBoxStartingCash->winEnable(true);
  }
#endif


#if !defined(GENERALS_ONLINE_ALLOW_ALL_SETTINGS_FOR_STATS_MATCHES)
	if (isUsingStats)
	{
		// Recorded stats games can never limit superweapons, limit armies, or have inflated starting cash.
		// This should probably be enforced at the gamespy level as well, to prevent expoits.
		checkBoxLimitSuperweapons->winEnable( FALSE );
		comboBoxStartingCash->winEnable( FALSE );
		checkBoxLimitArmies->winEnable( FALSE );
		NameKeyType labelID = TheNameKeyGenerator->nameToKey(AsciiString("GameSpyGameOptionsMenu.wnd:StartingCashLabel"));
		TheWindowManager->winGetWindowFromId(parentWOLGameSetup, labelID)->winEnable( FALSE );
	}
#endif

	//Added By Sadullah Nader
	//Tooltip Function set 
	windowMap->winSetTooltipFunc(MapSelectorTooltip);
	//
	
	GameWindow *staticTextTitle = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, staticTextTitleID );
	if (staticTextTitle)
	{
#if defined(GENERALS_ONLINE)
		GadgetStaticTextSetText(staticTextTitle, theGameInfo->getGameName());
#else
		GadgetStaticTextSetText(staticTextTitle, TheGameSpyGame->getGameName());
#endif
	}

	if (!theGameInfo)
	{
		DEBUG_CRASH(("No staging room!"));
		return;
	}

	std::vector<LobbyMemberEntry>& lobbyRoster = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();

	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		AsciiString tmpString;
		tmpString.format("GameSpyGameOptionsMenu.wnd:ComboBoxPlayer%d", i);
		comboBoxPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayer[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, comboBoxPlayerID[i] );
		GadgetComboBoxReset(comboBoxPlayer[i]);
		comboBoxPlayer[i]->winSetTooltipFunc(playerTooltip);

		tmpString.format("GameSpyGameOptionsMenu.wnd:StaticTextPlayer%d", i);
		staticTextPlayerID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		staticTextPlayer[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, staticTextPlayerID[i] );
		staticTextPlayer[i]->winSetTooltipFunc(playerTooltip);
		
		bool bIsHost = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost();
		if (bIsHost)
			staticTextPlayer[i]->winHide(TRUE);

		if(i==0 && bIsHost)
		{
			UnicodeString uName;

			//auto& plrElement = *std::next(lobbyRoster.begin(), i);

			// TODO_NGMP: What if we don't find a user we expect to find?
			LobbyMemberEntry plrElement = lobbyRoster.at(i);

			uName.translate(plrElement.display_name.c_str());
			GadgetComboBoxAddEntry(comboBoxPlayer[i],uName,GameSpyColor[GSCOLOR_PLAYER_OWNER]);
			GadgetComboBoxSetSelectedPos(comboBoxPlayer[0],0);
		}
		else
		{
			GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Open"),GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:Closed"),GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:EasyAI"),GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:MediumAI"),GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			GadgetComboBoxAddEntry(comboBoxPlayer[i],TheGameText->fetch("GUI:HardAI"),GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			GadgetComboBoxSetSelectedPos(comboBoxPlayer[i],0);
		}

		tmpString.format("GameSpyGameOptionsMenu.wnd:ComboBoxColor%d", i);
		comboBoxColorID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxColor[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, comboBoxColorID[i] );
		DEBUG_ASSERTCRASH(comboBoxColor[i], ("Could not find the comboBoxColor[%d]",i ));

		PopulateColorComboBox(i, comboBoxColor, theGameInfo);
		GadgetComboBoxSetSelectedPos(comboBoxColor[i], 0);
		
		tmpString.format("GameSpyGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d", i);
		comboBoxPlayerTemplateID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxPlayerTemplate[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, comboBoxPlayerTemplateID[i] );
		DEBUG_ASSERTCRASH(comboBoxPlayerTemplate[i], ("Could not find the comboBoxPlayerTemplate[%d]",i ));

		// add tooltips to the player template combobox and listbox
		comboBoxPlayerTemplate[i]->winSetTooltipFunc(playerTemplateComboBoxTooltip);
		GadgetComboBoxGetListBox(comboBoxPlayerTemplate[i])->winSetTooltipFunc(playerTemplateListBoxTooltip);

		tmpString.format("GameSpyGameOptionsMenu.wnd:ComboBoxTeam%d", i);
		comboBoxTeamID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		comboBoxTeam[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, comboBoxTeamID[i] );
		DEBUG_ASSERTCRASH(comboBoxTeam[i], ("Could not find the comboBoxTeam[%d]",i ));

		PopulateTeamComboBox(i, comboBoxTeam, theGameInfo);

		tmpString.format("GameSpyGameOptionsMenu.wnd:ButtonAccept%d", i); 
		buttonAcceptID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonAccept[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, buttonAcceptID[i] );
		DEBUG_ASSERTCRASH(buttonAccept[i], ("Could not find the buttonAccept[%d]",i ));
		buttonAccept[i]->winSetTooltipFunc(gameAcceptTooltip);

		tmpString.format("GameSpyGameOptionsMenu.wnd:GenericPing%d", i); 
		genericPingWindowID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		genericPingWindow[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, genericPingWindowID[i] );
		DEBUG_ASSERTCRASH(genericPingWindow[i], ("Could not find the genericPingWindow[%d]",i ));
		genericPingWindow[i]->winSetTooltipFunc(pingTooltip);

//		tmpString.format("GameSpyGameOptionsMenu.wnd:ButtonStartPosition%d", i);
//		buttonStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
//		buttonStartPosition[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, buttonStartPositionID[i] );
//		DEBUG_ASSERTCRASH(buttonStartPosition[i], ("Could not find the ButtonStartPosition[%d]",i ));

		tmpString.format("GameSpyGameOptionsMenu.wnd:ButtonMapStartPosition%d", i);
		buttonMapStartPositionID[i] = TheNameKeyGenerator->nameToKey( tmpString );
		buttonMapStartPosition[i] = TheWindowManager->winGetWindowFromId( parentWOLGameSetup, buttonMapStartPositionID[i] );
		DEBUG_ASSERTCRASH(buttonMapStartPosition[i], ("Could not find the ButtonMapStartPosition[%d]",i ));

//		if (buttonStartPosition[i])
//			buttonStartPosition[i]->winHide(TRUE);

		if(i !=0 && buttonAccept[i])
			buttonAccept[i]->winHide(TRUE);
	}

	if( buttonAccept[0] )
		buttonAccept[0]->winEnable(TRUE);

	if (buttonBack != NULL)
	{
		buttonBack->winEnable(TRUE);
	}
		//GadgetButtonSetEnabledColor(buttonAccept[0], GameSpyColor[GSCOLOR_ACCEPT_TRUE]);

		// TODO_NGMP: Where does this happen in the normal game?
#if defined(GENERALS_ONLINE)
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, theGameInfo, theGameInfo->getAllowObservers());

		// Make sure selections are up to date on all machines
		handlePlayerTemplateSelection(i);
	}
#endif
}

void DeinitWOLGameGadgets( void )
{
	parentWOLGameSetup = NULL;
	buttonEmote = NULL;
	buttonSelectMap = NULL;
	buttonStart = NULL;
	buttonBack = NULL;
	listboxGameSetupChat = NULL;
	textEntryChat = NULL;
	textEntryMapDisplay = NULL;
	if (windowMap)
	{
		windowMap->winSetUserData(NULL);
		windowMap = NULL;
	}
	checkBoxUseStats = NULL;
  checkBoxLimitSuperweapons = NULL;
  comboBoxStartingCash = NULL;
  
//	GameWindow *staticTextTitle = NULL;
	for (Int i = 0; i < MAX_SLOTS; i++)
	{
		comboBoxPlayer[i] = NULL;
		staticTextPlayer[i] = NULL;
		comboBoxColor[i] = NULL;
		comboBoxPlayerTemplate[i] = NULL;
		comboBoxTeam[i] = NULL;
		buttonAccept[i] = NULL;
//		buttonStartPosition[i] = NULL;
		buttonMapStartPosition[i] = NULL;
		genericPingWindow[i] = NULL;
	}
}

static Bool initDone = false;
UnsignedInt lastSlotlistTime = 0;
UnsignedInt enterTime = 0;
Bool initialAcceptEnable = FALSE;
//-------------------------------------------------------------------------------------------------
/** Initialize the Lan Game Options Menu */
//-------------------------------------------------------------------------------------------------
void WOLGameSetupMenuInit( WindowLayout *layout, void *userData )
{
	// TODO_NGMP: impl this again
	GameWindow* buttonBuddy = TheWindowManager->winGetWindowFromId(NULL, NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator"));
	if (buttonBuddy)
		buttonBuddy->winEnable(FALSE);

	// register for chat events
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForChatCallback([](UnicodeString strMessage, GameSpyColors color)
		{
			GadgetListBoxAddEntryText(listboxGameSetupChat, strMessage, GameSpyColor[color], -1, -1);
		});

	// cannot connect to the lobby we joined
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForCannotConnectToLobbyCallback([](void)
		{
			if (TheNetwork != NULL) {
				delete TheNetwork;
				TheNetwork = NULL;
			}
			GSMessageBoxOk(TheGameText->fetch("GUI:Error"), UnicodeString(L"Could not connect to all players in the lobby"));

			PopBackToLobby();
		});

	// connection events (for debug really)
	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
	pMesh->RegisterForConnectionEvents([](int64_t userID, std::string strDisplayName, EConnectionState connState)
		{
			std::string strConnectionType = "Unknown";
			if (connState == EConnectionState::CONNECTED_DIRECT)
			{
				strConnectionType = "Direct Connection";
			}
			else
			{
				strConnectionType = "Relayed Connection";
			}

			UnicodeString strConnectionMessage;
			strConnectionMessage.format(L"You are now connected to %hs using connection mechanism %hs", strDisplayName.c_str(), strConnectionType.c_str());
			GadgetListBoxAddEntryText(listboxGameSetupChat, strConnectionMessage, GameMakeColor(255, 194, 15, 255), -1, -1);
		});

	// player doesnt have map events
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForPlayerDoesntHaveMapCallback([](LobbyMemberEntry lobbyMember)
		{
			// tell the host the user doesn't have the map
			UnicodeString mapDisplayName;
			const MapMetaData* mapData = TheMapCache->findMap(TheNGMPGame->getMap());
			Bool willTransfer = TRUE;
			if (mapData)
			{
				mapDisplayName.format(L"%ls", mapData->m_displayName.str());
				willTransfer = !mapData->m_isOfficial;
			}
			else
			{
				mapDisplayName.format(L"%hs", TheNGMPGame->getMap().str());
				willTransfer = WouldMapTransfer(TheNGMPGame->getMap());
			}

			UnicodeString strDisplayName;
			strDisplayName.format(L"%hs", lobbyMember.display_name.c_str());

			UnicodeString text;
			if (willTransfer)
				text.format(TheGameText->fetch("GUI:PlayerNoMapWillTransfer"), strDisplayName.str(), mapDisplayName.str());
			else
				text.format(TheGameText->fetch("GUI:PlayerNoMap"), strDisplayName.str(), mapDisplayName.str());
			GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
		});

	

	// register for roster events
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForRosterNeedsRefreshCallback([]()
		{
			WOLDisplaySlotList();
			WOLDisplayGameOptions();
		});

	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForGameStartPacket([]()
		{
			NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
			if (!myGame || !myGame->isInGame())
				return;

			if (!TheNGMPGame)
				return;

			// TODO_NGMP
			//SendStatsToOtherPlayers(TheNGMPGame);

			// we've started, there's no going back
			// i.e. disable the back button.
			buttonBack->winEnable(FALSE);
			GameWindow* buttonBuddy = TheWindowManager->winGetWindowFromId(NULL, NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator"));
			if (buttonBuddy)
				buttonBuddy->winEnable(FALSE);
			GameSpyCloseOverlay(GSOVERLAY_BUDDY);

			*TheNGMPGame = *myGame;
			TheNGMPGame->startGame(0);
		});


	if (TheNGMPGame && TheNGMPGame->isGameInProgress())
	{
		TheNGMPGame->setGameInProgress(FALSE);
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->LeaveCurrentLobby();

		// check if we were disconnected

		// TODO_NGMP: Handle disconnected
		/*
		Int disconReason;
		if (TheGameSpyInfo->isDisconnectedAfterGameStart(&disconReason))
		{
			AsciiString disconMunkee;
			disconMunkee.format("GUI:GSDisconReason%d", disconReason);
			UnicodeString title, body;
			title = TheGameText->fetch( "GUI:GSErrorTitle" );
			body = TheGameText->fetch( disconMunkee );
			GameSpyCloseAllOverlays();
			GSMessageBoxOk( title, body );
			TheGameSpyInfo->reset();
			DEBUG_LOG(("WOLGameSetupMenuInit() - game was in progress, and we were disconnected, so pop immediate back to main menu\n"));
			TheShell->popImmediate();
			return;
		}
		*/

		// If we init while the game is in progress, we are really returning to the menu
		// after the game.  So, we pop the menu and go back to the lobby.  Whee!
		DEBUG_LOG(("WOLGameSetupMenuInit() - game was in progress, so pop immediate back to lobby\n"));
		TheShell->popImmediate();

		// TODO_NGMP: Only do this if still connected to service
		//if (TheGameSpyPeerMessageQueue && TheGameSpyPeerMessageQueue->isConnected())
		{
			DEBUG_LOG(("We're still connected, so pushing back on the lobby\n"));
			TheShell->push("Menus/WOLCustomLobby.wnd", TRUE);
		}

		
		return;
	}

	// TODO_NGMP
	//TheGameSpyInfo->setCurrentGroupRoom(0);

	if (TheNAT != NULL) {
		delete TheNAT;
		TheNAT = NULL;
	}

	nextScreen = NULL;
	buttonPushed = false;
	isShuttingDown = false;
	launchGameNext = FALSE;

	//initialize the gadgets
	EnableSlotListUpdates(FALSE);
	InitWOLGameGadgets();
	EnableSlotListUpdates(TRUE);
	// TODO_NGMP
	//TheGameSpyInfo->registerTextWindow(listboxGameSetupChat);

	//The dialog needs to react differently depending on whether it's the host or not.
	TheMapCache->updateCache();

	// TODO_NGMP
	NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
	NGMPGameSlot* hostSlot = game->getGameSpySlot(0);
	hostSlot->setAccept();

	bool bIsHost = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost();

	if (bIsHost)
	{
		// TODO_NGMP
		/*
		OptionPreferences natPref;
		CustomMatchPreferences customPref;
		game->setMap(customPref.getPreferredMap());
		*/

		// TODO_NGMP: Preferred color & factionsupport
		hostSlot->setColor(0);
		hostSlot->setPlayerTemplate(PLAYERTEMPLATE_RANDOM);
		//hostSlot->setNATBehavior((FirewallHelperClass::FirewallBehaviorType)natPref.getFirewallBehavior());
		hostSlot->setPingString("TODO_NGMP");

		CustomMatchPreferences customPref;


	// Recorded stats games can never limit superweapons, limit armies, or have inflated starting cash.
		// This should probably be enforced at the gamespy level as well, to prevent expoits.
#if !defined(GENERALS_ONLINE)
		Int isUsingStats = TheGameSpyGame->getUseStats();
#else
#if !defined(GENERALS_ONLINE_ALLOW_ALL_SETTINGS_FOR_STATS_MATCHES)
		Int isUsingStats = game->getUseStats();
#endif
#endif

#if !defined(GENERALS_ONLINE_ALLOW_ALL_SETTINGS_FOR_STATS_MATCHES)
		game->setStartingCash( isUsingStats? TheMultiplayerSettings->getDefaultStartingMoney() : customPref.getStartingCash() );
		game->setSuperweaponRestriction( isUsingStats? 0 : customPref.getSuperweaponRestricted() ? 1 : 0 );
		if (isUsingStats)
			game->setOldFactionsOnly( 0 );
#else
		game->setStartingCash(customPref.getStartingCash());
		game->setSuperweaponRestriction(customPref.getSuperweaponRestricted() ? 1 : 0);
#endif

		//game->setOldFactionsOnly( customPref.getFactionsLimited() );
    if ( game->oldFactionsOnly() )
    {
      // Make sure host follows the old factions only restrictions!
      const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(hostSlot->getPlayerTemplate());

      if ( fac != NULL && !fac->isOldFaction() )
      {
        hostSlot->setPlayerTemplate( PLAYERTEMPLATE_RANDOM );
      }
    }


		for (Int i=1; i<MAX_SLOTS; ++i)
		{
			NGMPGameSlot *slot = game->getGameSpySlot(i);
			slot->setState( SLOT_OPEN );
		}

		// TODO_NGMP: preferred map support
		AsciiString lowerMap = getDefaultOfficialMap();
		//AsciiString lowerMap = customPref.getPreferredMap();
		lowerMap.toLower();
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(lowerMap);
		if (it != TheMapCache->end())
		{
			hostSlot->setMapAvailability(TRUE);
			game->setMapCRC( it->second.m_CRC );
			game->setMapSize( it->second.m_filesize );

			game->adjustSlotsForMap(); // BGC- adjust the slots for the new map.
		}


		WOLDisplaySlotList();
		WOLDisplayGameOptions();
	}
	else
	{
		OptionPreferences natPref;
		CustomMatchPreferences customPref;
		AsciiString options;
		//PeerRequest req;
		UnicodeString uName = hostSlot->getName();
		AsciiString aName;
		aName.translate(uName);

		// TODO_NGMP: Do this on join? and map change
		AsciiString asciiMap = game->getMap();
		asciiMap.toLower();

		// TODO_NGMP: Sync map availability back to host, he needs it
		std::map<AsciiString, MapMetaData>::iterator it = TheMapCache->find(asciiMap);
		if (it != TheMapCache->end())
		{
			// TODO_NGMP: Urgent, this crashes when you join a 2/2 game as the 3rd player, because we shouldnt let you join...
			if (game->getLocalSlotNum() != -1)
			{
				game->getSlot(game->getLocalSlotNum())->setMapAvailability(true);
				game->setMapCRC(it->second.m_CRC);
				game->setMapSize(it->second.m_filesize);
			}
		}
		else
		{
			game->getSlot(game->getLocalSlotNum())->setMapAvailability(false);
			game->setMapCRC(0);
			game->setMapSize(0);
		}

		/*
		req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
		req.UTM.isStagingRoom = TRUE;
		req.id = "REQ/";
		req.nick = aName.str();
		options.format("PlayerTemplate=%d", customPref.getPreferredFaction());
		req.options = options.str();
		TheGameSpyPeerMessageQueue->addRequest(req);
		options.format("Color=%d", customPref.getPreferredColor());
		req.options = options.str();
		TheGameSpyPeerMessageQueue->addRequest(req);
		options.format("NAT=%d", natPref.getFirewallBehavior());
		req.options = options.str();
		TheGameSpyPeerMessageQueue->addRequest(req);
		options.format("Ping=%s", TheGameSpyInfo->getPingString().str());
		req.options = options.str();
		TheGameSpyPeerMessageQueue->addRequest(req);
		*/

		game->setMapCRC( game->getMapCRC() );		// force a recheck
		game->setMapSize( game->getMapSize() ); // of if we have the map

		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			//I'm a client, disable the controls I can't touch.
			comboBoxPlayer[i]->winEnable(FALSE);

			comboBoxColor[i]->winEnable(FALSE);
			comboBoxPlayerTemplate[i]->winEnable(FALSE);
			comboBoxTeam[i]->winEnable(FALSE);
//			buttonStartPosition[i]->winEnable(FALSE);
			buttonMapStartPosition[i]->winEnable(FALSE);

		}
		buttonStart->winSetText(TheGameText->fetch("GUI:Accept"));
		buttonStart->winEnable( FALSE );
		buttonSelectMap->winEnable( FALSE );
		initialAcceptEnable = FALSE;

		WOLDisplaySlotList();
		WOLDisplayGameOptions();
	}

	// Show the Menu
	layout->hide( FALSE );
	
	// Make sure the text fields are clear
	GadgetListBoxReset( listboxGameSetupChat );
	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);	

	initDone = true;

	// TODO_NGMP
	//TheGameSpyInfo->setGameOptions();
	//TheShell->registerWithAnimateManager(parentWOLGameSetup, WIN_ANIMATION_SLIDE_TOP, TRUE);
	WOLPositionStartSpots();

	lastSlotlistTime = 0;
	enterTime = timeGetTime();

	// Set Keyboard to chat entry
	TheWindowManager->winSetFocus( textEntryChat );
	raiseMessageBoxes = true;
	TheTransitionHandler->setGroup("GameSpyGameOptionsMenuFade");

	// NGMP: Did we just enter a lobby with modified camera height?
	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr != nullptr)
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = pOnlineServicesMgr->GetLobbyInterface();

		if (pLobbyInterface != nullptr)
		{
			if (pLobbyInterface->IsInLobby())
			{
				LobbyEntry& theLobby = pLobbyInterface->GetCurrentLobby();

				if (theLobby.max_cam_height != GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM)
				{
					

					if (!pLobbyInterface->IsHost())
					{
						UnicodeString strInform;
						strInform.format(L"NOTE: This lobby has a customized maximum camera height / zoom level of %lu set by the host.", theLobby.max_cam_height);
						GadgetListBoxAddEntryText(listboxGameSetupChat, strInform, GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
					}
					else
					{
						UnicodeString strInform;
						UnicodeString strInform2;
						strInform.format(L"NOTE: This lobby has a customized maximum camera height / zoom level of %lu set as per your preference.", theLobby.max_cam_height);
						strInform2.format(L"\tDo /maxcameraheight <val> to change this (e.g. /maxcameraheight 450).", theLobby.max_cam_height);
					
						GadgetListBoxAddEntryText(listboxGameSetupChat, strInform, GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
						GadgetListBoxAddEntryText(listboxGameSetupChat, strInform2, GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
					}
						
				}
			}
		}
	}
}// void WOLGameSetupMenuInit( WindowLayout *layout, void *userData )

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	isShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (nextScreen != NULL) );

	if (nextScreen != NULL)
	{
		// TODO_NGMP: Handle disconnect again
		if (false)
		//if (!TheGameSpyPeerMessageQueue || !TheGameSpyPeerMessageQueue->isConnected())
		{
			DEBUG_LOG(("GameSetup shutdownComplete() - skipping push because we're disconnected\n"));
		}
		else
		{
			TheShell->push(nextScreen);
		}
	}

	/*
	if (launchGameNext)
	{
		TheNGMPGame->launchGame();
		TheGameSpyInfo->leaveStagingRoom();
	}
	*/

	nextScreen = NULL;

}  // end if

//-------------------------------------------------------------------------------------------------
/** GameSpy Game Options menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLGameSetupMenuShutdown( WindowLayout *layout, void *userData )
{
	//TheGameSpyInfo->unregisterTextWindow(listboxGameSetupChat);

	if( WOLMapSelectLayout )
	{
		WOLMapSelectLayout->destroyWindows();
		deleteInstance(WOLMapSelectLayout);
		WOLMapSelectLayout = NULL;
	}
	parentWOLGameSetup = NULL;
	EnableSlotListUpdates(FALSE);
	DeinitWOLGameGadgets();
	if (TheEstablishConnectionsMenu != NULL)
	{
		TheEstablishConnectionsMenu->endMenu();
	}
	initDone = false;

	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}  //end if

	TheShell->reverseAnimatewindow();

	RaiseGSMessageBox();
	TheTransitionHandler->reverse("GameSpyGameOptionsMenuFade");
}  // void WOLGameSetupMenuShutdown( WindowLayout *layout, void *userData )

static void fillPlayerInfo(const PeerResponse *resp, PlayerInfo *info)
{
	info->m_name			= resp->nick.c_str();
	info->m_profileID	= resp->player.profileID;
	info->m_flags			= resp->player.flags;
	info->m_wins			= resp->player.wins;
	info->m_losses		= resp->player.losses;
	info->m_locale		= resp->locale.c_str();
	info->m_rankPoints= resp->player.rankPoints;
	info->m_side			= resp->player.side;
	info->m_preorder	= resp->player.preorder;
}

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu update method */
//-------------------------------------------------------------------------------------------------
void WOLGameSetupMenuUpdate( WindowLayout * layout, void *userData)
{
	// We'll only be successful if we've requested to 
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
	{
		shutdownComplete(layout);
		return;
	}

	if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_bHostMigrated)
	{
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_bHostMigrated = false;

		// If we are in-game, nothing to do here, the game handles it for us
		if (!TheNGMPGame->isGameInProgress()) // in progress is in game, ingame is just in lobby
		{
			// TODO_NGMP: Make sure we did a lobby get first
			// did we become the host?
			bool bIsHost = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost();

			if (bIsHost)
			{
				// re init our UI & enable host buttons
				EnableSlotListUpdates(FALSE);
				InitWOLGameGadgets();
				EnableSlotListUpdates(TRUE);
				buttonStart->winSetText(TheGameText->fetch("GUI:Start"));
				buttonStart->winEnable(TRUE);
				buttonSelectMap->winEnable(TRUE);
				initialAcceptEnable = TRUE;
				WOLDisplaySlotList();
				WOLDisplayGameOptions();

				NetworkLog("Host left and server migrated the host to us...");

				GadgetListBoxAddEntryText(listboxGameSetupChat, UnicodeString(L"The previous host has left the lobby. You are now the host."), GameMakeColor(255, 255, 255, 255), -1, -1);

				// NOTE: don't need to mark ourselves ready, the service did it for us upon migration
			}
			else
			{
				GadgetListBoxAddEntryText(listboxGameSetupChat, UnicodeString(L"The previous host has left the lobby. a new host has been selected."), GameMakeColor(255, 255, 255, 255), -1, -1);
			}
		}
	}

	if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_bPendingHostHasLeft)
	{
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_bPendingHostHasLeft = false;

		buttonPushed = true;
		DEBUG_LOG(("Host left lobby\n"));
		if (TheNGMPGame)
			TheNGMPGame->reset();

		// TODO_NGMP: Impl host migration, less annoying for users

		GSMessageBoxOk(TheGameText->fetch("GUI:HostLeftTitle"), TheGameText->fetch("GUI:HostLeft"));

		PopBackToLobby();

		return;
	}

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = false;
	}

	if (TheShell->isAnimFinished() && !buttonPushed && TheGameSpyPeerMessageQueue)
	{
		HandleBuddyResponses();
		HandlePersistentStorageResponses();

		if (TheNGMPGame && TheNGMPGame->isGameInProgress())
		{
			if (TheGameSpyInfo->isDisconnectedAfterGameStart(NULL))
			{
				return; // already been disconnected, so don't worry.
			}

			Int allowedMessages = TheGameSpyInfo->getMaxMessagesPerUpdate();
			Bool sawImportantMessage = FALSE;
			PeerResponse resp;
			while (allowedMessages-- && !sawImportantMessage && TheGameSpyPeerMessageQueue->getResponse( resp ))
			{
				switch (resp.peerResponseType)
				{
				case PeerResponse::PEERRESPONSE_DISCONNECT:
					{
					// TODO_NGMP: hook this up again
						sawImportantMessage = TRUE;
						AsciiString disconMunkee;
						disconMunkee.format("GUI:GSDisconReason%d", resp.discon.reason);

						// check for scorescreen
						NameKeyType listboxChatWindowScoreScreenID = NAMEKEY("ScoreScreen.wnd:ListboxChatWindowScoreScreen");
						GameWindow *listboxChatWindowScoreScreen = TheWindowManager->winGetWindowFromId( NULL, listboxChatWindowScoreScreenID );
						if (listboxChatWindowScoreScreen)
						{
							GadgetListBoxAddEntryText(listboxChatWindowScoreScreen, TheGameText->fetch(disconMunkee),
								GameSpyColor[GSCOLOR_DEFAULT], -1);
						}
						else
						{
							// still ingame
							TheInGameUI->message(disconMunkee);
						}
						TheGameSpyInfo->markAsDisconnectedAfterGameStart(resp.discon.reason);
					}
				}
			}

			return; // if we're in game, all we care about is if we've been disconnected from the chat server
		}

		Bool isHosting = TheGameSpyInfo->amIHost(); // only while in game setup screen
		isHosting = isHosting || (TheNGMPGame && TheNGMPGame->isInGame() && TheNGMPGame->amIHost()); // while in game
		if (!isHosting && !lastSlotlistTime && timeGetTime() > enterTime + 10000)
		{
			// don't do this if we're disconnected
			if (TheGameSpyPeerMessageQueue->isConnected())
			{
				// haven't seen ourselves
				buttonPushed = true;
				DEBUG_LOG(("Haven't seen ourselves in slotlist\n"));
				if (TheNGMPGame)
					TheNGMPGame->reset();
				TheGameSpyInfo->leaveStagingRoom();
				//TheGameSpyInfo->joinBestGroupRoom();
				GSMessageBoxOk(TheGameText->fetch("GUI:HostLeftTitle"), TheGameText->fetch("GUI:HostLeft"));
				nextScreen = "Menus/WOLCustomLobby.wnd";
				TheShell->pop();
			}
			return;
		}

		if (TheNAT != NULL) {
			NATStateType NATState = TheNAT->update();
			if (NATState == NATSTATE_DONE)
			{
				//launchGameNext = TRUE;
				//TheShell->pop();
				TheNGMPGame->launchGame();
				if (TheGameSpyInfo) // this can be blown away by a disconnect on the map transfer screen
					TheGameSpyInfo->leaveStagingRoom();
				return;
			}
			else if (NATState == NATSTATE_FAILED)
			{
				// Just back out.  This cleans up some slot list problems
				buttonPushed = true;

				// delete TheNAT, its no good for us anymore.
				delete TheNAT;
				TheNAT = NULL;

				NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
				myGame->reset();
				TheGameSpyInfo->leaveStagingRoom();
				//TheGameSpyInfo->joinBestGroupRoom();
				GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NATNegotiationFailed"));
				nextScreen = "Menus/WOLCustomLobby.wnd";
				TheShell->pop();
				return;
			}
		}

		PeerResponse resp;

		Int allowedMessages = TheGameSpyInfo->getMaxMessagesPerUpdate();
		Bool sawImportantMessage = FALSE;
		while (allowedMessages-- && !sawImportantMessage)
		{

			if (!TheLobbyQueuedUTMs.empty())
			{
				DEBUG_LOG(("Got response from queued lobby UTM list\n"));
				resp = TheLobbyQueuedUTMs.front();
				TheLobbyQueuedUTMs.pop_front();
			}
			else if (TheGameSpyPeerMessageQueue->getResponse( resp ))
			{
				DEBUG_LOG(("Got response from message queue\n"));
			}
			else
			{
				break;
			}

			switch (resp.peerResponseType)
			{
			case PeerResponse::PEERRESPONSE_FAILEDTOHOST:
				{
					// oops - we've not heard from the qr server.  bail.
					GadgetListBoxAddEntryText(listboxGameSetupChat, TheGameText->fetch("GUI:GSFailedToHost"), GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
				}
				break;
			case PeerResponse::PEERRESPONSE_GAMESTART:
				{
					sawImportantMessage = TRUE;
					NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
					if (!myGame || !myGame->isInGame())
						break;

					if (!TheNGMPGame)
						break;

					SendStatsToOtherPlayers(TheNGMPGame);

					// we've started, there's no going back
					// i.e. disable the back button.
					buttonBack->winEnable(FALSE);
					GameWindow *buttonBuddy = TheWindowManager->winGetWindowFromId(NULL, NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator"));
					if (buttonBuddy)
						buttonBuddy->winEnable(FALSE);
					GameSpyCloseOverlay(GSOVERLAY_BUDDY);

					*TheNGMPGame = *myGame;
					TheNGMPGame->startGame(0);
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					WOLDisplaySlotList();
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERINFO:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					WOLDisplaySlotList();
					// send out new slotlist if I'm host
					TheGameSpyInfo->setGameOptions();
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERJOIN:
				{
					if (resp.player.roomType != StagingRoom)
					{
						break;
					}
					sawImportantMessage = TRUE;
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);

					if (p.m_profileID)
					{
						if (TheGameSpyPSMessageQueue->findPlayerStatsByID(p.m_profileID).id == 0)
						{
							PSRequest req;
							req.requestType = PSRequest::PSREQUEST_READPLAYERSTATS;
							req.player.id = p.m_profileID;
							TheGameSpyPSMessageQueue->addRequest(req);
						}
					}

					// check if we have room for the dude
					NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
					if (TheGameSpyInfo->amIHost() && game)
					{
						if (TheNAT)
						{
							// ditch him
							PeerRequest req;
							req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
							req.UTM.isStagingRoom = TRUE;
							req.id = "KICK/";
							req.nick = p.m_name.str();
							req.options = "GameStarted";
							TheGameSpyPeerMessageQueue->addRequest(req);
						}
						else
						{
							// look for room for him
							// See if there's room
							// First get the number of players currently in the room.
							Int numPlayers = 0;
							for (Int player = 0; player < MAX_SLOTS; ++player)
							{
								if (game->getSlot(player)->isOccupied() &&
									game->getSlot(player)->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
								{
									++numPlayers;
								}
							}

							// now get the number of starting spots on the map.
							Int numStartingSpots = MAX_SLOTS;
							const MapMetaData *md = TheMapCache->findMap(game->getMap());
							if (md != NULL)
							{
								numStartingSpots = md->m_numPlayers;
							}

							Int openSlotIndex = -1;
							for (Int i=0; i<MAX_SLOTS; ++i)
							{
								const GameSlot *slot = game->getConstSlot(i);
								if (slot && slot->isOpen())
								{
									openSlotIndex = i;
									break;
								}
							}

							if (openSlotIndex >= 0)
							{
								// add him
								GameSlot newSlot;
								UnicodeString uName;
								uName.translate(p.m_name);
								newSlot.setState(SLOT_PLAYER, uName);
								newSlot.setIP(ntohl(resp.player.IP));
								game->setSlot( openSlotIndex, newSlot );
								game->resetAccepted(); // BGC - need to unaccept everyone if someone joins the game.
							}
							else
							{
								// ditch him
								PeerRequest req;
								req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
								req.UTM.isStagingRoom = TRUE;
								req.id = "KICK/";
								req.nick = p.m_name.str();
								req.options = "GameFull";
								TheGameSpyPeerMessageQueue->addRequest(req);
							}

							// send out new slotlist if I'm host
							TheGameSpyInfo->setGameOptions();
						}
					}
					WOLDisplaySlotList();
				}
				break;

			case PeerResponse::PEERRESPONSE_PLAYERLEFT:
				{
					sawImportantMessage = TRUE;
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->playerLeftGroupRoom(resp.nick.c_str());

					if (TheNGMPGame && TheNGMPGame->isGameInProgress())
					{
						break;
					}

					if (TheNAT == NULL) // don't update slot list if we're trying to start a game
					{

						NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
						if (game && TheGameSpyInfo->amIHost())
						{
							Int idx = game->getSlotNum(resp.nick.c_str());
							if (idx >= 0)
							{
								game->getSlot(idx)->setState(SLOT_OPEN);
								game->resetAccepted(); // BGC - need to unaccept everyone if someone leaves the game.
							}
						}

						// send out new slotlist if I'm host
						TheGameSpyInfo->setGameOptions();
						WOLDisplaySlotList();
						
						if (game && !TheGameSpyInfo->amIHost())
						{
							Int idx = game->getSlotNum(resp.nick.c_str());
							if (idx == 0)
							{
								// host left
								buttonPushed = true;
								game->reset();
								TheGameSpyInfo->leaveStagingRoom();
								//TheGameSpyInfo->joinBestGroupRoom();
								GSMessageBoxOk(TheGameText->fetch("GUI:HostLeftTitle"), TheGameText->fetch("GUI:HostLeft"));
								nextScreen = "Menus/WOLCustomLobby.wnd";
								TheShell->pop();
							}
						}

					}
				}
				break;

			case PeerResponse::PEERRESPONSE_MESSAGE:
				{
					TheGameSpyInfo->addChat(resp.nick.c_str(), resp.message.profileID,
						UnicodeString(resp.text.c_str()), !resp.message.isPrivate, resp.message.isAction, listboxGameSetupChat);
				}
				break;

			case PeerResponse::PEERRESPONSE_DISCONNECT:
				{
					sawImportantMessage = TRUE;
					UnicodeString title, body;
					AsciiString disconMunkee;
					disconMunkee.format("GUI:GSDisconReason%d", resp.discon.reason);
					title = TheGameText->fetch( "GUI:GSErrorTitle" );
					body = TheGameText->fetch( disconMunkee );
					GameSpyCloseAllOverlays();
					GSMessageBoxOk( title, body );
					TheGameSpyInfo->reset();
					TheShell->pop();
				}

			case PeerResponse::PEERRESPONSE_ROOMUTM:
				{
					sawImportantMessage = TRUE;
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
					if (g_debugSlots)
					{
						DEBUG_LOG(("About to process a room UTM.  Command is '%s', command options is '%s'\n",
							resp.command.c_str(), resp.commandOptions.c_str()));
					}
#endif
					if (!strcmp(resp.command.c_str(), "SL"))
					{
						// slotlist
						NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
						Bool isValidSlotList = game && game->getSlot(0) && game->getSlot(0)->isPlayer( resp.nick.c_str() ) && !TheGameSpyInfo->amIHost();
						if (!isValidSlotList)
						{
							SLOTLIST_DEBUG_LOG(("Not a valid slotlist\n"));
							if (!game)
							{
								SLOTLIST_DEBUG_LOG(("No game!\n"));
							}
							else
							{
								if (!game->getSlot(0))
								{
									SLOTLIST_DEBUG_LOG(("No slot 0!\n"));
								}
								else
								{
									if (TheGameSpyInfo->amIHost())
									{
										SLOTLIST_DEBUG_LOG(("I'm the host!\n"));
									}
									else
									{
										SLOTLIST_DEBUG_LOG(("Not from the host!  isHuman:%d, name:'%ls', sender:'%s'\n",
											game->getSlot(0)->isHuman(), game->getSlot(0)->getName().str(),
											resp.nick.c_str()));
									}
								}
							}
						}
						else // isValidSlotList
						{
							Int oldLocalSlotNum = (game->isInGame()) ? game->getLocalSlotNum() : -1;
							Bool wasInGame = oldLocalSlotNum >= 0;
							AsciiString oldMap = game->getMap();
							UnsignedInt oldMapCRC, newMapCRC;
							oldMapCRC = game->getMapCRC();

							AsciiString options = resp.commandOptions.c_str();
							options.trim();
							UnsignedShort ports[MAX_SLOTS];
							UnsignedInt ips[MAX_SLOTS];
							Int i;
							for (i=0; i<MAX_SLOTS; ++i)
							{
								if (game && game->getConstSlot(i))
								{
									ips[i] = game->getConstSlot(i)->getIP();
									ports[i] = game->getConstSlot(i)->getPort();
								}
								else
								{
									ips[i] = 0;
									ports[i] = 0;
								}
							}
							Bool optionsOK = ParseAsciiStringToGameInfo(game, options.str());
							if (TheNAT)
							{
								for (i=0; i<MAX_SLOTS; ++i)
								{
									if (game && game->getSlot(i))
									{
#ifdef DEBUG_LOGGING
										UnsignedShort newPort = game->getConstSlot(i)->getPort();
										UnsignedInt newIP = game->getConstSlot(i)->getIP();
										DEBUG_ASSERTLOG(newIP == ips[i], ("IP was different for player %d (%X --> %X)\n",
											i, ips[i], newIP));
										DEBUG_ASSERTLOG(newPort == ports[i], ("Port was different for player %d (%d --> %d)\n",
											i, ports[i], newPort));
#endif
										game->getSlot(i)->setPort(ports[i]);
										game->getSlot(i)->setIP(ips[i]);
									}
								}
							}
							Int newLocalSlotNum = (game->isInGame()) ? game->getLocalSlotNum() : -1;
							Bool isInGame = newLocalSlotNum >= 0;
							if (!optionsOK)
							{
								SLOTLIST_DEBUG_LOG(("Options are bad!  bailing!\n"));
								break;
							}
							else
							{
								SLOTLIST_DEBUG_LOG(("Options are good, local slot is %d\n", newLocalSlotNum));
								if (!isInGame)
								{
									SLOTLIST_DEBUG_LOG(("Not in game; players are:\n"));
									for (Int i=0; i<MAX_SLOTS; ++i)
									{
										const NGMPGameSlot *slot = game->getGameSpySlot(i);
										if (slot && slot->isHuman())
										{
											UnicodeString munkee;
											munkee.format(L"\t%d: %ls", i, slot->getName().str());
											SLOTLIST_DEBUG_LOG(("%ls\n", munkee.str()));
										}
									}
								}
							}
							WOLDisplaySlotList();

							// if I changed map availability, send it across
							newMapCRC = game->getMapCRC();
							if (isInGame)
							{
								lastSlotlistTime = timeGetTime();
								if ( (oldMapCRC ^ newMapCRC) || (!wasInGame && isInGame) )
								{
									// it changed.  send it
									UnicodeString hostName = game->getSlot(0)->getName();
									AsciiString asciiName;
									asciiName.translate(hostName);
									PeerRequest req;
									req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
									req.UTM.isStagingRoom = TRUE;
									req.id = "MAP";
									req.nick = asciiName.str();
									req.options = (game->getSlot(newLocalSlotNum)->hasMap())?"1":"0";
									TheGameSpyPeerMessageQueue->addRequest(req);
									if (!game->getSlot(newLocalSlotNum)->hasMap())
									{
										UnicodeString text;
										UnicodeString mapDisplayName;
										const MapMetaData *mapData = TheMapCache->findMap( game->getMap() );
										Bool willTransfer = TRUE;
										if (mapData)
										{
											mapDisplayName.format(L"%ls", mapData->m_displayName.str());
											willTransfer = !mapData->m_isOfficial;
										}
										else
										{
											mapDisplayName.format(L"%hs", TheGameState->getMapLeafName(game->getMap()).str());
											willTransfer = WouldMapTransfer(game->getMap());
										}
										if (willTransfer)
											text.format(TheGameText->fetch("GUI:LocalPlayerNoMapWillTransfer"), mapDisplayName.str());
										else
											text.format(TheGameText->fetch("GUI:LocalPlayerNoMap"), mapDisplayName.str());
										GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
									}
								}
								if (!initialAcceptEnable)
								{
									buttonStart->winEnable( TRUE );
									initialAcceptEnable = TRUE;
								}
							}
							else
							{
								if (lastSlotlistTime)
								{
									// can't see ourselves
									buttonPushed = true;
									DEBUG_LOG(("Can't see ourselves in slotlist %s\n", options.str()));
									game->reset();
									TheGameSpyInfo->leaveStagingRoom();
									//TheGameSpyInfo->joinBestGroupRoom();
									GSMessageBoxOk(TheGameText->fetch("GUI:GSErrorTitle"), TheGameText->fetch("GUI:GSKicked"));
									nextScreen = "Menus/WOLCustomLobby.wnd";
									TheShell->pop();
								}
							}
						}
					}
					else if (!strcmp(resp.command.c_str(), "HWS"))
					{
						// host wants to start
						NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
						if (game && game->isInGame() && game->getSlot(0) && game->getSlot(0)->isPlayer(resp.nick.c_str()))
						{
							Int slotNum = game->getLocalSlotNum();
							GameSlot* slot = game->getSlot(slotNum);
							if (slot && (slot->isAccepted() == false))
							{
								GadgetListBoxAddEntryText(listboxGameSetupChat, TheGameText->fetch("GUI:HostWantsToStart"), GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
							}
						}
					}
					else if (!stricmp(resp.command.c_str(), "NAT"))
					{
						if (TheNAT != NULL) {
							TheNAT->processGlobalMessage(-1, resp.commandOptions.c_str());
						}
					}
					else if (!stricmp(resp.command.c_str(), "Pings"))
					{
						if (!TheGameSpyInfo->amIHost())
						{
							AsciiString pings = resp.commandOptions.c_str();
							AsciiString token;
							for (Int i=0; i<MAX_SLOTS; ++i)
							{
								NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
								NGMPGameSlot *slot = game->getGameSpySlot(i);
								if (pings.nextToken(&token, ","))
								{
									token.trim();
									slot->setPingString(token);
								}
								else
								{
									slot->setPingString("");
								}
							}
						}
					}
				}
				break;

			case PeerResponse::PEERRESPONSE_PLAYERUTM:
				{
					sawImportantMessage = TRUE;
					if (!strcmp(resp.command.c_str(), "STATS"))
					{
						PSPlayerStats stats = TheGameSpyPSMessageQueue->parsePlayerKVPairs(resp.commandOptions.c_str());
						if (stats.id && (TheGameSpyPSMessageQueue->findPlayerStatsByID(stats.id).id == 0))
							TheGameSpyPSMessageQueue->trackPlayerStats(stats);
						break;
					}
					NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
					if (game)
					{
						Int slotNum = game->getSlotNum(resp.nick.c_str());
						if ((slotNum >= 0) && (slotNum < MAX_SLOTS) && (!stricmp(resp.command.c_str(), "NAT"))) {
							// this is a command for NAT negotiations, pass if off to TheNAT
							if (TheNAT != NULL) {
								TheNAT->processGlobalMessage(slotNum, resp.commandOptions.c_str());
							}
						}
						if (slotNum == 0 && !TheGameSpyInfo->amIHost())
						{
							if (!strcmp(resp.command.c_str(), "KICK"))
							{
								// oops - we've been kicked.  bail.
								buttonPushed = true;
								game->reset();
								TheGameSpyInfo->leaveStagingRoom();
								//TheGameSpyInfo->joinBestGroupRoom();
								UnicodeString message = TheGameText->fetch("GUI:GSKicked");
								AsciiString commandMessage = resp.commandOptions.c_str();
								commandMessage.trim();
								DEBUG_LOG(("We were kicked: reason was '%s'\n", resp.commandOptions.c_str()));
								if (commandMessage == "GameStarted")
								{
									message = TheGameText->fetch("GUI:GSKickedGameStarted");
								}
								else if (commandMessage == "GameFull")
								{
									message = TheGameText->fetch("GUI:GSKickedGameFull");
								}
								GSMessageBoxOk(TheGameText->fetch("GUI:GSErrorTitle"), message);
								nextScreen = "Menus/WOLCustomLobby.wnd";
								TheShell->pop();
							}
						}
						else if (slotNum > 0 && TheGameSpyInfo->amIHost())
						{
							if (!strcmp(resp.command.c_str(), "accept"))
							{
								game->getSlot(slotNum)->setAccept();
								TheGameSpyInfo->setGameOptions();
								WOLDisplaySlotList();
							}
							else if (!strcmp(resp.command.c_str(), "MAP"))
							{
								Bool hasMap = atoi(resp.commandOptions.c_str());
								game->getSlot(slotNum)->setMapAvailability(hasMap);
								if (!hasMap)
								{
									// tell the host the user doesn't have the map
									UnicodeString mapDisplayName;
									const MapMetaData *mapData = TheMapCache->findMap( game->getMap() );
									Bool willTransfer = TRUE;
									if (mapData)
									{
										mapDisplayName.format(L"%ls", mapData->m_displayName.str());
										willTransfer = !mapData->m_isOfficial;
									}
									else
									{
										mapDisplayName.format(L"%hs", game->getMap().str());
										willTransfer = WouldMapTransfer(game->getMap());
									}
									UnicodeString text;
									if (willTransfer)
										text.format(TheGameText->fetch("GUI:PlayerNoMapWillTransfer"), game->getSlot(slotNum)->getName().str(), mapDisplayName.str());
									else
										text.format(TheGameText->fetch("GUI:PlayerNoMap"), game->getSlot(slotNum)->getName().str(), mapDisplayName.str());
									GadgetListBoxAddEntryText(listboxGameSetupChat, text, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
								}
								WOLDisplaySlotList();
							}
							else if (!strcmp(resp.command.c_str(), "REQ"))
							{
								AsciiString options = resp.commandOptions.c_str();
								options.trim();

								Bool change = false;
								Bool shouldUnaccept = false;
								AsciiString key;
								options.nextToken(&key, "=");
								Int val = atoi(options.str()+1);
								UnsignedInt uVal = atoi(options.str()+1);
								DEBUG_LOG(("GameOpt request: key=%s, val=%s from player %d\n", key.str(), options.str()+1, slotNum));

								NGMPGameSlot *slot = game->getGameSpySlot(slotNum);
								if (!slot)
									break;

								if (key == "Color")
								{
									if (val >= -1 && val < TheMultiplayerSettings->getNumColors() && val != slot->getColor() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
									{
										Bool colorAvailable = TRUE;
										if(val != -1 )
										{
											for(Int i=0; i <MAX_SLOTS; i++)
											{
												GameSlot *checkSlot = game->getSlot(i);
												if(val == checkSlot->getColor() && slot != checkSlot)
												{
													colorAvailable = FALSE;
													break;
												}
											}
										}
										if(colorAvailable)
											slot->setColor(val);
										change = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid color %d\n", val));
									}
								}
								else if (key == "PlayerTemplate")
								{
									if (val >= PLAYERTEMPLATE_MIN && val < ThePlayerTemplateStore->getPlayerTemplateCount() && val != slot->getPlayerTemplate())
									{
                    // Validate for LimitArmies checkbox
                    if ( game->oldFactionsOnly() )
                    {
                      const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(val);
                      if ( fac != NULL && !fac->isOldFaction())
                      {
                        val = PLAYERTEMPLATE_RANDOM;
                      }
                    }

										slot->setPlayerTemplate(val);
										if (val == PLAYERTEMPLATE_OBSERVER)
										{
											slot->setColor(-1);
											slot->setStartPos(-1);
											slot->setTeamNumber(-1);
										}
										change = true;
										shouldUnaccept = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid PlayerTemplate %d\n", val));
									}
								}
								else if (key == "StartPos")
								{
									if (val >= -1 && val < MAX_SLOTS && val != slot->getStartPos() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
									{
										Bool startPosAvailable = TRUE;
										if(val != -1)
										{
											for(Int i=0; i <MAX_SLOTS; i++)
											{
												GameSlot *checkSlot = game->getSlot(i);
												if(val == checkSlot->getStartPos() && slot != checkSlot)
												{
													startPosAvailable = FALSE;
													break;
												}
											}
										}
										if(startPosAvailable)
											slot->setStartPos(val);
										change = true;
										shouldUnaccept = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid startPos %d\n", val));
									}
								}
								else if (key == "Team")
								{
									if (val >= -1 && val < MAX_SLOTS/2 && val != slot->getTeamNumber() && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
									{
										slot->setTeamNumber(val);
										change = true;
										shouldUnaccept = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid team %d\n", val));
									}
								}
								else if (key == "IP")
								{
									if (uVal != slot->getIP())
									{
										DEBUG_LOG(("setting IP of player %ls from 0x%08x to be 0x%08x", slot->getName().str(), slot->getIP(), uVal));
										slot->setIP(uVal);
										change = true;
										shouldUnaccept = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid IP %d\n", uVal));
									}
								}
								else if (key == "NAT")
								{
									if ((val >= FirewallHelperClass::FIREWALL_MIN) &&
											(val <= FirewallHelperClass::FIREWALL_MAX))
									{
										slot->setNATBehavior((FirewallHelperClass::FirewallBehaviorType)val);
										DEBUG_LOG(("Setting NAT behavior to %d for player %d\n", val, slotNum));
										change = true;
									}
									else
									{
										DEBUG_LOG(("Rejecting invalid NAT behavior %d from player %d\n", val, slotNum));
									}
								}
								else if (key == "Ping")
								{
									slot->setPingString(options.str()+1);
									TheGameSpyInfo->setGameOptions();
									DEBUG_LOG(("Setting ping string to %s for player %d\n", options.str()+1, slotNum));
								}

								if (change)
								{
									if (shouldUnaccept)
										game->resetAccepted();

									TheGameSpyInfo->setGameOptions();

									WOLDisplaySlotList();
									DEBUG_LOG(("Slot value is color=%d, PlayerTemplate=%d, startPos=%d, team=%d, IP=0x%8.8X\n",
										slot->getColor(), slot->getPlayerTemplate(), slot->getStartPos(), slot->getTeamNumber(), slot->getIP()));
									DEBUG_LOG(("Slot list updated to %s\n", GameInfoToAsciiString(game).str()));
								}
							}
						}
					}
				}
				break;

			}
		}


	}
}// void WOLGameSetupMenuUpdate( WindowLayout * layout, void *userData)

//-------------------------------------------------------------------------------------------------
/** Lan Game Options menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLGameSetupMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	/*
	switch( msg ) 
	{

		//-------------------------------------------------------------------------------------------------
		case GWM_RIGHT_UP:
		{
			if (buttonPushed)
				break;

			GameWindow *control = (GameWindow *)mData1;
			NameKeyType controlID = (NameKeyType)control->winGetWindowId();
			DEBUG_LOG(("GWM_RIGHT_UP for control %d(%s)\n", controlID, TheNameKeyGenerator->keyToName(controlID).str()));
			break;
		}

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;
			if (buttonPushed)
				break;

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
						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED, 
																							(WindowMsgData)buttonBack, buttonBackID );
					}  // end if
					// don't let key fall through anywhere else
					return MSG_HANDLED;
				}  // end escape
			}  // end switch( key )
		}  // end char
	}  // end switch( msg )
	*/
	return MSG_IGNORED;
}//WindowMsgHandledType WOLGameSetupMenuInput( GameWindow *window, UnsignedInt msg,


// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus(void);
}
extern int isThreadHosting;

Bool handleGameSetupSlashCommands(UnicodeString uText)
{
	AsciiString message;
	message.translate(uText);

	if (message.getCharAt(0) != '/')
	{
		return FALSE; // not a slash command
	}

	AsciiString remainder = message.str() + 1;
	AsciiString token;
	remainder.nextToken(&token);
	token.toLower();

	if (token == "host")
	{
		UnicodeString s;
		s.format(L"Hosting qr2:%d thread:%d", getQR2HostingStatus(), isThreadHosting);
#if !defined(GENERALS_ONLINE)
#else
		GadgetListBoxAddEntryText(listboxGameSetupChat, s, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
#endif
		return TRUE; // was a slash command
	}
	else if (token == "me" && uText.getLength()>4)
	{
		UnicodeString msg = UnicodeString(uText.str() + 4); // skip the /me
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendChatMessageToCurrentLobby(msg, true);
		return TRUE; // was a slash command
	}

#if defined(GENERALS_ONLINE)
	else if (token == "maxcameraheight" && uText.getLength() > 17)
	{
		NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
		if (pOnlineServicesMgr != nullptr)
		{
			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = pOnlineServicesMgr->GetLobbyInterface();

			if (pLobbyInterface != nullptr)
			{
				if (pLobbyInterface->IsInLobby())
				{
					if (pLobbyInterface->IsHost())
					{
						UnicodeString val = UnicodeString(uText.str() + 17); // skip the command
						
						AsciiString asciiVal;
						asciiVal.translate(val);

						bool bIsNumber = true;

						for (int i = 0; i < asciiVal.getLength(); ++i)
						{
							char thisChar = asciiVal.getCharAt(i);
							if (!std::isdigit(thisChar))
							{
								bIsNumber = false;
								break;
							}
						}

						if (bIsNumber)
						{
							int newCameraHeight = atoi(asciiVal.str());

							if (newCameraHeight < GENERALS_ONLINE_MIN_LOBBY_CAMERA_ZOOM || newCameraHeight > GENERALS_ONLINE_MAX_LOBBY_CAMERA_ZOOM)
							{
								UnicodeString msg;
								msg.format(L"The camera height must be between %d and %d.", GENERALS_ONLINE_MIN_LOBBY_CAMERA_ZOOM, GENERALS_ONLINE_MAX_LOBBY_CAMERA_ZOOM);
								GadgetListBoxAddEntryText(listboxGameSetupChat, msg, GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
								return TRUE; // was a slash command
							}
							else
							{
								// save locally
								NGMP_OnlineServicesManager::Settings.Save_Camera_MaxHeight_WhenLobbyHost((float)newCameraHeight);

								// update lobby
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobbyMaxCameraHeight((uint16_t)newCameraHeight);
							}
						}
						else
						{
							GadgetListBoxAddEntryText(listboxGameSetupChat, UnicodeString(L"The camera height must be a number."), GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
							return TRUE; // was a slash command
						}
						
					}
					else
					{
						GadgetListBoxAddEntryText(listboxGameSetupChat, UnicodeString(L"You must be the lobby host to modify the maximum camera height."), GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
						return TRUE; // was a slash command
					}
				}
			}
		}

		return TRUE; // was a slash command
	}
	else if (token == "leave")
	{
		PopBackToLobby();
	}
	else if (token == "quit")
	{
		exit(0);
	}
	else if (token == "connections" || token == "conns" || token == "c")
	{
		// TODO_NGMP: Disable this on retail builds
		std::map<int64_t, PlayerConnection>& connections = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh()->GetAllConnections();

		UnicodeString msg;
		msg.format(L"Dumping %d network connection(s) to this lobby:", connections.size());
		GadgetListBoxAddEntryText(listboxGameSetupChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);

		int64_t localUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();

		auto lobbyMembers = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();

		int i = 0;
		for (auto& kvPair : connections)
		{
			PlayerConnection& conn = kvPair.second;
			std::string strIPAddr = conn.GetIPAddrString();

			std::string strState = "Unknown";

			switch (conn.m_State)
			{
			case EConnectionState::NOT_CONNECTED:
				strState = "Not Connected";
				break;

			case EConnectionState::CONNECTING_DIRECT:
				strState = "Connecting (Direct)";
				break;
			case EConnectionState::CONNECTING_RELAY:
				strState = "Connecting (Relay)";
				break;

			case EConnectionState::CONNECTED_DIRECT:
				strState = "Connected (Direct)";
				break;

			case EConnectionState::CONNECTED_RELAY:
				strState = "Connected (Relay)";
				break;


			case EConnectionState::CONNECTION_FAILED:
				strState = "Connection Failed";
				break;

			default:
				strState = "Unknown";
				break;
			}

			std::string strDisplayName = "Unknown";
			for (auto& member : lobbyMembers)
			{
				if (member.user_id == kvPair.first)
				{
					strDisplayName = member.display_name;
					break;
				}
			}

			std::string strConnectionType = "Unknown";
			if (localUserID == kvPair.first)
			{
				strConnectionType = "Local";
			}
			else
			{
				strConnectionType = "Remote";
			}

			UnicodeString msg;
			msg.format(L"        Connection %d (%hs) - user %lld - name %hs - addr %hs:%d - State: %hs - Attempts: %d - Latency: %d",
				i, strConnectionType.c_str(), kvPair.first, strDisplayName.c_str(), strIPAddr.c_str(), conn.m_address.port, strState.c_str(), conn.m_ConnectionAttempts, conn.latency);
			GadgetListBoxAddEntryText(listboxGameSetupChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);

			++i;
		}

		return TRUE; // was a slash command
	}
#endif

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
	else if (token == "slots")
	{
		g_debugSlots = !g_debugSlots;
#if !defined(GENERALS_ONLINE)
#else
		GadgetListBoxAddEntryText(listboxGameSetupChat, UnicodeString(L"Toggled SlotList debug"), GameSpyColor[GSCOLOR_DEFAULT], -1, -1);
#endif
		return TRUE; // was a slash command
	}
	else if (token == "discon")
	{
		PeerRequest req;
		req.peerRequestType = PeerRequest::PEERREQUEST_LOGOUT;
		TheGameSpyPeerMessageQueue->addRequest( req );
		return TRUE;
	}

#endif // defined(RTS_DEBUG) || defined(RTS_INTERNAL)

	return FALSE; // not a slash command
}

static Int getNextSelectablePlayer(Int start)
{
#if !defined(GENERALS_ONLINE)
#else
	NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
#endif
	if (!game->amIHost())
		return -1;
	for (Int j=start; j<MAX_SLOTS; ++j)
	{
#if !defined(GENERALS_ONLINE)
#else
		NGMPGameSlot *slot = game->getGameSpySlot(j);
#endif

		if (slot && slot->getStartPos() == -1 &&
			( (j==game->getLocalSlotNum() && game->getConstSlot(j)->getPlayerTemplate()!=PLAYERTEMPLATE_OBSERVER)
			|| slot->isAI()))
		{
			return j;
		}
	}
	return -1;
}

static Int getFirstSelectablePlayer(const GameInfo *game)
{
	const GameSlot *slot = game->getConstSlot(game->getLocalSlotNum());
	if (!game->amIHost() || slot && slot->getPlayerTemplate() != PLAYERTEMPLATE_OBSERVER)
		return game->getLocalSlotNum();

	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		slot = game->getConstSlot(i);
		if (slot && slot->isAI())
			return i;
	}

	return game->getLocalSlotNum();
}

//-------------------------------------------------------------------------------------------------
/** WOL Game Options menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLGameSetupMenuSystem( GameWindow *window, UnsignedInt msg, 
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;

	static int buttonCommunicatorID = NAMEKEY_INVALID;
	switch( msg )
	{
		//-------------------------------------------------------------------------------------------------	
		case GWM_CREATE:
			{
				buttonCommunicatorID = NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator");
				break;
			} // case GWM_DESTROY:
		//-------------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				if (windowMap)
					windowMap->winSetUserData(NULL);

				break;
			} // case GWM_DESTROY:
		//-------------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{	
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}//case GWM_INPUT_FOCUS:
		//-------------------------------------------------------------------------------------------------
		case GCM_SELECTED:
		{
			if (!initDone)
				break;
			if (buttonPushed)
				break;
			GameWindow* control = (GameWindow*)mData1;
			Int controlID = control->winGetWindowId();
			NGMPGame* myGame = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();

			if (controlID == comboBoxStartingCashID)
			{
				handleStartingCashSelection();
			}
			else
			{
				for (Int i = 0; i < MAX_SLOTS; i++)
				{
					if (controlID == comboBoxColorID[i])
					{
						handleColorSelection(i);
					}
					else if (controlID == comboBoxPlayerTemplateID[i])
					{
						handlePlayerTemplateSelection(i);
					}
					else if (controlID == comboBoxTeamID[i])
					{
						handleTeamSelection(i);
					}
					else if (controlID == comboBoxPlayerID[i] && NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost())
					{
						// We don't have anything that'll happen if we click on ourselves
						if (i == myGame->getLocalSlotNum())
							break;
						// Get
						Int pos = -1;
						GadgetComboBoxGetSelectedPos(comboBoxPlayer[i], &pos);
						if (pos != SLOT_PLAYER && pos >= 0)
						{
							if (myGame->getSlot(i)->getState() == SLOT_PLAYER)
							{
								// TODO_NGMP: Support kick again
								/*
								PeerRequest req;
								req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
								req.UTM.isStagingRoom = TRUE;
								AsciiString aName;
								aName.translate(myGame->getSlot(i)->getName());
								req.nick = aName.str();
								req.id = "KICK/";
								req.options = "true";
								TheGameSpyPeerMessageQueue->addRequest(req);
								*/

								UnicodeString name = myGame->getSlot(i)->getName();

								// NOTE: No host check here, service enforces it
								NGMPGameSlot* pSlot = (NGMPGameSlot*)myGame->getSlot(i);
								int64_t userBeingKicked = pSlot->m_userID;
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_KickUser(userBeingKicked, name);

								myGame->getSlot(i)->setState(SlotState(pos));
								myGame->resetAccepted();

								// // TODO_NGMP
								//TheGameSpyInfo->setGameOptions();
								WOLDisplaySlotList();
								//TheLAN->OnPlayerLeave(name);
							}
							else if (myGame->getSlot(i)->getState() != pos)
							{
								Bool wasAI = (myGame->getSlot(i)->isAI());
								myGame->getSlot(i)->setState(SlotState(pos));
								Bool isAI = (myGame->getSlot(i)->isAI());
								myGame->resetAccepted();
								if (wasAI ^ isAI)
									PopulatePlayerTemplateComboBox(i, comboBoxPlayerTemplate, myGame, wasAI && myGame->getAllowObservers());

								// inform service
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateCurrentLobby_SetSlotState(i, pos);

								WOLDisplaySlotList();
							}
						}
						break;
					}
				}
			}
			}// case GCM_SELECTED:
		//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				static int buttonCommunicatorID = NAMEKEY("GameSpyGameOptionsMenu.wnd:ButtonCommunicator");

				if ( controlID == buttonBackID )
				{
					savePlayerInfo();
					if( WOLMapSelectLayout )
					{
						WOLMapSelectLayout->destroyWindows();
						deleteInstance(WOLMapSelectLayout);
						WOLMapSelectLayout = NULL;
					}

					
					PopBackToLobby();

				} //if ( controlID == buttonBack )
				else if ( controlID == buttonCommunicatorID )
				{
					GameSpyToggleOverlay( GSOVERLAY_BUDDY );

				}
				else if ( controlID == buttonEmoteID )
				{
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty())
					{
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendChatMessageToCurrentLobby(txtInput, false);
					}
				} //if ( controlID == buttonEmote )
				else if ( controlID == buttonSelectMapID )
				{
					WOLMapSelectLayout = TheWindowManager->winCreateLayout( "Menus/WOLMapSelectMenu.wnd" );
					WOLMapSelectLayout->runInit();
					WOLMapSelectLayout->hide( FALSE );
					WOLMapSelectLayout->bringForward();
				}
				else if ( controlID == buttonStartID )
				{
					savePlayerInfo();
					
					bool bIsHost = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost();
					if (bIsHost)
					{
						StartPressed();
					}
					else
					{
						//I'm the Client... send an accept message to the host.
						NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
						GameSlot *localSlot = game->getSlot(game->getLocalSlotNum());
						if (localSlot)
						{
							localSlot->setAccept();
						}

						// force a refresh of our local lobby properties to sync to remote players
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();

						/*
						UnicodeString hostName = game->getSlot(0)->getName();
						AsciiString asciiName;
						asciiName.translate(hostName);
						PeerRequest req;
						req.peerRequestType = PeerRequest::PEERREQUEST_UTMPLAYER;
						req.UTM.isStagingRoom = TRUE;
						req.id = "accept";
						req.nick = asciiName.str();
						req.options = "true";
						TheGameSpyPeerMessageQueue->addRequest(req);
						//peerSetReady( PEER, PEERTrue );
						WOLDisplaySlotList();
						*/
					}
				}
        else if ( controlID == checkBoxLimitSuperweaponsID )
        {
          handleLimitSuperweaponsClick();
        }
				else
				{
					for (Int i = 0; i < MAX_SLOTS; i++)
					{
						if (controlID == buttonMapStartPositionID[i])
						{
							NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
							Int playerIdxInPos = -1;
							for (Int j=0; j<MAX_SLOTS; ++j)
							{
								NGMPGameSlot *slot = game->getGameSpySlot(j);
								if (slot && slot->getStartPos() == i)
								{
									playerIdxInPos = j;
									break;
								}
							}
							if (playerIdxInPos >= 0)
							{
								NGMPGameSlot *slot = game->getGameSpySlot(playerIdxInPos);
								if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
								{
									// it's one of my type.  Try to change it.
									Int nextPlayer = getNextSelectablePlayer(playerIdxInPos+1);
									handleStartPositionSelection(playerIdxInPos, -1);
									if (nextPlayer >= 0)
									{
										handleStartPositionSelection(nextPlayer, i);
									}
								}
							}
							else
							{
								// nobody in the slot - put us in
								Int nextPlayer = getNextSelectablePlayer(0);
								if (nextPlayer < 0)
									nextPlayer = getFirstSelectablePlayer(game);
								handleStartPositionSelection(nextPlayer, i);
							}
						}
					}
				}


				break;
			}// case GBM_SELECTED:
		//-------------------------------------------------------------------------------------------------
		case GBM_SELECTED_RIGHT:
   		{
   			if (buttonPushed)
   				break;
   
   			GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				for (Int i = 0; i < MAX_SLOTS; i++)
				{
					if (controlID == buttonMapStartPositionID[i])
					{
						NGMPGame* game = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame();
						Int playerIdxInPos = -1;
						for (Int j=0; j<MAX_SLOTS; ++j)
						{
							NGMPGameSlot *slot = game->getGameSpySlot(j);
							if (slot && slot->getStartPos() == i)
							{
								playerIdxInPos = j;
								break;
							}
						}
						if (playerIdxInPos >= 0)
						{
							NGMPGameSlot *slot = game->getGameSpySlot(playerIdxInPos);
							if (playerIdxInPos == game->getLocalSlotNum() || (game->amIHost() && slot && slot->isAI()))
							{
								// it's one of my type.  Remove it.
								handleStartPositionSelection(playerIdxInPos, -1);
							}
						}
					}
				}
				break;
			}

		//-------------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				// Take the user's input and echo it into the chat window as well as
				// send it to the other clients on the lan
				if ( controlID == textEntryChatID )
				{
					
					// read the user's input
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					// Clear the text entry line
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					// Clean up the text (remove leading/trailing chars, etc)
					txtInput.trim();
					// Echo the user's input to the chat window
					if (!txtInput.isEmpty())
					{
						if (!handleGameSetupSlashCommands(txtInput))
						{
							NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SendChatMessageToCurrentLobby(txtInput, false);
						}
					}

				}// if ( controlID == textEntryChatID )
				break;
			}
		//-------------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;
	}//Switch
	return MSG_HANDLED;
}//WindowMsgHandledType WOLGameSetupMenuSystem( GameWindow *window, UnsignedInt msg, 


void OnKickedFromLobby()
{
	// can't see ourselves
	buttonPushed = true;
	TheNGMPGame->reset();
	GSMessageBoxOk(TheGameText->fetch("GUI:GSErrorTitle"), TheGameText->fetch("GUI:GSKicked"));
	nextScreen = "Menus/WOLCustomLobby.wnd";
	TheShell->pop();
}