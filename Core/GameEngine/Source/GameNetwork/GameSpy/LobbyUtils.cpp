/*
**	Command & Conquer Generals(tm)
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
// FILE: LobbyUtils.cpp
// Author: Matthew D. Campbell, Sept 2002
// Description: GameSpy lobby utils
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameEngine.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/Version.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/Image.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameText.h"
#include "GameClient/MapUtil.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Mouse.h"
#include "GameNetwork/GameSpyOverlay.h"

#include "GameClient/LanguageFilter.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/LadderDefs.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/GSConfig.h"

#include "Common/STLTypedefs.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"


// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
// Note: if you add more columns, you must modify the .wnd files and change the listbox properties (yuck!)

// TODO_NGMP: We probably need to modify this in the WND... they set the width for observer to 0, for now we dont show ping, but we should show box
#if defined(GENERALS_ONLINE)
enum {
	COLUMN_NAME = 0,
	COLUMN_MAP,
	COLUMN_LADDER,
	COLUMN_NUMPLAYERS,
	COLUMN_PASSWORD,
	COLUMN_OBSERVER,
  COLUMN_USE_STATS,
  COLUMN_PING
};
#else
enum {
	COLUMN_NAME = 0,
	COLUMN_MAP,
	COLUMN_LADDER,
	COLUMN_NUMPLAYERS,
	COLUMN_PASSWORD,
	COLUMN_OBSERVER,
#if !RTS_GENERALS
  COLUMN_USE_STATS,
#endif
	COLUMN_PING,
};
#endif

// ---------------------------------------------------------------------------
// row entry animation
// ---------------------------------------------------------------------------

static std::map<int, GameRowAnim> g_gameListAnim;

// initialize the smoothed index for this lobbyID
static float UpdateAndGetGameRowIndex(int lobbyID, float logicalIndex)
{
    GameRowAnim& anim = g_gameListAnim[lobbyID];

    if (!anim.alive)
    {
        // start slightly below, then glide into place
        anim.currentIndex = logicalIndex + 0.3f;
        anim.targetIndex  = logicalIndex;
        anim.alive        = true;
    }
    else
    {
        anim.targetIndex = logicalIndex;
    }

    // how fast to snap into place
    const float t = 0.1f; 

    float delta = anim.targetIndex - anim.currentIndex;
    anim.currentIndex += delta * t;

    return anim.currentIndex;
}

// Clears animation state for IDs that no longer exist
static void CleanupMissingGameRows(const std::vector<LobbyEntry>& lobbies)
{
	std::map<int, bool> stillPresent;

	for (const LobbyEntry& lobby : lobbies)
	{
		stillPresent[lobby.lobbyID] = true;
	}

	for (auto it = g_gameListAnim.begin(); it != g_gameListAnim.end(); )
	{
		if (stillPresent.find(it->first) == stillPresent.end())
			it = g_gameListAnim.erase(it);
		else
			++it;
	}
}

static NameKeyType buttonSortAlphaID = NAMEKEY_INVALID;
static NameKeyType buttonSortPingID = NAMEKEY_INVALID;
static NameKeyType buttonSortBuddiesID = NAMEKEY_INVALID;
static NameKeyType windowSortAlphaID = NAMEKEY_INVALID;
static NameKeyType windowSortPingID = NAMEKEY_INVALID;
static NameKeyType windowSortBuddiesID = NAMEKEY_INVALID;

static GameWindow *buttonSortAlpha = nullptr;
static GameWindow *buttonSortPing = nullptr;
static GameWindow *buttonSortBuddies = nullptr;
static GameWindow *windowSortAlpha = nullptr;
static GameWindow *windowSortPing = nullptr;
static GameWindow *windowSortBuddies = nullptr;

static GameSortType theGameSortType = GAMESORT_MAP_ASCENDING; // was ping
static Bool sortBuddies = TRUE;
static void showSortIcons()
{
	if (windowSortAlpha && windowSortPing)
	{
		switch (theGameSortType)
		{
		case GAMESORT_AGE_ASCENDING: // was alpha
			windowSortAlpha->winHide(FALSE);
			windowSortAlpha->winEnable(TRUE);
			windowSortPing->winHide(TRUE);
			break;
		case GAMESORT_AGE_DESCENDING: // was alpha
			windowSortAlpha->winHide(FALSE);
			windowSortAlpha->winEnable(FALSE);
			windowSortPing->winHide(TRUE);
			break;
		case GAMESORT_MAP_ASCENDING: // was ping
			windowSortPing->winHide(FALSE);
			windowSortPing->winEnable(TRUE);
			windowSortAlpha->winHide(TRUE);
			break;
		case GAMESORT_MAP_DESCENDING: // was ping
			windowSortPing->winHide(FALSE);
			windowSortPing->winEnable(FALSE);
			windowSortAlpha->winHide(TRUE);
			break;
		}
	}

	if (sortBuddies)
	{
		if (windowSortBuddies)
		{
			windowSortBuddies->winHide(FALSE);
		}
	}
	else
	{
		if (windowSortBuddies)
		{
			windowSortBuddies->winHide(TRUE);
		}
	}
}
void setSortMode(GameSortType sortType) { theGameSortType = sortType; showSortIcons(); RefreshGameListBoxes(); }
void sortByBuddies(Bool doSort) { sortBuddies = doSort; showSortIcons(); RefreshGameListBoxes(); }

Bool HandleSortButton(NameKeyType sortButton)
{
	if (sortButton == buttonSortBuddiesID)
	{
		sortByBuddies(!sortBuddies);
		return TRUE;
	}
	else if (sortButton == buttonSortAlphaID)
	{
		if (theGameSortType == GAMESORT_AGE_ASCENDING) // was alpha
		{
			setSortMode(GAMESORT_AGE_DESCENDING); // was alpha
		}
		else
		{
			setSortMode(GAMESORT_AGE_ASCENDING); // was alpha
		}
		return TRUE;
	}
	else if (sortButton == buttonSortPingID)
	{
		if (theGameSortType == GAMESORT_MAP_ASCENDING) // was ping
		{
			setSortMode(GAMESORT_MAP_DESCENDING); // was ping
		}
		else
		{
			setSortMode(GAMESORT_MAP_ASCENDING); // was ping
		}
		return TRUE;
	}
	return FALSE;
}

// window ids ------------------------------------------------------------------------------
static NameKeyType parentID = NAMEKEY_INVALID;
//static NameKeyType parentGameListSmallID = NAMEKEY_INVALID;
static NameKeyType parentGameListLargeID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyGamesSmallID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyGamesLargeID = NAMEKEY_INVALID;
//static NameKeyType listboxLobbyGameInfoID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parent = nullptr;
//static GameWindow *parentGameListSmall = nullptr;
static GameWindow *parentGameListLarge = nullptr;
       //GameWindow *listboxLobbyGamesSmall = nullptr;
       GameWindow *listboxLobbyGamesLarge = nullptr;
       //GameWindow *listboxLobbyGameInfo = nullptr;

static const Image *pingImages[3] = { nullptr, nullptr, nullptr };

static void gameTooltip(GameWindow* window,
	WinInstanceData* instData,
	UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);//TheGameText->fetch("TOOLTIP:GamesBeingFormed") );
		return;
	}

	Int gameID = (Int)GadgetListBoxGetItemData(window, row, 0);
#if defined(GENERALS_ONLINE)
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		return;
	}

	LobbyEntry lobbyEntry = pLobbyInterface->GetLobbyFromID(gameID);
	if (lobbyEntry.lobbyID == -1)
	{
		return;
	}
#else
	GameSpyStagingRoom *room = TheGameSpyInfo->findStagingRoomByID(gameID);
	if (!room)
	{
		TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UnknownGame") );
		return;
	}
#endif

	if (col == COLUMN_PING)
	{
#if 0 //def DEBUG_LOGGING
		UnicodeString s;
		s.format(L"Ping is %d ms (cutoffs are %d ms and %d ms\n%hs local pings\n%hs remote pings",
			room->getPingAsInt(), TheGameSpyConfig->getPingCutoffGood(), TheGameSpyConfig->getPingCutoffBad(),
			TheGameSpyInfo->getPingString().str(), room->getPingString().str()
		);
		TheMouse->setCursorTooltip( s, 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
#else
		TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:PingInfo"), 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
#endif
		return;
	}
	if (col == COLUMN_NUMPLAYERS)
	{
		TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:NumberOfPlayers"), 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
		return;
	}
	if (col == COLUMN_PASSWORD)
	{
#if defined(GENERALS_ONLINE)
		if (lobbyEntry.passworded)
#else
		if (room->getHasPassword())
#endif
		{
			UnicodeString checkTooltip =TheGameText->fetch("TOOTIP:Password");
			if(!checkTooltip.compare(L"Password required to joing game"))
				checkTooltip.set(L"Password required to join game");
			TheMouse->setCursorTooltip( checkTooltip, 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
		}
		else
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString );
		return;
	}
#if !RTS_GENERALS
  if (col == COLUMN_USE_STATS)
  {
#if defined(GENERALS_ONLINE)
	  if (lobbyEntry.track_stats)
#else
	  if (room->getUseStats())
#endif
    {
      TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UseStatsOn") );
    }
    else
    {
      TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:UseStatsOff") );
    }
    return;
  }
#endif

	UnicodeString tooltip;

	UnicodeString mapName;

#if defined(GENERALS_ONLINE)
	// GO already has the full map info, don't need the cache
	mapName.translate(lobbyEntry.map_name.c_str());
#else
	const MapMetaData *md = TheMapCache->findMap(room->getMap());
	if (md)
	{
		mapName = md->m_displayName;
	}
	else
	{
		const char *start = room->getMap().reverseFind('\\');
		if (start)
		{
			++start;
		}
		else
		{
			start = room->getMap().str();
		}
		mapName.translate( start );
	}
#endif
	UnicodeString tmp;

#if defined(GENERALS_ONLINE)
	UnicodeString gameName;
	gameName.format(L"%s", from_utf8(lobbyEntry.name).c_str());
	tooltip.format(TheGameText->fetch("TOOLTIP:GameInfoGameName"), gameName.str());

	UnicodeString region;
	region.format(L"\n\nRegion: %s", from_utf8(lobbyEntry.region).c_str());
	tooltip.concat(region);

	UnicodeString latency;
	latency.format(L"\nLatency: %d (%d frames)\n", lobbyEntry.latency, ConvertMSLatencyToGenToolFrames(lobbyEntry.latency));
	tooltip.concat(latency);
#else
	tooltip.format(TheGameText->fetch("TOOLTIP:GameInfoGameName"), room->getGameName().str());
#endif
	
#if defined(GENERALS_ONLINE)
	// TODO_QUICKMATCH
	if (false)
#else
	if (room->getLadderPort() != 0)

	{
		const LadderInfo *linfo = TheLadderList->findLadder(room->getLadderIP(), room->getLadderPort());
		if (linfo)
		{
			tmp.format(TheGameText->fetch("TOOLTIP:GameInfoLadderName"), linfo->name.str());
			tooltip.concat(tmp);
		}
	}
#endif
#if defined(GENERALS_ONLINE)
	if (lobbyEntry.exe_crc != TheGlobalData->m_exeCRC || lobbyEntry.ini_crc != TheGlobalData->m_iniCRC)
#else
	if (room->getExeCRC() != TheGlobalData->m_exeCRC || room->getIniCRC() != TheGlobalData->m_iniCRC)
#endif
	{
		tmp.format(TheGameText->fetch("TOOLTIP:InvalidGameVersion"), mapName.str());
		tooltip.concat(tmp);
	}
	tmp.format(TheGameText->fetch("TOOLTIP:GameInfoMap"), mapName.str());
	tooltip.concat(tmp);
	tooltip.concat(L"\n");

#if defined(GENERALS_ONLINE)
	for (LobbyMemberEntry& member : lobbyEntry.members)
	{
		if (member.IsHuman())
		{
			UnicodeString plrName;
			plrName.format(L"%s [%hs, %dms latency]", from_utf8(member.display_name).c_str(), member.region.c_str(), member.latency);

			// TODO_NGMP: We don't have stats info
			//tmp.format(TheGameText->fetch("TOOLTIP:GameInfoPlayer"), plrName.str(), slot->getWins(), slot->getLosses());
			tooltip.concat(L'\n');
			tooltip.concat(plrName);
		}
		else
		{
			switch (member.m_SlotState)
			{
			case SLOT_EASY_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:EasyAI"));
				break;
			case SLOT_MED_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:MediumAI"));
				break;
			case SLOT_BRUTAL_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:HardAI"));
				break;
			}
		}
	}
#else
	AsciiString aPlayer;
	UnicodeString player;
	Int numPlayers = 0;
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		GameSpyGameSlot *slot = room->getGameSpySlot(i);
		if (i == 0 && (!slot || !slot->isHuman()))
		{
			DEBUG_CRASH(("About to tooltip a non-hosted game!"));
		}
		if (slot && slot->isHuman())
		{
			tmp.format(TheGameText->fetch("TOOLTIP:GameInfoPlayer"), slot->getName().str(), slot->getWins(), slot->getLosses());
			tooltip.concat(tmp);
			++numPlayers;
		}
		else if (slot && slot->isAI())
		{
			++numPlayers;
			switch(slot->getState())
			{
			case SLOT_EASY_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:EasyAI"));
				break;
			case SLOT_MED_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:MediumAI"));
				break;
			case SLOT_BRUTAL_AI:
				tooltip.concat(L'\n');
				tooltip.concat(TheGameText->fetch("GUI:HardAI"));
				break;
			}
		}
	}
	DEBUG_ASSERTCRASH(numPlayers, ("Tooltipping a 0-player game!"));
#endif

	TheMouse->setCursorTooltip( tooltip, 10, nullptr, 2.0f ); // the text and width are the only params used.  the others are the default values.
}

static Bool isSmall = TRUE;

GameWindow *GetGameListBox()
{
	return listboxLobbyGamesLarge;
}

GameWindow *GetGameInfoListBox()
{
	return nullptr;
}

NameKeyType GetGameListBoxID()
{
	return listboxLobbyGamesLargeID;
}

NameKeyType GetGameInfoListBoxID()
{
	return NAMEKEY_INVALID;
}

void GrabWindowInfo()
{
	isSmall = TRUE;
	parentID = NAMEKEY( "WOLCustomLobby.wnd:WOLLobbyMenuParent" );
	parent = TheWindowManager->winGetWindowFromId(nullptr, parentID);

	pingImages[0] = TheMappedImageCollection->findImageByName("Ping03");
	pingImages[1] = TheMappedImageCollection->findImageByName("Ping02");
	pingImages[2] = TheMappedImageCollection->findImageByName("Ping01");
	DEBUG_ASSERTCRASH(pingImages[0], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[1], ("Can't find ping image!"));
	DEBUG_ASSERTCRASH(pingImages[2], ("Can't find ping image!"));

//	parentGameListSmallID = NAMEKEY( "WOLCustomLobby.wnd:ParentGameListSmall" );
//	parentGameListSmall = TheWindowManager->winGetWindowFromId(nullptr, parentGameListSmallID);

	parentGameListLargeID = NAMEKEY( "WOLCustomLobby.wnd:ParentGameListLarge" );
	parentGameListLarge = TheWindowManager->winGetWindowFromId(nullptr, parentGameListLargeID);

	listboxLobbyGamesSmallID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGames" );
//	listboxLobbyGamesSmall = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGamesSmallID);
//	listboxLobbyGamesSmall->winSetTooltipFunc(gameTooltip);

	listboxLobbyGamesLargeID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGamesLarge" );
	listboxLobbyGamesLarge = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGamesLargeID);
	listboxLobbyGamesLarge->winSetTooltipFunc(gameTooltip);
//
//	listboxLobbyGameInfoID = NAMEKEY( "WOLCustomLobby.wnd:ListboxGameInfo" );
//	listboxLobbyGameInfo = TheWindowManager->winGetWindowFromId(nullptr, listboxLobbyGameInfoID);

	buttonSortAlphaID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortAlpha");
	buttonSortPingID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortPing");
	buttonSortBuddiesID = NAMEKEY("WOLCustomLobby.wnd:ButtonSortBuddies");
	windowSortAlphaID = NAMEKEY("WOLCustomLobby.wnd:WindowSortAlpha");
	windowSortPingID = NAMEKEY("WOLCustomLobby.wnd:WindowSortPing");
	windowSortBuddiesID = NAMEKEY("WOLCustomLobby.wnd:WindowSortBuddies");

	buttonSortAlpha = TheWindowManager->winGetWindowFromId(parent, buttonSortAlphaID);
	if (buttonSortAlpha)
	{
		buttonSortAlpha->winSetText(UnicodeString(L"Sort by Newest"));
		buttonSortAlpha->winSetTooltip(UnicodeString::TheEmptyString);
	}
	buttonSortPing = TheWindowManager->winGetWindowFromId(parent, buttonSortPingID);
	if (buttonSortPing)
	{
		buttonSortPing->winSetText(UnicodeString(L"Sort by Map"));
		buttonSortPing->winSetTooltip(UnicodeString::TheEmptyString);
	}
	buttonSortBuddies = TheWindowManager->winGetWindowFromId(parent, buttonSortBuddiesID);
	if (buttonSortBuddies)
	{
		buttonSortBuddies->winSetText(UnicodeString(L"Sort by Buddies"));
		buttonSortBuddies->winSetTooltip(UnicodeString::TheEmptyString);
	}

	windowSortAlpha = TheWindowManager->winGetWindowFromId(parent, windowSortAlphaID);
	windowSortPing = TheWindowManager->winGetWindowFromId(parent, windowSortPingID);
	windowSortBuddies = TheWindowManager->winGetWindowFromId(parent, windowSortBuddiesID);

	showSortIcons();
}

void ReleaseWindowInfo()
{
	isSmall = TRUE;
	parent = nullptr;
//	parentGameListSmall = nullptr;
	parentGameListLarge = nullptr;
//	listboxLobbyGamesSmall = nullptr;
	listboxLobbyGamesLarge = nullptr;
//	listboxLobbyGameInfo = nullptr;

	buttonSortAlpha = nullptr;
	buttonSortPing = nullptr;
	buttonSortBuddies = nullptr;
	windowSortAlpha = nullptr;
	windowSortPing = nullptr;
	windowSortBuddies = nullptr;
}

#if defined(GENERALS_ONLINE)
typedef std::set<int64_t> BuddyGameSet;
#else
typedef std::set<GameSpyStagingRoom*> BuddyGameSet;
#endif

static BuddyGameSet *theBuddyGames = nullptr;
#if defined(GENERALS_ONLINE)
static void populateBuddyGames(std::vector<LobbyEntry>& vecLobbies)
#else
static void populateBuddyGames(void)
#endif
{
#if defined(GENERALS_ONLINE)
	theBuddyGames = NEW BuddyGameSet;

	NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pSocialInterface == nullptr)
	{
		return;
	}

	for (LobbyEntry& lobby : vecLobbies)
	{
		// is host our friend?
		if (pSocialInterface->IsUserFriend(lobby.owner))
		{
			theBuddyGames->insert(lobby.lobbyID);
		}
		else // does the lobby contain any of our friends
		{
			for (auto member : lobby.members)
			{
				if (pSocialInterface->IsUserFriend(member.user_id))
				{
					theBuddyGames->insert(lobby.lobbyID);
					break; // its binary, we don't care how many friends
				}
			}
		}
	}
#else
	BuddyInfoMap *m = TheGameSpyInfo->getBuddyMap();
	theBuddyGames = NEW BuddyGameSet;
	if (!m)
	{
		return;
	}
	for (BuddyInfoMap::const_iterator bit = m->begin(); bit != m->end(); ++bit)
	{
		BuddyInfo info = bit->second;
		if (info.m_status == GP_STAGING)
		{
			StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
			for (StagingRoomMap::iterator srmIt = srm->begin(); srmIt != srm->end(); ++srmIt)
			{
				GameSpyStagingRoom *game = srmIt->second;
				game->cleanUpSlotPointers();
				const GameSpyGameSlot *slot = game->getGameSpySlot(0);
				if (slot && slot->getName() == info.m_locationString)
				{
					theBuddyGames->insert(game);
					break;
				}
			}
		}
	}
#endif
}

static void clearBuddyGames()
{
	delete theBuddyGames;
	theBuddyGames = nullptr;
}

#if defined(GENERALS_ONLINE)
struct GameSortStruct
{
	bool operator()(const LobbyEntry& g1, const LobbyEntry& g2) const
	{
		const bool g1Join = !(g1.exe_crc != TheGlobalData->m_exeCRC || g1.ini_crc != TheGlobalData->m_iniCRC);
		const bool g2Join = !(g2.exe_crc != TheGlobalData->m_exeCRC || g2.ini_crc != TheGlobalData->m_iniCRC);

		if (g1Join != g2Join)
			return g1Join && !g2Join;

		if (sortBuddies)
		{
			const bool g1Buddy = (theBuddyGames && theBuddyGames->count(g1.lobbyID));
			const bool g2Buddy = (theBuddyGames && theBuddyGames->count(g2.lobbyID));

			if (g1Buddy != g2Buddy)
				return g1Buddy && !g2Buddy;
		}

		// Push passworded lobbies below open ones
		if (g1.passworded != g2.passworded)
			return !g1.passworded && g2.passworded;

        // NOTE: GO currently does not have private ladders, so this check is moot
		/*
		// sort games with private ladders to the bottom
		Bool g1UnknownLadder = (g1->getLadderPort() && TheLadderList->findLadder(g1->getLadderIP(), g1->getLadderPort()) == NULL);
		Bool g2UnknownLadder = (g2->getLadderPort() && TheLadderList->findLadder(g2->getLadderIP(), g2->getLadderPort()) == NULL);
		if (g1UnknownLadder ^ g2UnknownLadder)
		{
			return g2UnknownLadder;
		}
		*/

		switch (theGameSortType)
		{
		case GAMESORT_MAP_ASCENDING: // was ping
			if (g1.map_name != g2.map_name)
				return g1.map_name < g2.map_name;
			break;

		case GAMESORT_MAP_DESCENDING: // was ping
			if (g1.map_name != g2.map_name)
				return g1.map_name > g2.map_name;
			break;

		case GAMESORT_AGE_ASCENDING:  // was alpha
			return g1.lobbyID > g2.lobbyID;
		case GAMESORT_AGE_DESCENDING: // was alpha
			return g1.lobbyID < g2.lobbyID;
		}
		return g1.lobbyID > g2.lobbyID;
	}
};
#else
struct GameSortStruct
{
	bool operator()(GameSpyStagingRoom *g1, GameSpyStagingRoom *g2) const
	{
		// sort CRC mismatches to the bottom
		Bool g1Good = (g1->getExeCRC() == TheGlobalData->m_exeCRC && g1->getIniCRC() == TheGlobalData->m_iniCRC);
		Bool g2Good = (g2->getExeCRC() == TheGlobalData->m_exeCRC && g2->getIniCRC() == TheGlobalData->m_iniCRC);
		if ( g1Good ^ g2Good )
		{
			return g1Good;
		}

		// sort games with private ladders to the bottom
		Bool g1UnknownLadder = (g1->getLadderPort() && TheLadderList->findLadder(g1->getLadderIP(), g1->getLadderPort()) == nullptr);
		Bool g2UnknownLadder = (g2->getLadderPort() && TheLadderList->findLadder(g2->getLadderIP(), g2->getLadderPort()) == nullptr);
		if ( g1UnknownLadder ^ g2UnknownLadder )
		{
			return g2UnknownLadder;
		}

		// sort full games to the bottom
		Bool g1Full = (g1->getNumNonObserverPlayers() == g1->getMaxPlayers() || g1->getNumPlayers() == MAX_SLOTS);
		Bool g2Full = (g2->getNumNonObserverPlayers() == g2->getMaxPlayers() || g2->getNumPlayers() == MAX_SLOTS);
		if ( g1Full ^ g2Full )
		{
			return g2Full;
		}

		if (sortBuddies)
		{
			Bool g1HasBuddies = (theBuddyGames->find(g1) != theBuddyGames->end());
			Bool g2HasBuddies = (theBuddyGames->find(g2) != theBuddyGames->end());
			if ( g1HasBuddies ^ g2HasBuddies )
			{
				return g1HasBuddies;
			}
		}

		switch(theGameSortType)
		{
		case GAMESORT_ALPHA_ASCENDING:
			return wcsicmp(g1->getGameName().str(), g2->getGameName().str()) < 0;
			break;
		case GAMESORT_ALPHA_DESCENDING:
			return wcsicmp(g1->getGameName().str(),g2->getGameName().str()) > 0;
			break;
		case GAMESORT_PING_ASCENDING:
			return g1->getPingAsInt() < g2->getPingAsInt();
			break;
		case GAMESORT_PING_DESCENDING:
			return g1->getPingAsInt() > g2->getPingAsInt();
			break;
		}
		return false;
	}
};
#endif

static Int insertGame(GameWindow* win, LobbyEntry& lobbyInfo, Bool showMap)
{
	Color gameColor = GameSpyColor[GSCOLOR_GAME];
	if (lobbyInfo.exe_crc != TheGlobalData->m_exeCRC || lobbyInfo.ini_crc != TheGlobalData->m_iniCRC)
	{
		gameColor = GameSpyColor[GSCOLOR_GAME_CRCMISMATCH];
	}
#if defined(GENERALS_ONLINE)
	// Buddy lobby highlight:
	if (theBuddyGames && theBuddyGames->count(lobbyInfo.lobbyID))
	{
		const bool nonJoinable =
				(gameColor == GameSpyColor[GSCOLOR_GAME_CRCMISMATCH]);

		gameColor = nonJoinable
			? GameMakeColor(0, 98, 130, 255)   // darker cyan
			: GameMakeColor(20, 177, 255, 255); // lighter cyan
	}
#endif
	std::wstring strOwnerName = L"";
	for (LobbyMemberEntry& member : lobbyInfo.members)
	{
		if (member.user_id == lobbyInfo.owner)
		{
			strOwnerName = from_utf8(member.display_name);
		}
	}

	UnicodeString gameName;
	gameName.format(L"%s (%s)", from_utf8(lobbyInfo.name).c_str(), strOwnerName.c_str());

	int numPlayers = lobbyInfo.current_players;
	int maxPlayers = lobbyInfo.max_players;

	AsciiString lobbyMapName = AsciiString(lobbyInfo.map_name.c_str());
	AsciiString ladder = AsciiString("TODO_NGMP");
	USHORT ladderPort = 1;
	int gameID = lobbyInfo.lobbyID; // TODO_NGMP: Downcast. We should use int64 everywhere, although its unlikely we actually need int64 for lobby since its reset regularly.

	bool bHasPassword = lobbyInfo.passworded;

	bool bAllowSpectators = lobbyInfo.allow_observers;
	bool bTrackStats = lobbyInfo.track_stats;

	int latency = lobbyInfo.latency;

	// TODO_NGMP: Asian text
	/*
	if (TheGameSpyInfo->getDisallowAsianText())
	{
		const WideChar* buff = gameName.str();
		Int length = gameName.getLength();
		for (Int i = 0; i < length; ++i)
		{
			if (buff[i] >= 256)
				return -1;
		}
	}
	else if (TheGameSpyInfo->getDisallowNonAsianText())
	{
		const WideChar* buff = gameName.str();
		Int length = gameName.getLength();
		Bool hasUnicode = FALSE;
		for (Int i = 0; i < length; ++i)
		{
			if (buff[i] >= 256)
			{
				hasUnicode = TRUE;
				break;
			}
		}
		if (!hasUnicode)
			return -1;
	}
	*/


	Int rowCount = GadgetListBoxGetNumEntries(win);
	bool bAlternate = (rowCount % 2 == 0);
	if (bAlternate && gameColor == GameSpyColor[GSCOLOR_GAME])
	{
		gameColor = GameMakeColor(191, 198, 201, 255);
	}
	Int index = GadgetListBoxAddEntryText(win, gameName, gameColor, -1, COLUMN_NAME);
	GadgetListBoxSetItemData(win, (void*)gameID, index);

	UnicodeString s;

	if (showMap)
	{
		UnicodeString mapName;
		const MapMetaData* md = TheMapCache->findMap(lobbyMapName);
		if (md)
		{
			mapName = md->m_displayName;
		}
		else
		{
			const char* start = lobbyMapName.reverseFind('\\');
			if (start)
			{
				++start;
			}
			else
			{
				start = lobbyMapName.str();
			}
			mapName.translate(start);
		}
		GadgetListBoxAddEntryText(win, mapName, gameColor, index, COLUMN_MAP);

		// TODO_NGMP: Support ladder again
		if (TheLadderList != nullptr)
		{
			const LadderInfo* li = TheLadderList->findLadder(ladder, ladderPort);
			if (li)
			{
				GadgetListBoxAddEntryText(win, li->name, gameColor, index, COLUMN_LADDER);
			}
			else if (ladderPort)
			{
				GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:UnknownLadder"), gameColor, index, COLUMN_LADDER);
			}
			else
			{
				GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:NoLadder"), gameColor, index, COLUMN_LADDER);
			}
		}
		else
		{
			GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:NoLadder"), gameColor, index, COLUMN_LADDER);
		}
	}
	else
	{
		GadgetListBoxAddEntryText(win, UnicodeString(L" "), gameColor, index, COLUMN_MAP);
		GadgetListBoxAddEntryText(win, UnicodeString(L" "), gameColor, index, COLUMN_LADDER);
	}

	s.format(L"%d/%d", numPlayers, maxPlayers);
	const bool bIsFull = (lobbyInfo.current_players == lobbyInfo.max_players || lobbyInfo.current_players == MAX_SLOTS);
	const bool bIsAlmostFull = !bIsFull && (maxPlayers > 0) && ((float)numPlayers / (float)maxPlayers >= 0.6f);
	Color numPlayersColor = bIsFull ? GameMakeColor(255, 80, 80, 255) 
		: bIsAlmostFull ? GameMakeColor(16, 173, 144, 255) 
		: gameColor;
	GadgetListBoxAddEntryText(win, s, numPlayersColor, index, COLUMN_NUMPLAYERS);

	if (bHasPassword)
	{
		const Image* img = TheMappedImageCollection->findImageByName("Password");
		Int width = 10, height = 10;
		if (img)
		{
			width = img->getImageWidth();
			height = img->getImageHeight();
		}
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_PASSWORD, width, height);
	}
	else
	{
		GadgetListBoxAddEntryText(win, UnicodeString(L" "), gameColor, index, COLUMN_PASSWORD);
	}

	if (bAllowSpectators)
	{
		const Image* img = TheMappedImageCollection->findImageByName("Observer");
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_OBSERVER, img->getImageHeight()/2, img->getImageWidth()/2);
	}
	else
	{
		GadgetListBoxAddEntryText(win, UnicodeString(L" "), gameColor, index, COLUMN_OBSERVER);
	}

	if (bTrackStats)
	{
		const Image* img = TheMappedImageCollection->findImageByName("GoodStatsIcon");
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_USE_STATS, img->getImageHeight(), img->getImageWidth());
	}

	s.format(L"%d", latency);
	GadgetListBoxAddEntryText(win, s, gameColor, index, COLUMN_PING);
	Int ping = latency;
	Int width = 10, height = 10;
	if (pingImages[0])
	{
		width = pingImages[0]->getImageWidth();
		height = pingImages[0]->getImageHeight();
	}
	// CLH picking an arbitrary number for our ping display
	// TODO_NGMP: Better values for this
	if (ping < 250)
	{
		GadgetListBoxAddEntryImage(win, pingImages[0], index, COLUMN_PING, width, height);
	}
	else if (ping < 500)
	{
		GadgetListBoxAddEntryImage(win, pingImages[1], index, COLUMN_PING, width, height);
	}
	else
	{
		GadgetListBoxAddEntryImage(win, pingImages[2], index, COLUMN_PING, width, height);
	}

	return index;

	/*
	game->cleanUpSlotPointers();
	Color gameColor = GameSpyColor[GSCOLOR_GAME];
	if (game->getNumNonObserverPlayers() == game->getMaxPlayers() || game->getNumPlayers() == MAX_SLOTS)
	{
		gameColor = GameSpyColor[GSCOLOR_GAME_FULL];
	}
	if (game->getExeCRC() != TheGlobalData->m_exeCRC || game->getIniCRC() != TheGlobalData->m_iniCRC)
	{
		gameColor = GameSpyColor[GSCOLOR_GAME_CRCMISMATCH];
	}
	UnicodeString gameName = game->getGameName();

	if(TheGameSpyInfo->getDisallowAsianText())
	{
		const WideChar *buff = gameName.str();
		Int length =  gameName.getLength();
		for(Int i = 0; i < length; ++i)
		{
			if(buff[i] >= 256)
				return -1;
		}
	}
	else if(TheGameSpyInfo->getDisallowNonAsianText())
	{
		const WideChar *buff = gameName.str();
		Int length =  gameName.getLength();
		Bool hasUnicode = FALSE;
		for(Int i = 0; i < length; ++i)
		{
			if(buff[i] >= 256)
			{
				hasUnicode = TRUE;
				break;
			}
		}
		if(!hasUnicode)
			return -1;
	}



	Int index = GadgetListBoxAddEntryText(win, game->getGameName(), gameColor, -1, COLUMN_NAME);
	GadgetListBoxSetItemData(win, (void *)game->getID(), index);

	UnicodeString s;

	if (showMap)
	{
		UnicodeString mapName;
		const MapMetaData *md = TheMapCache->findMap(game->getMap());
		if (md)
		{
			mapName = md->m_displayName;
		}
		else
		{
			const char *start = game->getMap().reverseFind('\\');
			if (start)
			{
				++start;
			}
			else
			{
				start = game->getMap().str();
			}
			mapName.translate( start );
		}
		GadgetListBoxAddEntryText(win, mapName, gameColor, index, COLUMN_MAP);

		const LadderInfo * li = TheLadderList->findLadder(game->getLadderIP(), game->getLadderPort());
		if (li)
		{
			GadgetListBoxAddEntryText(win, li->name, gameColor, index, COLUMN_LADDER);
		}
		else if (game->getLadderPort())
		{
			GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:UnknownLadder"), gameColor, index, COLUMN_LADDER);
		}
		else
		{
			GadgetListBoxAddEntryText(win, TheGameText->fetch("GUI:NoLadder"), gameColor, index, COLUMN_LADDER);
		}
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_MAP);
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_LADDER);
	}

	s.format(L"%d/%d", game->getReportedNumPlayers(), game->getReportedMaxPlayers());
	GadgetListBoxAddEntryText(win, s, gameColor, index, COLUMN_NUMPLAYERS);

	if (game->getHasPassword())
	{
		const Image *img = TheMappedImageCollection->findImageByName("Password");
		Int width = 10, height = 10;
		if (img)
		{
			width = img->getImageWidth();
			height = img->getImageHeight();
		}
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_PASSWORD, width, height);
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_PASSWORD);
	}

	if (game->getAllowObservers())
	{
		const Image *img = TheMappedImageCollection->findImageByName("Observer");
		GadgetListBoxAddEntryImage(win, img, index, COLUMN_OBSERVER);
	}
	else
	{
		GadgetListBoxAddEntryText(win, L" ", gameColor, index, COLUMN_OBSERVER);
	}

#if !RTS_GENERALS
  {
    if (game->getUseStats())
    {
      if (const Image *img = TheMappedImageCollection->findImageByName("GoodStatsIcon"))
      {
        GadgetListBoxAddEntryImage(win, img, index, COLUMN_USE_STATS, img->getImageHeight(), img->getImageWidth());
      }
    }
  }
#endif

	s.format(L"%d", game->getPingAsInt());
	GadgetListBoxAddEntryText(win, s, gameColor, index, COLUMN_PING);
	Int ping = game->getPingAsInt();
	Int width = 10, height = 10;
	if (pingImages[0])
	{
		width = pingImages[0]->getImageWidth();
		height = pingImages[0]->getImageHeight();
	}
	// CLH picking an arbitrary number for our ping display
	if (ping < TheGameSpyConfig->getPingCutoffGood())
	{
		GadgetListBoxAddEntryImage(win, pingImages[0], index, COLUMN_PING, width, height);
	}
	else if (ping < TheGameSpyConfig->getPingCutoffBad())
	{
		GadgetListBoxAddEntryImage(win, pingImages[1], index, COLUMN_PING, width, height);
	}
	else
	{
		GadgetListBoxAddEntryImage(win, pingImages[2], index, COLUMN_PING, width, height);
	}

	return index;
	*/

}

void RefreshGameListBox(GameWindow* win, Bool showMap)
{
	if (!win)
		return;

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		return;
	}

	// save off selection
	Int selectedIndex = -1;
	Int selectedID = 0;
	GadgetListBoxGetSelected(win, &selectedIndex);
	if (selectedIndex != -1)
	{
		selectedID = (Int)GadgetListBoxGetItemData(win, selectedIndex);
	}
	int prevPos = GadgetListBoxGetTopVisibleEntry(win);

	pLobbyInterface->SearchForLobbies(
		[=]()
		{
			win->winEnable(false);
			GadgetListBoxAddEntryText(win, UnicodeString(L"Searching for public lobbies..."), GameMakeColor(255, 194, 15, 255), -1, -1);
		},
		[=](std::vector<LobbyEntry> vecLobbies)
		{
			// empty listbox
			GadgetListBoxReset(win);

			size_t numResults = vecLobbies.size();

			GadgetListBoxReset(win);
			if (numResults == 0)
			{
				win->winEnable(false);
				GadgetListBoxAddEntryText(win, UnicodeString(L"No lobbies were found"), GameMakeColor(255, 194, 15, 255), -1, -1);

			}
			else
			{
				win->winEnable(true);

				// sort our games
				typedef std::multiset<LobbyEntry, GameSortStruct> SortedGameList;
				SortedGameList sgl;
				populateBuddyGames(vecLobbies);
				for (LobbyEntry& lobby : vecLobbies)
				{
					sgl.insert(lobby);
				}

				// now add the games
				Int indexToSelect = -1;

				int i = 0;
				//for (LobbyEntry lobby : vecLobbies)
				for (SortedGameList::iterator sglIt = sgl.begin(); sglIt != sgl.end(); ++sglIt)
				{
					LobbyEntry lobby = *sglIt;

                    // Logical index for this entry in the new sorted list
					float logicalIndex = (float)i;

					// register/update animation index for this lobbyID
					UpdateAndGetGameRowIndex(lobby.lobbyID, logicalIndex);

					Int index = insertGame(win, lobby, showMap);
					if (lobby.lobbyID == selectedID)
					{
						indexToSelect = (int)index; // TODO_NGMP: downcast
					}

					++i;
				}

				// restore selection
				GadgetListBoxSetSelected(win, indexToSelect); // even for -1, so we can disable the 'Join Game' button
				//	if(prevPos > 10)
				GadgetListBoxSetTopVisibleEntry(win, prevPos);//+ 1

				if (indexToSelect < 0 && selectedID)
				{
					TheWindowManager->winSetLoneWindow(NULL);
				}
			}
		});

	// Note: clearBuddyGames(), GadgetListBoxSetSelected(), and GadgetListBoxSetTopVisibleEntry()
	// are now handled inside the SearchForLobbies callback above (after async operation completes).
	// The code below was removed to fix a race condition where these operations executed on the
	// empty/reset listbox before the async callback populated it, causing crashes.
}

void RefreshGameInfoListBox( GameWindow *mainWin, GameWindow *win )
{
//	if (!mainWin || !win)
//		return;
//
//	GadgetListBoxReset(win);
//
//	Int selected = -1;
//	GadgetListBoxGetSelected(mainWin, &selected);
//	if (selected < 0)
//	{
//		return;
//	}
//
//	Int selectedID = (Int)GadgetListBoxGetItemData(mainWin, selected);
//	if (selectedID < 0)
//	{
//		return;
//	}
//
//	StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
//	StagingRoomMap::iterator srmIt = srm->find(selectedID);
//	if (srmIt != srm->end())
//	{
//		GameSpyStagingRoom *theRoom = srmIt->second;
//		theRoom->cleanUpSlotPointers();
//
//		// game name
////		GadgetListBoxAddEntryText(listboxLobbyGameInfo, theRoom->getGameName(), GameSpyColor[GSCOLOR_DEFAULT], -1);
//
//		const LadderInfo * li = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
//		if (li)
//		{
//			UnicodeString tmp;
//			tmp.format(TheGameText->fetch("TOOLTIP:LadderName"), li->name.str());
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, tmp, GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//		else if (theRoom->getLadderPort())
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:UnknownLadder"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//		else
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:NoLadder"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//
//		if (theRoom->getExeCRC() != TheGlobalData->m_exeCRC || theRoom->getIniCRC() != TheGlobalData->m_iniCRC)
//		{
//			GadgetListBoxAddEntryText(listboxLobbyGameInfo, TheGameText->fetch("TOOLTIP:InvalidGameVersionSingleLine"), GameSpyColor[GSCOLOR_DEFAULT], -1);
//		}
//
//		// map name
//		UnicodeString mapName;
//		const MapMetaData *md = TheMapCache->findMap(theRoom->getMap());
//		if (md)
//		{
//			mapName = md->m_displayName;
//		}
//		else
//		{
//			const char *start = theRoom->getMap().reverseFind('\\');
//			if (start)
//			{
//				++start;
//			}
//			else
//			{
//				start = theRoom->getMap().str();
//			}
//			mapName.translate( start );
//		}
//
//		GadgetListBoxAddEntryText(listboxLobbyGameInfo, mapName, GameSpyColor[GSCOLOR_DEFAULT], -1);
//
//		// player list (rank, win/loss, side)
//		for (Int i=0; i<MAX_SLOTS; ++i)
//		{
//			const GameSpyGameSlot *slot = theRoom->getGameSpySlot(i);
//			if (slot && slot->isHuman())
//			{
//				UnicodeString theName, theRating, thePlayerTemplate;
//				Int colorIdx = slot->getColor();
//				theName = slot->getName();
//				theRating.format(L" (%d-%d)", slot->getWins(), slot->getLosses());
//				const PlayerTemplate * pt = ThePlayerTemplateStore->getNthPlayerTemplate(slot->getPlayerTemplate());
//				if (pt)
//				{
//					thePlayerTemplate = pt->getDisplayName();
//				}
//				else
//				{
//					thePlayerTemplate = TheGameText->fetch("GUI:Random");
//				}
//
//				UnicodeString theText;
//				theText.format(L"%ls - %ls - %ls", theName.str(), thePlayerTemplate.str(), theRating.str());
//
//				Int theColor = GameSpyColor[GSCOLOR_DEFAULT];
//				const MultiplayerColorDefinition *mcd = TheMultiplayerSettings->getColor(colorIdx);
//				if (mcd)
//				{
//					theColor = mcd->getColor();
//				}
//
//				GadgetListBoxAddEntryText(listboxLobbyGameInfo, theText, theColor, -1);
//			}
//		}
//	}

}

// ---------------------------------------------------------------------------
// compute pixel offset for a game list row
// ---------------------------------------------------------------------------
int GetGameListRowPixelOffsetForRow(GameWindow* window, int rowIndex, int rowHeight)
{
	if (!window)
		return 0;

	// We rely on listbox item data storing lobbyID, like RefreshGameListBox uses
	Int lobbyID = (Int)GadgetListBoxGetItemData(window, rowIndex);
	if (lobbyID == 0)
		return 0;

    GameWindow* mainGameList = GetGameListBox();

    int animKey;
    if (window == mainGameList)
    {
        // For the main game list: use lobbyID
        animKey = lobbyID;
    }
    else
    {
        // For all other lists: keep per row key to avoid overlap
        animKey = lobbyID * 1024 + rowIndex;
    }


	// Get smoothed "visual index" for this lobbyID
	float visualIndex = UpdateAndGetGameRowIndex(animKey, (float)rowIndex);

	// Offset = (visualIndex - logicalIndex) * rowHeight
	float offsetRows = visualIndex - (float)rowIndex;
	int pixelOffset = (int)(offsetRows * (float)rowHeight);

	return pixelOffset;
}


void RefreshGameListBoxes( void )
{
	GameWindow *main = GetGameListBox();
	GameWindow *info = GetGameInfoListBox();

	RefreshGameListBox( main, (info == nullptr) );

	if (info)
	{
		RefreshGameInfoListBox( main, info );
	}
}

void ToggleGameListType()
{
	isSmall = !isSmall;
	if(isSmall)
	{
		parentGameListLarge->winHide(TRUE);
//		parentGameListSmall->winHide(FALSE);
	}
	else
	{
		parentGameListLarge->winHide(FALSE);
//		parentGameListSmall->winHide(TRUE);
	}

	RefreshGameListBoxes();
}

// for use by GameWindow::winSetTooltipFunc
// displays the Army Tooltip for the player templates
void playerTemplateComboBoxTooltip(GameWindow *wndComboBox, WinInstanceData *instData, UnsignedInt mouse)
{
	Int index = 0;
	GadgetComboBoxGetSelectedPos(wndComboBox, &index);
	Int templateNum = (Int)GadgetComboBoxGetItemData(wndComboBox, index);
	UnicodeString ustringTooltip;
	if (templateNum == -1)
	{
			// the "Random" template is always first
			ustringTooltip = TheGameText->fetch("TOOLTIP:BioStrategyLong_Random");
	}
	else
	{
		const PlayerTemplate *playerTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(templateNum);
		if (playerTemplate)
		{
			ustringTooltip = TheGameText->fetch(playerTemplate->getTooltip());
		}
	}
	TheMouse->setCursorTooltip(ustringTooltip);
}

// -----------------------------------------------------------------------------

// for use by GameWindow::winSetTooltipFunc
// displays the Army Tooltip for the player templates
void playerTemplateListBoxTooltip(GameWindow *wndListBox, WinInstanceData *instData, UnsignedInt mouse)
{
	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);
	GadgetListBoxGetEntryBasedOnXY(wndListBox, x, y, row, col);
	if (row == -1 || col == -1)
		return;

	Int templateNum = (Int)GadgetListBoxGetItemData(wndListBox, row, col);
	UnicodeString ustringTooltip;
	if (templateNum == -1)
	{
			// the "Random" template is always first
			ustringTooltip = TheGameText->fetch("TOOLTIP:BioStrategyLong_Random");
	}
	else
	{
		const PlayerTemplate *playerTemplate = ThePlayerTemplateStore->getNthPlayerTemplate(templateNum);
		if (playerTemplate)
		{
			ustringTooltip = TheGameText->fetch(playerTemplate->getTooltip());
		}
	}

	// use no tooltip delay here
	TheMouse->setCursorTooltip(ustringTooltip, 0);
}
