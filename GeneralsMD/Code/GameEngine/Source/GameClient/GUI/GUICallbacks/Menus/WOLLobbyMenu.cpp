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
// FILE: WOLLobbyMenu.cpp
// Author: Chris Huybregts, November 2001
// Description: WOL Lobby Menu
///////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/GameEngine.h"
#include "Common/GameState.h"
#include "Common/MiniLog.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "Common/CustomMatchPreferences.h"
#include "Common/version.h"
#include "GameClient/AnimateWindowManager.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameClient.h"
#include "GameClient/Shell.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameClient/Mouse.h"
#include "GameClient/Display.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/GameWindowTransitions.h"

#include "GameLogic/GameLogic.h"

#include "GameClient/LanguageFilter.h"
#include "GameNetwork/GameSpy/BuddyDefs.h"
#include "GameNetwork/GameSpy/GSConfig.h"
#include "GameNetwork/GameSpy/LadderDefs.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameNetwork/GameSpy/PeerThread.h"
#include "GameNetwork/GameSpy/PersistentStorageDefs.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/GameSpy/LobbyUtils.h"
#include "GameNetwork/RankPointValue.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

void refreshGameList( Bool forceRefresh = FALSE );
void refreshPlayerList( Bool forceRefresh = FALSE );

#ifdef DEBUG_LOGGING
#define PERF_TEST
static LogClass s_perfLog("Perf.txt");
#define PERF_LOG(x) s_perfLog.log x
#else // DEBUG_LOGGING
#define PERF_LOG(x) {}
#endif // DEBUG_LOGGING

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static Bool isShuttingDown = false;
static Bool buttonPushed = false;
static const char *nextScreen = NULL;
static Bool raiseMessageBoxes = false;
static time_t gameListRefreshTime = 0;
static const time_t gameListRefreshInterval = 10000;
static time_t playerListRefreshTime = 0;
static const time_t playerListRefreshInterval = 5000;

void setUnignoreText( WindowLayout *layout, AsciiString nick, GPProfile id);
static void doSliderTrack(GameWindow *control, Int val);
Bool DontShowMainMenu = FALSE;
enum { COLUMN_PLAYERNAME = 1 };

// window ids ------------------------------------------------------------------------------
static NameKeyType parentWOLLobbyID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;
static NameKeyType buttonHostID = NAMEKEY_INVALID;
static NameKeyType buttonRefreshID = NAMEKEY_INVALID;
static NameKeyType buttonJoinID = NAMEKEY_INVALID;
static NameKeyType buttonBuddyID = NAMEKEY_INVALID;
static NameKeyType buttonEmoteID = NAMEKEY_INVALID;
static NameKeyType textEntryChatID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyPlayersID = NAMEKEY_INVALID;
static NameKeyType listboxLobbyChatID = NAMEKEY_INVALID;
static NameKeyType comboLobbyGroupRoomsID = NAMEKEY_INVALID;
//static NameKeyType // sliderChatAdjustID = NAMEKEY_INVALID;

// Window Pointers ------------------------------------------------------------------------
static GameWindow *parentWOLLobby = NULL;
static GameWindow *buttonBack = NULL;
static GameWindow *buttonHost = NULL;
static GameWindow *buttonRefresh = NULL;
static GameWindow *buttonJoin = NULL;
static GameWindow *buttonBuddy = NULL;
static GameWindow *buttonEmote = NULL;
static GameWindow *textEntryChat = NULL;
static GameWindow *listboxLobbyPlayers = NULL;
static GameWindow *listboxLobbyChat = NULL;
static GameWindow *comboLobbyGroupRooms = NULL;
static GameWindow *parent = NULL;

static Int groupRoomToJoin = 0;
static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

#if defined(RTS_DEBUG)
Bool g_fakeCRC = FALSE;
Bool g_debugSlots = FALSE;
#endif

std::list<PeerResponse> TheLobbyQueuedUTMs;

// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus(void);
}
extern int isThreadHosting;

Bool handleLobbySlashCommands(UnicodeString uText)
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
		// TODO_NGMP
		/*
		UnicodeString s;
		s.format(L"Hosting qr2:%d thread:%d", getQR2HostingStatus(), isThreadHosting);
		TheGameSpyInfo->addText(s, GameSpyColor[GSCOLOR_DEFAULT], NULL);
		*/
		return TRUE; // was a slash command
	}
	else if (token == "me" && uText.getLength()>4)
	{
		UnicodeString msg = UnicodeString(uText.str() + 4); // skip the /me
		NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->SendChatMessageToCurrentRoom(msg, true);
		return TRUE; // was a slash command
	}
	else if (token == "help" || token == "commands")
	{
		GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"The following commands are available:"), GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
		GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"/name <value> - Changes your display name - Example: /name General Granger"), GameSpyColor[GSCOLOR_CHAT_NORMAL], -1, -1);
		return TRUE; // was a slash command
	}
	else if ((token == "name" && uText.getLength() > 6) || (token == "nick" && uText.getLength() > 6))
	{
		AsciiString newName;
		newName.translate(UnicodeString(uText.str() + 6)); // skip the /name or nick

		if (newName.getLength() < 3 || newName.getLength() > 16)
		{
			GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Your new name must be between 3 and 16 characters."), GameMakeColor(255, 0, 0, 255), -1, -1);
		}
		else
		{
			NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_ChangeName(newName.str());
		}
		
		return TRUE; // was a slash command
	}
	else if (token == "forcerelay")
	{
		extern bool g_bForceRelay;
		extern UnsignedInt m_exeCRCOriginal;
		g_bForceRelay = true;
		m_exeCRCOriginal = TheWritableGlobalData->m_exeCRC;
		TheWritableGlobalData->m_exeCRC = 123456;
		GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Relays are now forced on. You will only be able to join lobbies where the same option has been set. Use /allowrelay to reset this"), GameMakeColor(255, 0, 0, 255), -1, -1);
		return TRUE; // was a slash command
	}
	else if (token == "allowrelay")
	{
		extern bool g_bForceRelay;
		extern UnsignedInt m_exeCRCOriginal;
		g_bForceRelay = false;
		TheWritableGlobalData->m_exeCRC = m_exeCRCOriginal;
		m_exeCRCOriginal = 0;
		GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Relays are now optional again. You will only be able to join lobbies where the same option has been set. Use /forcerelay to reset this"), GameMakeColor(255, 0, 0, 255), -1, -1);
		return TRUE; // was a slash command
	}
	else if (token == "refresh")
	{
		// Added 2/19/03 added the game refresh
		refreshGameList(TRUE);	
		refreshPlayerList(TRUE);
		return TRUE; // was a slash command
	}
	/*
	if (token == "togglegamelist")
	{
		NameKeyType buttonID = NAMEKEY("WOLCustomLobby.wnd:ButtonGameListToggle");
		GameWindow *button = TheWindowManager->winGetWindowFromId(parent, buttonID);
		if (button)
		{
			button->winHide(!button->winIsHidden());
		}
		return TRUE; // was a slash command
	}
	else if (token == "adjustchat")
	{
		NameKeyType sliderID = NAMEKEY("WOLCustomLobby.wnd:SliderChatAdjust");
		GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderID);
		if (slider)
		{
			slider->winHide(!slider->winIsHidden());
		}
		return TRUE; // was a slash command
	}
	*/
#if defined(RTS_DEBUG)
	else if (token == "fakecrc")
	{
		g_fakeCRC = !g_fakeCRC;
		TheGameSpyInfo->addText(UnicodeString(L"Toggled CRC fakery"), GameSpyColor[GSCOLOR_DEFAULT], NULL);
		return TRUE; // was a slash command
	}
	else if (token == "slots")
	{
		g_debugSlots = !g_debugSlots;
		TheGameSpyInfo->addText(UnicodeString(L"Toggled SlotList debug"), GameSpyColor[GSCOLOR_DEFAULT], NULL);
		return TRUE; // was a slash command
	}
#endif

	return FALSE; // not a slash command
}

static Bool s_tryingToHostOrJoin = FALSE;
void SetLobbyAttemptHostJoin(Bool start)
{
	s_tryingToHostOrJoin = start;
}

// Tooltips -------------------------------------------------------------------------------

static void playerTooltip(GameWindow *window,
													WinInstanceData *instData,
													UnsignedInt mouse)
{
	// TODO_NGMP: Support all of this again

	Int x, y, row, col;
	x = LOLONGTOSHORT(mouse);
	y = HILONGTOSHORT(mouse);

	GadgetListBoxGetEntryBasedOnXY(window, x, y, row, col);

	if (row == -1 || col == -1)
	{
		TheMouse->setCursorTooltip(UnicodeString::TheEmptyString);//TheGameText->fetch("TOOLTIP:PlayersInLobby") );
		return;
	}

	UnicodeString uName = GadgetListBoxGetText(window, row, COLUMN_PLAYERNAME);
	AsciiString aName;
	aName.translate(uName);

	NetworkRoomMember* pRoomMember = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetRoomMemberFromIndex(row);

	if (col > 0)
	{
		if (pRoomMember != nullptr)
		{
			std::string strTooltip = std::format("Display Name: {}\nUser ID: {}", pRoomMember->display_name.c_str(), pRoomMember->user_id);

			UnicodeString ucTooltip;
			ucTooltip.translate(strTooltip.c_str());

			TheMouse->setCursorTooltip(ucTooltip, -1, NULL, 1.5f); // the text and width are the only params used.  the others are the default values.
		}
		else
		{
			TheMouse->setCursorTooltip(UnicodeString(L"Error: 1"), -1, NULL, 1.5f); // the text and width are the only params used.  the others are the default values.
		}
		


	}


	return;


	PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
	PlayerInfo *info = &(it->second);
	Bool isLocalPlayer = (TheGameSpyInfo->getLocalName().compareNoCase(info->m_name) == 0);

	if (col == 0)
	{
		if (info->m_preorder)
		{
			TheMouse->setCursorTooltip( TheGameText->fetch("TOOLTIP:LobbyOfficersClub") );
		}
		else
		{
			TheMouse->setCursorTooltip( UnicodeString::TheEmptyString);
		}
		return;
	}

	AsciiString	playerLocale = info->m_locale;
	AsciiString localeIdentifier;
	localeIdentifier.format("WOL:Locale%2.2d", atoi(playerLocale.str()));
	Int					playerWins   = info->m_wins;
	Int					playerLosses = info->m_losses;
	UnicodeString	playerInfo;
	playerInfo.format(TheGameText->fetch("TOOLTIP:PlayerInfo"), TheGameText->fetch(localeIdentifier).str(), playerWins, playerLosses);

	UnicodeString tooltip = UnicodeString::TheEmptyString;//TheGameText->fetch("TOOLTIP:PlayersInLobby");
	if (isLocalPlayer)
	{
		tooltip.format(TheGameText->fetch("TOOLTIP:LocalPlayer"), uName.str());
	}
	else
	{
		// not us
		if (TheGameSpyInfo->getBuddyMap()->find(info->m_profileID) != TheGameSpyInfo->getBuddyMap()->end())
		{
			// buddy
			tooltip.format(TheGameText->fetch("TOOLTIP:BuddyPlayer"), uName.str());
		}
		else
		{
			if (info->m_profileID)
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

	if (info->isIgnored())
	{
		tooltip.concat(TheGameText->fetch("TOOLTIP:IgnoredModifier"));
	}

	if (info->m_profileID)
	{
		tooltip.concat(playerInfo);
	}

	Int rank = 0;
	Int i = 0;
	while( info->m_rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;
	AsciiString sideName = "GUI:RandomSide";
	if (info->m_side > 0)
	{		
		const PlayerTemplate *fac = ThePlayerTemplateStore->getNthPlayerTemplate(info->m_side);
		if (fac)
		{
			sideName.format("SIDE:%s", fac->getSide().str());
		}
	}
	AsciiString rankName;
	rankName.format("GUI:GSRank%d", rank);
	UnicodeString tmp;
	tmp.format(L"\n%ls %ls", TheGameText->fetch(sideName).str(), TheGameText->fetch(rankName).str());
	tooltip.concat(tmp);

	TheMouse->setCursorTooltip( tooltip, -1, NULL, 1.5f ); // the text and width are the only params used.  the others are the default values.
}

static void populateGroupRoomListbox(GameWindow *lb)
{
	if (!lb)
		return;

	GadgetComboBoxReset(lb);
	Int indexToSelect = -1;
	GroupRoomMap::iterator iter;

	// now populate the combo box
	// TODO_NGMP
	/*
	for (iter = TheGameSpyInfo->getGroupRoomList()->begin(); iter != TheGameSpyInfo->getGroupRoomList()->end(); ++iter)
	{
		GameSpyGroupRoom room = iter->second;
		if (room.m_groupID != TheGameSpyConfig->getQMChannel())
		{
			DEBUG_LOG(("populateGroupRoomListbox(): groupID %d", room.m_groupID));
			if (room.m_groupID == TheGameSpyInfo->getCurrentGroupRoom())
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_CURRENTROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
				indexToSelect = selected;
			}
			else
			{
				Int selected = GadgetComboBoxAddEntry(lb, room.m_translatedName, GameSpyColor[GSCOLOR_ROOM]);
				GadgetComboBoxSetItemData(lb, selected, (void *)(room.m_groupID));
			}
		}
		else
		{
			DEBUG_LOG(("populateGroupRoomListbox(): skipping QM groupID %d", room.m_groupID));
		}
	}
	*/

	for (NetworkRoom netRoom : NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms())
	{
		// TODO_NGMP: Support current group color highlighting again
		int roomID = netRoom.GetRoomID();
		if (roomID == NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetCurrentRoomID())
		{
			Int selected = GadgetComboBoxAddEntry(lb, netRoom.GetRoomDisplayName(), GameSpyColor[GSCOLOR_CURRENTROOM]);
			GadgetComboBoxSetItemData(lb, selected, (void*)(roomID));
			indexToSelect = selected;
		}
		else
		{
			Int selected = GadgetComboBoxAddEntry(lb, netRoom.GetRoomDisplayName(), GameSpyColor[GSCOLOR_ROOM]);
			GadgetComboBoxSetItemData(lb, selected, (void*)(roomID));
		}
	}

	GadgetComboBoxSetSelectedPos(lb, indexToSelect);
}

static const char *rankNames[] = {
	"Private",
	"Corporal",
	"Sergeant",
	"Lieutenant",
	"Captain",
	"Major",
	"Colonel",
	"General",
	"Brigadier",
	"Commander",
};

const Image* LookupSmallRankImage(Int side, Int rankPoints)
{
	if (rankPoints == 0)
		return NULL;

	Int rank = 0;
	Int i = 0;
	while( rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;

	if (rank < 0 || rank >= 10)
		return NULL;

	AsciiString sideStr = "N";
	switch(side)
	{
		case 2:  //USA
		case 5:  //Super Weapon
		case 6:  //Laser
		case 7:  //Air Force
			sideStr = "USA";
			break;

		case 3:  //China
		case 8:  //Tank
		case 9:  //Infantry
		case 10: //Nuke
			sideStr = "CHA";
			break;

		case 4:  //GLA
		case 11: //Toxin
		case 12: //Demolition
		case 13: //Stealth
			sideStr = "GLA";
			break;
	}

	AsciiString fullImageName;
	fullImageName.format("%s-%s", rankNames[rank], sideStr.str());
	const Image *img = TheMappedImageCollection->findImageByName(fullImageName);
	DEBUG_ASSERTLOG(img, ("*** Could not load small rank image '%s' from TheMappedImageCollection!", fullImageName.str()));
	return img;
}

static Int insertPlayerInListbox(const PlayerInfo& info, Color color)
{
	UnicodeString uStr;
	uStr.translate(info.m_name);

	Int currentRank = info.m_rankPoints;
	Int currentSide = info.m_side;
	/* since PersistentStorage updates now update PlayerInfo, we don't need this.
	if (info.m_profileID)
	{
		PSPlayerStats psStats = TheGameSpyPSMessageQueue->findPlayerStatsByID(info.m_profileID);
		if (psStats.id)
		{
			currentRank = CalculateRank(psStats);

			PerGeneralMap::iterator it;
			Int numGames = 0;
			for(it = psStats.games.begin(); it != psStats.games.end(); ++it)
			{
				if(it->second >= numGames)
				{
					numGames = it->second;
					currentSide = it->first;
				}
			}
			if(numGames == 0 || psStats.gamesAsRandom >= numGames )
			{
				currentSide = 0;
			}
		}
	}
	*/

	// TODO_NGMP: Reimplement this, what were the pre-order bonuses?
	Bool isPreorder = true;
	//Bool isPreorder = TheGameSpyInfo->didPlayerPreorder(info.m_profileID);

	const Image *preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
	Int w = (preorderImg)?preorderImg->getImageWidth():10;
	//Int h = (preorderImg)?preorderImg->getImageHeight():10;
	w = min(GadgetListBoxGetColumnWidth(listboxLobbyPlayers, 0), w);
	Int h = w;
	if (!isPreorder)
		preorderImg = NULL;

	const Image *rankImg = LookupSmallRankImage(currentSide, currentRank);

#if 0  //Officer's Club (preorder image) no longer used in Zero Hour
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, preorderImg, -1, 0, w, h);
	GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, index, 1, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 2);
#else
	Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, -1, 0, w, h);
	GadgetListBoxAddEntryText(listboxLobbyPlayers, uStr, color, index, 1);
#endif
	return index;
}

std::vector<int64_t> m_vecUsersProcessed;

void PopulateLobbyPlayerListbox(void)
{
	m_vecUsersProcessed.clear();
	GadgetListBoxReset(listboxLobbyPlayers);

	for (auto kvPair: NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetMembersListForCurrentRoom())
	{
		NetworkRoomMember& netRoomMember = kvPair.second;


		// TODO_NGMP: Add a batched request
		// TODO_NGMP: Add a timeout to this where we just add the person with no stats
		NGMP_OnlineServicesManager::GetInstance()->GetStatsInterface()->findPlayerStatsByID(netRoomMember.user_id, [=](bool bSuccess, PSPlayerStats stats)
			{
				// safety, this is async so we could in theory get delayed callbacks resulting in dupes
				if (std::find(m_vecUsersProcessed.begin(), m_vecUsersProcessed.end(), netRoomMember.user_id) != m_vecUsersProcessed.end())
				{
					return;
				}

				m_vecUsersProcessed.push_back(netRoomMember.user_id);
				PlayerInfo pi;
				pi.m_name = AsciiString(netRoomMember.display_name.c_str());

				// if we don't have the stats from the server, just add us without any stats
				if (bSuccess)
				{
					Int currentRank = 0;
					Int rankPoints = CalculateRank(stats);
					Int i = 0;
					while (rankPoints >= TheRankPointValues->m_ranks[i + 1])
						++i;
					currentRank = i;

					PerGeneralMap::iterator it;
					Int numWins = 0;
					Int numLosses = 0;
					Int numDiscons = 0;
					Int numGamesTotal = 0;
					for (it = stats.wins.begin(); it != stats.wins.end(); ++it)
					{
						numWins += it->second;
					}
					for (it = stats.losses.begin(); it != stats.losses.end(); ++it)
					{
						numLosses += it->second;
					}
					for (it = stats.discons.begin(); it != stats.discons.end(); ++it)
					{
						numDiscons += it->second;
					}
					for (it = stats.desyncs.begin(); it != stats.desyncs.end(); ++it)
					{
						numDiscons += it->second;
					}

					numDiscons += GetAdditionalDisconnectsFromUserFile(netRoomMember.user_id);

					numGamesTotal = numWins + numLosses + numDiscons;

					// determine favorite army
					Int numGamesThisArmy = 0;
					Int favorite = 0;
					for (it = stats.games.begin(); it != stats.games.end(); ++it)
					{
						if (it->second >= numGamesThisArmy)
						{
							numGamesThisArmy = it->second;
							favorite = it->first;
						}
					}

					int favoriteSide = PLAYERTEMPLATE_RANDOM;
					if (numGamesThisArmy == 0)
					{
						favoriteSide = 0; // this isnt a real army, but they also havent played any games so they cant possibly have a rank
					}
					else if (stats.gamesAsRandom >= numGamesThisArmy)
					{
						favoriteSide = PLAYERTEMPLATE_RANDOM;
					}
					else
					{
						favoriteSide = favorite;

						/*
						const PlayerTemplate* fac = ThePlayerTemplateStore->getNthPlayerTemplate(favorite);
						if (fac)
						{
							AsciiString side;
							side.format("SIDE:%s", fac->getSide().str());

							favoriteSide = TheGameText->fetch(side);
						}
						*/
					}

					// store on playerinfo object
					pi.m_wins = numWins;
					pi.m_losses = numLosses;
					pi.m_profileID = netRoomMember.user_id; // TODO_NGMP: Downcast... we need to use int64_t everywhere really
					pi.m_flags = 0;
					pi.m_rankPoints = rankPoints;
					pi.m_side = favorite;
					pi.m_preorder = 0;
				}

				insertPlayerInListbox(pi, GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
			}, EStatsRequestPolicy::RESPECT_CACHE_ALLOW_REQUEST);

		// TODO_NGMP: Support ignored again
		//insertPlayerInListbox(pi, pi.isIgnored() ? GameSpyColor[GSCOLOR_PLAYER_IGNORED] : GameSpyColor[GSCOLOR_PLAYER_NORMAL]);
	}

	return;

	if (!listboxLobbyPlayers)
		return;

	// TODO_NGMP: Implement friends etc again, retain selection, etc

	// Display players
	PlayerInfoMap *players = TheGameSpyInfo->getPlayerInfoMap();
	PlayerInfoMap::iterator it;
	BuddyInfoMap *buddies = TheGameSpyInfo->getBuddyMap();
	BuddyInfoMap::iterator bIt;
	if (listboxLobbyPlayers)
	{
		// save off old selection
		Int maxSelectedItems = GadgetListBoxGetNumEntries(listboxLobbyPlayers);
		Int *selectedIndices;
		GadgetListBoxGetSelected(listboxLobbyPlayers, (Int *)(&selectedIndices));
		std::set<AsciiString> selectedNames;
		std::set<AsciiString>::const_iterator selIt;
		std::set<Int> indicesToSelect;
		UnicodeString uStr;
		Int numSelected = 0;
		for (Int i=0; i<maxSelectedItems; ++i)
		{
			if (selectedIndices[i] < 0)
			{
				break;
			}
			++numSelected;
			AsciiString selectedName;
			uStr = GadgetListBoxGetText(listboxLobbyPlayers, selectedIndices[i], 2);
			selectedName.translate(uStr);
			selectedNames.insert(selectedName);
			DEBUG_LOG(("Saving off old selection %d (%s)", selectedIndices[i], selectedName.str()));
		}

		// save off old top entry
		Int previousTopIndex = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);

		GadgetListBoxReset(listboxLobbyPlayers);

		// Ops
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			if (info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID))
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_OWNER]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Buddies
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt != buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_BUDDY]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// Everyone else
		for (it = players->begin(); it != players->end(); ++it)
		{
			PlayerInfo info = it->second;
			bIt = buddies->find(info.m_profileID);
			if ( !(info.m_flags & PEER_FLAG_OP || TheGameSpyConfig->isPlayerVIP(info.m_profileID)) && bIt == buddies->end() )
			{
				Int index = insertPlayerInListbox(info, info.isIgnored()?GameSpyColor[GSCOLOR_PLAYER_IGNORED]:GameSpyColor[GSCOLOR_PLAYER_NORMAL]);

				selIt = selectedNames.find(info.m_name);
				if (selIt != selectedNames.end())
				{
					DEBUG_LOG(("Marking index %d (%s) to re-select", index, info.m_name.str()));
					indicesToSelect.insert(index);
				}
			}
		}

		// restore selection
		if (indicesToSelect.size())
		{
			std::set<Int>::const_iterator indexIt = indicesToSelect.begin();
			const size_t count = indicesToSelect.size();
			size_t index = 0;
			Int *newIndices = NEW Int[count];
			while (index < count)
			{
				newIndices[index] = *indexIt;
				DEBUG_LOG(("Queueing up index %d to re-select", *indexIt));
				++index;
				++indexIt;
			}
			GadgetListBoxSetSelected(listboxLobbyPlayers, newIndices, count);
			delete[] newIndices;
		}

		if (indicesToSelect.size() != numSelected)
		{
			TheWindowManager->winSetLoneWindow(NULL);
		}

		// restore top visible entry
		GadgetListBoxSetTopVisibleEntry(listboxLobbyPlayers, previousTopIndex);
	}

}

void NGMP_WOLLobbyMenu_CreateLobbyCallback(bool bSuccess)
{
	// TODO_NGMP: Handle error case

	buttonPushed = true;
	nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
	TheShell->pop();
	//TheGameSpyInfo->markAsStagingRoomHost();
	//TheGameSpyInfo->setGameOptions();
}

void NGMP_WOLLobbyMenu_JoinLobbyCallback(EJoinLobbyResult result)
{
	// TODO_NGMP: Show accurate errors again

	SetLobbyAttemptHostJoin(FALSE);
	if (result == EJoinLobbyResult::JoinLobbyResult_Success)
	{
		// Woohoo!  On to our next screen!
		buttonPushed = true;
		nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
		TheShell->pop();
	}
	else
	{
		UnicodeString s;

		switch (result)
		{
		case EJoinLobbyResult::JoinLobbyResult_FullRoom:        // The room is full.
			s = TheGameText->fetch("GUI:JoinFailedRoomFull");
			break;

		// NOTE: Commented out ones are no longer supported. Seems like these we GS concepts but not part of the game
		/*
		case PEERInviteOnlyRoom:  // The room is invite only.
			s = TheGameText->fetch("GUI:JoinFailedInviteOnly");
			break;
		case PEERBannedFromRoom:  // The local user is banned from the room.
			s = TheGameText->fetch("GUI:JoinFailedBannedFromRoom");
			break;
			*/
		case EJoinLobbyResult::JoinLobbyResult_BadPassword:     // An incorrect password (or none) was given for a passworded room.
			s = TheGameText->fetch("GUI:JoinFailedBadPassword");
			break;
		/*
		case PEERAlreadyInRoom:   // The local user is already in or entering a room of the same type.
			s = TheGameText->fetch("GUI:JoinFailedAlreadyInRoom");
			break;
		case PEERNoConnection:    // Can't join a room if there's no chat connection.
			s = TheGameText->fetch("GUI:JoinFailedNoConnection");
			break;
			*/
		default:
			s = TheGameText->fetch("GUI:JoinFailedDefault");
			break;
		}

		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), s);

		// NGMP: We don't need to do this anymore, the service does it for us
		/*
		if (groupRoomToJoin)
		{
			DEBUG_LOG(("WOLLobbyMenuUpdate() - rejoining group room %d\n", groupRoomToJoin));
			TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
			groupRoomToJoin = 0;
		}
		else
		{
			DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room\n"));
			TheGameSpyInfo->joinBestGroupRoom();
		}
		*/
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the WOL Lobby Menu */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuInit( WindowLayout *layout, void *userData )
{
	// for safety (and sanity)
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->LeaveCurrentLobby();

	// TODO_NGMP: impl this again
	GameWindow* buttonBuddy = TheWindowManager->winGetWindowFromId(NULL, NAMEKEY("WOLCustomLobby.wnd:ButtonBuddy"));
	if (buttonBuddy)
		buttonBuddy->winEnable(FALSE);

	nextScreen = NULL;
	buttonPushed = false;
	isShuttingDown = false;

	SetLobbyAttemptHostJoin(FALSE); // not trying to host or join

	gameListRefreshTime = 0;
	playerListRefreshTime = 0;

	parentWOLLobbyID = TheNameKeyGenerator->nameToKey( AsciiString( "WOLCustomLobby.wnd:WOLLobbyMenuParent" ) );
	parent = TheWindowManager->winGetWindowFromId(NULL, parentWOLLobbyID);

	buttonBackID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonBack"));
	buttonBack = TheWindowManager->winGetWindowFromId(parent, buttonBackID);

	buttonHostID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonHost"));
	buttonHost = TheWindowManager->winGetWindowFromId(parent, buttonHostID);

	buttonRefreshID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonRefresh"));
	buttonRefresh = TheWindowManager->winGetWindowFromId(parent, buttonRefreshID);

	// GENERALS_ONLINE: Disable refresh button, we refresh via push now, this button isn't necessary
	//buttonRefresh->winHide(TRUE);

	buttonJoinID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonJoin"));
	buttonJoin = TheWindowManager->winGetWindowFromId(parent, buttonJoinID);
	buttonJoin->winEnable(FALSE);

	buttonBuddyID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonBuddy"));
	buttonBuddy = TheWindowManager->winGetWindowFromId(parent, buttonBuddyID);

	buttonEmoteID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ButtonEmote"));
	buttonEmote = TheWindowManager->winGetWindowFromId(parent, buttonEmoteID);

	textEntryChatID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:TextEntryChat"));
	textEntryChat = TheWindowManager->winGetWindowFromId(parent, textEntryChatID);

	listboxLobbyPlayersID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ListboxPlayers"));
	listboxLobbyPlayers = TheWindowManager->winGetWindowFromId(parent, listboxLobbyPlayersID);
	listboxLobbyPlayers->winSetTooltipFunc(playerTooltip);

	listboxLobbyChatID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ListboxChat"));
	listboxLobbyChat = TheWindowManager->winGetWindowFromId(parent, listboxLobbyChatID);

	// TODO_NGMP
	//TheGameSpyInfo->registerTextWindow(listboxLobbyChat);

	comboLobbyGroupRoomsID = TheNameKeyGenerator->nameToKey(AsciiString("WOLCustomLobby.wnd:ComboBoxGroupRooms"));
	comboLobbyGroupRooms = TheWindowManager->winGetWindowFromId(parent, comboLobbyGroupRoomsID);

	//GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Welcome to Generals Online for Zero Hour!"), GameMakeColor(255, 194, 15, 255), -1, -1);

	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	populateGroupRoomListbox(comboLobbyGroupRooms);

	// Show Menu
	layout->hide( FALSE );

	// if we're not in a room, this will join the best available one
	// TODO_NGMP
	/*
	if (!TheGameSpyInfo->getCurrentGroupRoom())
	{
		if (groupRoomToJoin)
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - rejoining group room %d", groupRoomToJoin));
			TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
			groupRoomToJoin = 0;
		}
		else
		{
			DEBUG_LOG(("WOLLobbyMenuInit() - joining best group room"));
			TheGameSpyInfo->joinBestGroupRoom();
		}
	}
	else
	{
		DEBUG_LOG(("WOLLobbyMenuInit() - not joining group room because we're already in one"));
	}
	*/

	// NGMP: Register for create lobby callback
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForCreateLobbyCallback(NGMP_WOLLobbyMenu_CreateLobbyCallback);

	// NGMP: Join lobby callback
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->RegisterForJoinLobbyCallback(NGMP_WOLLobbyMenu_JoinLobbyCallback);

	// NGMP: Request lobbies
	
	//GadgetListBoxSetItemData(listboxLobbyChat, (void*)-1, index);
	
	// TODO_NGMP: player list change callbacks
	
	// register for chat events
	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->RegisterForChatCallback([](UnicodeString strMessage, GameSpyColors color)
		{
			GadgetListBoxAddEntryText(listboxLobbyChat, strMessage, GameSpyColor[color], -1, -1);
		});

	// register for roster events
	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->RegisterForRosterNeedsRefreshCallback([]()
		{
			refreshPlayerList(true);
		});

	GrabWindowInfo();

	// TODO_NGMP
	//TheGameSpyInfo->clearStagingRoomList();

	// TODO_NGMP
	/*
	PeerRequest req;
	req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
	req.gameList.restrictGameList = TheGameSpyConfig->restrictGamesToLobby();
	TheGameSpyPeerMessageQueue->addRequest(req);
	*/

	// animate controls
//	TheShell->registerWithAnimateManager(parent, WIN_ANIMATION_SLIDE_TOP, TRUE);
	TheShell->showShellMap(TRUE);


#if !defined(GENERALS_ONLINE)
	TheGameSpyGame->reset();
#else
	if (TheNGMPGame != nullptr)
	{
		TheNGMPGame->reset();
	}
#endif
	
	// TODO_NGMP
	//CustomMatchPreferences pref;
//	GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//	if (slider)
//	{
//		GadgetSliderSetPosition(slider, pref.getChatSizeSlider());
//		doSliderTrack(slider, pref.getChatSizeSlider());
//	}
//

	// TODO_NGMP
	/*
	if (pref.usesLongGameList())
	{
		ToggleGameListType();
	}
	*/

	// Set Keyboard to chat window
	TheWindowManager->winSetFocus( textEntryChat );
	raiseMessageBoxes = true;

	TheLobbyQueuedUTMs.clear();
	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(NULL, TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);
	DontShowMainMenu = TRUE;

	// upon entry, retrieve room list

	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetRoomList([=]()
		{
			// attempt to join the first room
			NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->JoinRoom(0, []()
				{
					//GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Attempting to join room"), GameMakeColor(255, 194, 15, 255), -1, -1);
				},
				[]()
				{
					GadgetListBoxReset(listboxLobbyChat);

					UnicodeString msg;
					msg.format(TheGameText->fetch("GUI:LobbyJoined"), NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().at(0).GetRoomDisplayName().str());
					GadgetListBoxAddEntryText(listboxLobbyChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);

					// process flag related info messages
					ERoomFlags flags = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().at(0).GetRoomFlags();
					if (flags == ERoomFlags::ROOM_FLAGS_SHOW_ALL_MATCHES)
					{
						GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"\t INFO: This is a special room where the lobby list shows lobbies from ALL rooms, not just the current room, to make it easier to find a match without room hopping."), GameMakeColor(255, 194, 15, 255), -1, -1);
						GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"\t INFO: Lobbies created here will only show in here. The members list & chat will also only show players in this room."), GameMakeColor(255, 194, 15, 255), -1, -1);
					}

					// refresh on join
					refreshPlayerList(TRUE);

					RefreshGameListBoxes();

					populateGroupRoomListbox(comboLobbyGroupRooms);
				});
		});
} // WOLLobbyMenuInit

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
		TheShell->push(nextScreen);
	}

	nextScreen = NULL;

}  // end if

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->DeregisterForCreateLobbyCallback();
	NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->DeregisterForJoinLobbyCallback();
	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->DeregisterForChatCallback();
	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->DeregisterForRosterNeedsRefreshCallback();

	CustomMatchPreferences pref;
//	GameWindow *slider = TheWindowManager->winGetWindowFromId(parent, sliderChatAdjustID);
//	if (slider)
//	{
//		pref.setChatSizeSlider(GadgetSliderGetPosition(slider));
//	}
	if (GetGameInfoListBox())
	{
		pref.setUsesLongGameList(FALSE);
	}
	else
	{
		pref.setUsesLongGameList(TRUE);
	}
	pref.write();

	ReleaseWindowInfo();

	// TODO_NGMP
	//TheGameSpyInfo->unregisterTextWindow(listboxLobbyChat);

	//TheGameSpyChat->stopListingGames();
	// TODO_NGMP
	//PeerRequest req;
	//req.peerRequestType = PeerRequest::PEERREQUEST_STOPGAMELIST;
	//TheGameSpyPeerMessageQueue->addRequest(req);

	listboxLobbyChat = NULL;
	listboxLobbyPlayers = NULL;

	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}  //end if

	TheShell->reverseAnimatewindow();
	DontShowMainMenu = FALSE;

	RaiseGSMessageBox();
	TheTransitionHandler->reverse("WOLCustomLobbyFade");

}  // WOLLobbyMenuShutdown

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

#ifdef PERF_TEST
static const char* getMessageString(Int t)
{
	switch(t)
	{
		case PeerResponse::PEERRESPONSE_LOGIN:
			return "login";
		case PeerResponse::PEERRESPONSE_DISCONNECT:
			return "disconnect";
		case PeerResponse::PEERRESPONSE_MESSAGE:
			return "message";
		case PeerResponse::PEERRESPONSE_GROUPROOM:
			return "group room";
		case PeerResponse::PEERRESPONSE_STAGINGROOM:
			return "staging room";
		case PeerResponse::PEERRESPONSE_STAGINGROOMPLAYERINFO:
			return "staging room player info";
		case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
			return "group room join";
		case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
			return "staging room create";
		case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
			return "staging room join";
		case PeerResponse::PEERRESPONSE_PLAYERJOIN:
			return "player join";
		case PeerResponse::PEERRESPONSE_PLAYERLEFT:
			return "player part";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
			return "player nick";
		case PeerResponse::PEERRESPONSE_PLAYERINFO:
			return "player info";
		case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
			return "player flags";
		case PeerResponse::PEERRESPONSE_ROOMUTM:
			return "room UTM";
		case PeerResponse::PEERRESPONSE_PLAYERUTM:
			return "player UTM";
		case PeerResponse::PEERRESPONSE_QUICKMATCHSTATUS:
			return "QM status";
		case PeerResponse::PEERRESPONSE_GAMESTART:
			return "game start";
		case PeerResponse::PEERRESPONSE_FAILEDTOHOST:
			return "host failure";
	}
	return "unknown";
}
#endif // PERF_TEST

//-------------------------------------------------------------------------------------------------
/** refreshGameList 
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
static void refreshGameList( Bool forceRefresh )
{
	// TODO_NGMP: rate limit this like before
	//RefreshGameListBoxes();

	Int refreshInterval = gameListRefreshInterval;

	if (forceRefresh || ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime())))
	{
#if defined(GENERALS_ONLINE)
		RefreshGameListBoxes();
		gameListRefreshTime = timeGetTime();
#else
		if (TheGameSpyInfo->hasStagingRoomListChanged())
		{
			//DEBUG_LOG(("################### refreshing game list"));
			//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
			RefreshGameListBoxes();
			gameListRefreshTime = timeGetTime();
		} else {
			//DEBUG_LOG(("-"));
		}
#endif
	} else {
		//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d"));
	}
}
//-------------------------------------------------------------------------------------------------
/** refreshPlayerList 
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
static void refreshPlayerList( Bool forceRefresh )
{
		Int refreshInterval = playerListRefreshInterval;

		if (forceRefresh ||((playerListRefreshTime == 0) || ((playerListRefreshTime + refreshInterval) <= timeGetTime())))
		{
				PopulateLobbyPlayerListbox();
				playerListRefreshTime = timeGetTime();
		}
}

void ExitState()
{
	if (s_tryingToHostOrJoin)
		return;

	// Leave any group room, then pop off the screen
	NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->LeaveRoom();

	SetLobbyAttemptHostJoin(TRUE); // pretend, since we don't want to queue up another action
	buttonPushed = true;

	if (NGMP_OnlineServicesManager::GetInstance()->IsPendingFullTeardown()) // go back to the front end
	{
		nextScreen = nullptr;
	}
	else // user backed out, go back to welcome menu
	{
		nextScreen = "Menus/WOLWelcomeMenu.wnd";
	}
	
	TheShell->pop();
}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu update method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuUpdate( WindowLayout * layout, void *userData)
{
	// need to exit?
	if (NGMP_OnlineServicesManager::GetInstance() != nullptr && NGMP_OnlineServicesManager::GetInstance()->IsPendingFullTeardown())
	{
		if (!s_tryingToHostOrJoin)
		{
			s_tryingToHostOrJoin = false;
			ExitState();
			TearDownGeneralsOnline();
		}		

		return;
	}

	if(justEntered)
	{
		if(initialGadgetDelay == 1)
		{
			TheTransitionHandler->remove("MainMenuDefaultMenuLogoFade");
			TheTransitionHandler->setGroup("WOLCustomLobbyFade");
			initialGadgetDelay = 2;
			justEntered = FALSE;
		}
		else
			initialGadgetDelay--;
	}
	if (TheGameLogic->isInShellGame() && TheGameLogic->getFrame() == 1)
	{
		SignalUIInteraction(SHELL_SCRIPT_HOOK_GENERALS_ONLINE_ENTERED_FROM_GAME);
	}

	
	// We'll only be successful if we've requested to 
	if(isShuttingDown && TheShell->isAnimFinished() && TheTransitionHandler->isFinished())
		shutdownComplete(layout);

	if (raiseMessageBoxes)
	{
		RaiseGSMessageBox();
		raiseMessageBoxes = false;
	}
	
	// do we need to update?
	if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface();
		if (pLobbyInterface != nullptr && pLobbyInterface->IsLobbyListDirty() && !isShuttingDown && !buttonPushed && !pLobbyInterface->IsInLobby() && pLobbyInterface->GetLobbyTryingToJoin().lobbyID == -1)
		{
			const bool bShouldAutoRefresh = true;

			pLobbyInterface->ConsumeLobbyListDirtyFlag();

			if (bShouldAutoRefresh)
			{
				refreshGameList(true);
			}
			else
			{
				GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Your lobby list is outdated. Hit refresh to see the latest servers."), GameMakeColor(255, 194, 15, 255), -1, -1);
			}

		}
	}

	if (TheShell->isAnimFinished() && TheTransitionHandler->isFinished() && !buttonPushed && TheGameSpyPeerMessageQueue)
	{
		HandleBuddyResponses();
		HandlePersistentStorageResponses();

#ifdef PERF_TEST
		UnsignedInt start = timeGetTime();
		UnsignedInt end = timeGetTime();
		std::list<Int> responses;
		Int numMessages = 0;
#endif // PERF_TEST

		Int allowedMessages = TheGameSpyInfo->getMaxMessagesPerUpdate();
		Bool sawImportantMessage = FALSE;
		Bool shouldRepopulatePlayers = FALSE;
		PeerResponse resp;
		while (allowedMessages-- && !sawImportantMessage && TheGameSpyPeerMessageQueue->getResponse( resp ))
		{
#ifdef PERF_TEST
			++numMessages;
			responses.push_back(resp.peerResponseType);
#endif // PERF_TEST
			switch (resp.peerResponseType)
			{
			case PeerResponse::PEERRESPONSE_JOINGROUPROOM:
				sawImportantMessage = TRUE;
				if (resp.joinGroupRoom.ok)
				{
					//buttonPushed = true;
					TheGameSpyInfo->setCurrentGroupRoom(resp.joinGroupRoom.id);
					TheGameSpyInfo->getPlayerInfoMap()->clear();
					GroupRoomMap::iterator iter = TheGameSpyInfo->getGroupRoomList()->find(resp.joinGroupRoom.id);
					if (iter != TheGameSpyInfo->getGroupRoomList()->end())
					{
						GameSpyGroupRoom room = iter->second;
						UnicodeString msg;
						msg.format(TheGameText->fetch("GUI:LobbyJoined"), room.m_translatedName.str());
						TheGameSpyInfo->addText(msg, GameSpyColor[GSCOLOR_DEFAULT], NULL);
					}
				}
				else
				{
					DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
					TheGameSpyInfo->joinBestGroupRoom();
				}
				populateGroupRoomListbox(comboLobbyGroupRooms);
				shouldRepopulatePlayers = TRUE;
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDFLAGS:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERCHANGEDNICK:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERINFO:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->updatePlayerInfo(p);
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERJOIN:
				{
					if (resp.player.roomType == GroupRoom)
					{
						PlayerInfo p;
						fillPlayerInfo(&resp, &p);
						TheGameSpyInfo->updatePlayerInfo(p);
						shouldRepopulatePlayers = TRUE;
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERUTM:
			case PeerResponse::PEERRESPONSE_ROOMUTM:
				{
					DEBUG_LOG(("Putting off a UTM in the lobby"));
					TheLobbyQueuedUTMs.push_back(resp);
				}
				break;
			case PeerResponse::PEERRESPONSE_PLAYERLEFT:
				{
					PlayerInfo p;
					fillPlayerInfo(&resp, &p);
					TheGameSpyInfo->playerLeftGroupRoom(resp.nick.c_str());
					shouldRepopulatePlayers = TRUE;
				}
				break;
			case PeerResponse::PEERRESPONSE_MESSAGE:
				{
					TheGameSpyInfo->addChat(resp.nick.c_str(), resp.message.profileID,
						UnicodeString(resp.text.c_str()), !resp.message.isPrivate, resp.message.isAction, listboxLobbyChat);
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
				break;
			case PeerResponse::PEERRESPONSE_CREATESTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					if (resp.createStagingRoom.result == PEERJoinSuccess)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
						TheGameSpyInfo->markAsStagingRoomHost();
						TheGameSpyInfo->setGameOptions();
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_JOINSTAGINGROOM:
				{
					sawImportantMessage = TRUE;
					SetLobbyAttemptHostJoin(FALSE);
					Bool isHostPresent = TRUE;
					if (resp.joinStagingRoom.ok == PEERTrue)
					{
						GameSpyStagingRoom *room = TheGameSpyInfo->getCurrentStagingRoom();
						if (!room)
						{
							isHostPresent = FALSE;
						}
						else
						{
							isHostPresent = FALSE;
							for (Int i=0; i<MAX_SLOTS; ++i)
							{
								AsciiString hostName;
								hostName.translate(room->getConstSlot(0)->getName());
								const char *firstPlayer = resp.stagingRoomPlayerNames[i].c_str();
								if (!strcmp(hostName.str(), firstPlayer))
								{
									DEBUG_LOG(("Saw host %s == %s in slot %d", hostName.str(), firstPlayer, i));
									isHostPresent = TRUE;
								}
							}
						}
					}
					if (resp.joinStagingRoom.ok == PEERTrue && isHostPresent)
					{
						// Woohoo!  On to our next screen!
						buttonPushed = true;
						nextScreen = "Menus/GameSpyGameOptionsMenu.wnd";
						TheShell->pop();
					}
					else
					{
						UnicodeString s;

						switch(resp.joinStagingRoom.result)
						{
						case PEERFullRoom:        // The room is full.
							s = TheGameText->fetch("GUI:JoinFailedRoomFull");
							break;
						case PEERInviteOnlyRoom:  // The room is invite only.
							s = TheGameText->fetch("GUI:JoinFailedInviteOnly");
							break;
						case PEERBannedFromRoom:  // The local user is banned from the room.
							s = TheGameText->fetch("GUI:JoinFailedBannedFromRoom");
							break;
						case PEERBadPassword:     // An incorrect password (or none) was given for a passworded room.
							s = TheGameText->fetch("GUI:JoinFailedBadPassword");
							break;
						case PEERAlreadyInRoom:   // The local user is already in or entering a room of the same type.
							s = TheGameText->fetch("GUI:JoinFailedAlreadyInRoom");
							break;
						case PEERNoConnection:    // Can't join a room if there's no chat connection.
							s = TheGameText->fetch("GUI:JoinFailedNoConnection");
							break;
						default:
							s = TheGameText->fetch("GUI:JoinFailedDefault");
							break;
						}
						GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), s);
						if (groupRoomToJoin)
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - rejoining group room %d", groupRoomToJoin));
							TheGameSpyInfo->joinGroupRoom(groupRoomToJoin);
							groupRoomToJoin = 0;
						}
						else
						{
							DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
							TheGameSpyInfo->joinBestGroupRoom();
						}
					}
				}
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOMLISTCOMPLETE:
				TheGameSpyInfo->sawFullGameList();
				break;
			case PeerResponse::PEERRESPONSE_STAGINGROOM:
				{
					GameSpyStagingRoom room;
					switch(resp.stagingRoom.action)
					{
					case PEER_CLEAR:
						TheGameSpyInfo->clearStagingRoomList();
						//TheGameSpyInfo->addText( UnicodeString(L"gameList: PEER_CLEAR"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					case PEER_ADD:
					case PEER_UPDATE:
					{
						if (resp.stagingRoom.percentComplete == 100)
						{
							TheGameSpyInfo->sawFullGameList();
						}

						//if (ParseAsciiStringToGameInfo(&room, resp.stagingRoomMapName.c_str()))
						//if (ParseAsciiStringToGameInfo(&room, resp.stagingServerGameOptions.c_str()))
						Bool serverOk = TRUE;
						if (!resp.stagingRoomMapName.length())
						{
							serverOk = FALSE;
						}
						// fix for ghost game problem - need to iterate over all resp.stagingRoomPlayerNames[i]
						Bool sawSelf = FALSE;
						//for (Int i=0; i<MAX_SLOTS; ++i)
						//{
							if (TheGameSpyInfo->getLocalName() == resp.stagingRoomPlayerNames[0].c_str())
							{
								sawSelf = TRUE; // don't show ghost games for myself
							}
						//}
						if (sawSelf)
							serverOk = FALSE;

						if (serverOk)
						{
							room.setGameName(UnicodeString(resp.stagingServerName.c_str()));
							room.setID(resp.stagingRoom.id);
							room.setHasPassword(resp.stagingRoom.requiresPassword);
							room.setVersion(resp.stagingRoom.version);
							room.setExeCRC(resp.stagingRoom.exeCRC);
							room.setIniCRC(resp.stagingRoom.iniCRC);
							room.setAllowObservers(resp.stagingRoom.allowObservers);
              room.setUseStats(resp.stagingRoom.useStats);
							room.setPingString(resp.stagingServerPingString.c_str());
							room.setLadderIP(resp.stagingServerLadderIP.c_str());
							room.setLadderPort(resp.stagingRoom.ladderPort);
							room.setReportedNumPlayers(resp.stagingRoom.numPlayers);
							room.setReportedMaxPlayers(resp.stagingRoom.maxPlayers);
							room.setReportedNumObservers(resp.stagingRoom.numObservers);

							Int i;
							AsciiString gsMapName = resp.stagingRoomMapName.c_str();
							AsciiString mapName = "";
							for (i=0; i<gsMapName.getLength(); ++i)
							{
								char c = gsMapName.getCharAt(i);
								if (c != '/')
									mapName.concat(c);
								else
									mapName.concat('\\');
							}
							room.setMap(TheGameState->portableMapPathToRealMapPath(mapName));

							Int numPlayers = 0;
							for (i=0; i<MAX_SLOTS; ++i)
							{
								GameSpyGameSlot *slot = room.getGameSpySlot(i);
								if (slot)
								{
									slot->setWins( resp.stagingRoom.wins[i] );
									slot->setLosses( resp.stagingRoom.losses[i] );
									slot->setProfileID( resp.stagingRoom.profileID[i] );
									slot->setPlayerTemplate( resp.stagingRoom.faction[i] );
									slot->setColor( resp.stagingRoom.color[i] );
									if (resp.stagingRoom.profileID[i] == SLOT_EASY_AI)
									{
										slot->setState(SLOT_EASY_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_MED_AI)
									{
										slot->setState(SLOT_MED_AI);
										++numPlayers;
									}
									else if (resp.stagingRoom.profileID[i] == SLOT_BRUTAL_AI)
									{
										slot->setState(SLOT_BRUTAL_AI);
										++numPlayers;
									}
									else if (resp.stagingRoomPlayerNames[i].length())
									{
										UnicodeString nameUStr;
										nameUStr.translate(resp.stagingRoomPlayerNames[i].c_str());
										slot->setState(SLOT_PLAYER, nameUStr);
										++numPlayers;
									}
									else
									{
										slot->setState(SLOT_OPEN);
									}
								}
							}
							DEBUG_ASSERTCRASH(numPlayers, ("Game had no players!"));
							//DEBUG_LOG(("Saw room: hasPass=%d, allowsObservers=%d", room.getHasPassword(), room.getAllowObservers()));
							if (resp.stagingRoom.action == PEER_ADD)
							{
								TheGameSpyInfo->addStagingRoom(room);
								//TheGameSpyInfo->addText( UnicodeString(L"gameList: PEER_ADD"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
							else
							{
								TheGameSpyInfo->updateStagingRoom(room);
								//TheGameSpyInfo->addText( UnicodeString(L"gameList: PEER_UPDATE"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
						}
						else
						{
							room.setID(resp.stagingRoom.id);
							TheGameSpyInfo->removeStagingRoom(room);
							//TheGameSpyInfo->addText( UnicodeString(L"gameList: PEER_UPDATE FAILED"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						}
						break;
					}
					case PEER_REMOVE:
						room.setID(resp.stagingRoom.id);
						TheGameSpyInfo->removeStagingRoom(room);
						//TheGameSpyInfo->addText( UnicodeString(L"gameList: PEER_REMOVE"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					default:
						//TheGameSpyInfo->addText( UnicodeString(L"gameList: Unknown"), GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					}
				}
				break;
			}
		}
#if 0
		if (shouldRepopulatePlayers)
		{
			PopulateLobbyPlayerListbox();
		}
#else
		refreshPlayerList();
#endif

#ifdef PERF_TEST
		// check performance
		end = timeGetTime();
		PERF_LOG(("Frame time was %d ms", end-start));
		std::list<Int>::const_iterator it;
		for (it = responses.begin(); it != responses.end(); ++it)
		{
			PERF_LOG(("  %s", getMessageString(*it)));
		}
		PERF_LOG((""));
#endif // PERF_TEST

#if 0
// Removed 2-17-03 to pull out into a function so we can do the same checks 
		Int refreshInterval = gameListRefreshInterval;

		if ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime()))
		{
			if (TheGameSpyInfo->hasStagingRoomListChanged())
			{
				//DEBUG_LOG(("################### refreshing game list"));
				//DEBUG_LOG(("gameRefreshTime=%d, refreshInterval=%d, now=%d", gameListRefreshTime, refreshInterval, timeGetTime()));
				RefreshGameListBoxes();
				gameListRefreshTime = timeGetTime();
			} else {
				//DEBUG_LOG(("-"));
			}
		} else {
			//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d"));
		}
#else
	refreshGameList();
#endif
	}
}// WOLLobbyMenuUpdate

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg ) 
	{

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

	return MSG_IGNORED;
}// WOLLobbyMenuInput

//static void doSliderTrack(GameWindow *control, Int val)
//{
//	Int sliderW, sliderH, sliderX, sliderY;
//	control->winGetPosition(&sliderX, &sliderY);
//	control->winGetSize(&sliderW, &sliderH);
//	Real cursorY = sliderY + (100-val)*0.01f*sliderH;
//
//	extern GameWindow *listboxLobbyGamesSmall;
//	extern GameWindow *listboxLobbyGamesLarge;
//	extern GameWindow *listboxLobbyGameInfo;
//
//	static Int gwsX = 0, gwsY = 0, gwsW = 0, gwsH = 0;
//	static Int gwlX = 0, gwlY = 0, gwlW = 0, gwlH = 0;
//	static Int gwiX = 0, gwiY = 0, gwiW = 0, gwiH = 0;
//	static Int pwX = 0, pwY = 0, pwW = 0, pwH = 0;
//	static Int chatPosX = 0, chatPosY = 0, chatW = 0, chatH = 0;
//	static Int spacing = 0;
//	if (chatPosX == 0)
//	{
//		listboxLobbyChat->winGetPosition(&chatPosX, &chatPosY);
//		listboxLobbyChat->winGetSize(&chatW, &chatH);
//
////		listboxLobbyGamesSmall->winGetPosition(&gwsX, &gwsY);
////		listboxLobbyGamesSmall->winGetSize(&gwsW, &gwsH);
//
//		listboxLobbyGamesLarge->winGetPosition(&gwlX, &gwlY);
//		listboxLobbyGamesLarge->winGetSize(&gwlW, &gwlH);
//
////		listboxLobbyGameInfo->winGetPosition(&gwiX, &gwiY);
////		listboxLobbyGameInfo->winGetSize(&gwiW, &gwiH);
////
//		listboxLobbyPlayers->winGetPosition(&pwX, &pwY);
//		listboxLobbyPlayers->winGetSize(&pwW, &pwH);
//
//		spacing = chatPosY - pwY - pwH;
//	}
//
//	Int newChatY = cursorY;
//	Int newChatH = chatH + chatPosY - newChatY;
//	listboxLobbyChat->winSetPosition(chatPosX, newChatY);
//	listboxLobbyChat->winSetSize(chatW, newChatH);
//
//	Int newH = cursorY - pwY - spacing;
//	listboxLobbyPlayers->winSetSize(pwW, newH);
////	listboxLobbyGamesSmall->winSetSize(gwsW, newH);
//	listboxLobbyGamesLarge->winSetSize(gwlW, newH);
////	listboxLobbyGameInfo->winSetSize(gwiW, newH);


//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType WOLLobbyMenuSystem( GameWindow *window, UnsignedInt msg, 
														 WindowMsgData mData1, WindowMsgData mData2 )
{
	UnicodeString txtInput;
	static NameKeyType buttonGameListTypeToggleID = NAMEKEY_INVALID;

	switch( msg )
	{
		
		
		//---------------------------------------------------------------------------------------------
		case GWM_CREATE:
			{
				buttonGameListTypeToggleID = NAMEKEY("WOLCustomLobby.wnd:ButtonGameListToggle");
//				sliderChatAdjustID = NAMEKEY("WOLCustomLobby.wnd:SliderChatAdjust");
				
				break;
			} // case GWM_DESTROY:

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				break;
			} // case GWM_DESTROY:

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{	
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}//case GWM_INPUT_FOCUS:

		//---------------------------------------------------------------------------------------------
		case GLM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if ( controlID == GetGameListBoxID() )
				{
					int rowSelected = mData2;
					if( rowSelected >= 0 )
					{
						buttonJoin->winEnable(TRUE);
						static UnsignedInt lastFrame = 0;
						static Int lastID = -1;
						UnsignedInt now = TheGameClient->getFrame();

						PeerRequest req;
						req.peerRequestType = PeerRequest::PEERREQUEST_GETEXTENDEDSTAGINGROOMINFO;
						req.stagingRoom.id = (Int)GadgetListBoxGetItemData(control, rowSelected, 0);

						if (lastID != req.stagingRoom.id || now > lastFrame + 60)
						{
							// TODO_NGMP: Impl this again
							/*
							TheGameSpyPeerMessageQueue->addRequest(req);
							*/
						}
						
						lastID = req.stagingRoom.id;
						lastFrame = now;
					}
					else
					{
						buttonJoin->winEnable(FALSE);
					}
					if (GetGameInfoListBox())
					{
						RefreshGameInfoListBox(GetGameListBox(), GetGameInfoListBox());
					}
				} //if ( controlID == GetGameListBoxID() )

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (HandleSortButton((NameKeyType)controlID))
					break;

				// If we back out, just bail - we haven't gotten far enough to need to log out
				if ( controlID == buttonBackID )
				{
					ExitState();

				} //if ( controlID == buttonBack )
				else if ( controlID == buttonRefreshID )
				{
					// Added 2/17/03 added the game refresh button
					refreshGameList(TRUE);	
					refreshPlayerList(TRUE);
				}
				else if ( controlID == buttonHostID )
				{
					if (s_tryingToHostOrJoin)
						break;

					SetLobbyAttemptHostJoin( TRUE );
					TheLobbyQueuedUTMs.clear();
					// TODO_NGMP
					//groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					GameSpyOpenOverlay(GSOVERLAY_GAMEOPTIONS);
				}
				else if ( controlID == buttonJoinID )
				{
					// TODO_NGMP: Support re-ordering again
					Int selected;
					GadgetListBoxGetSelected(GetGameListBox(), &selected);
					if (selected >= 0)
					{
						//Int selectedID = (Int)GadgetListBoxGetItemData(GetGameListBox(), selected);
						//if (selectedID > 0)
						{
							auto Lobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetLobbyFromIndex(selected);

							// CRC Check
							if (Lobby.exe_crc != TheGlobalData->m_exeCRC || Lobby.ini_crc != TheGlobalData->m_iniCRC)
							{
								if (TheGlobalData->m_iniCRC != VANILLA_INI_CRC)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), UnicodeString(L"You have modified INI files or a modification."));
								}
								else if (Lobby.ini_crc != VANILLA_INI_CRC)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), UnicodeString(L"The host has modified INI files or a modification."));
								}
								else
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
								}
								break;
							}

							// TODO_NGMP: Enforce this on the host too, vanilla game did not...

							
							NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SetLobbyTryingToJoin(Lobby);

							if (Lobby.passworded)
							{
								GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
							}
							else
							{
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->JoinLobby(selected, nullptr);

								SetLobbyAttemptHostJoin(TRUE);
							}
						}
					}
					else
					{
						GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), NULL);
					}
					// TODO_NGMP: Start using StagingRoomInfo again, it'll make this easier and cleaner
					/*
					if (s_tryingToHostOrJoin)
						break;

					TheLobbyQueuedUTMs.clear();
					// Look for a game to join
					groupRoomToJoin = TheGameSpyInfo->getCurrentGroupRoom();
					Int selected;
					GadgetListBoxGetSelected(GetGameListBox(), &selected);
					if (selected >= 0)
					{
						Int selectedID = (Int)GadgetListBoxGetItemData(GetGameListBox(), selected);
						if (selectedID > 0)
						{
							StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
							StagingRoomMap::iterator srmIt = srm->find(selectedID);
							if (srmIt != srm->end())
							{
								GameSpyStagingRoom *roomToJoin = srmIt->second;
								if (!roomToJoin || roomToJoin->getExeCRC() != TheGlobalData->m_exeCRC || roomToJoin->getIniCRC() != TheGlobalData->m_iniCRC)
								{
									// bad crc.  don't go.
									DEBUG_LOG(("WOLLobbyMenuSystem - CRC mismatch with the game I'm trying to join. My CRC's - EXE:0x%08X INI:0x%08X  Their CRC's - EXE:0x%08x INI:0x%08x", TheGlobalData->m_exeCRC, TheGlobalData->m_iniCRC, roomToJoin->getExeCRC(), roomToJoin->getIniCRC()));
#if defined(RTS_DEBUG)
									if (TheGlobalData->m_netMinPlayers)
									{
										GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
										break;
									}
									else if (g_fakeCRC)
									{
										TheWritableGlobalData->m_exeCRC = roomToJoin->getExeCRC();
										TheWritableGlobalData->m_iniCRC = roomToJoin->getIniCRC();
									}
#else
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedCRCMismatch"));
									break;
#endif
								}
								Bool unknownLadder = (roomToJoin->getLadderPort() && TheLadderList->findLadder(roomToJoin->getLadderIP(), roomToJoin->getLadderPort()) == NULL);
								if (unknownLadder)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedUnknownLadder"));
									break;
								}
								if (roomToJoin->getNumPlayers() == MAX_SLOTS)
								{
									GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"), TheGameText->fetch("GUI:JoinFailedRoomFull"));
									break;
								}
								TheGameSpyInfo->markAsStagingRoomJoiner(selectedID);
								TheGameSpyGame->setGameName(roomToJoin->getGameName());
								TheGameSpyGame->setLadderIP(roomToJoin->getLadderIP());
								TheGameSpyGame->setLadderPort(roomToJoin->getLadderPort());
								SetLobbyAttemptHostJoin( TRUE );
								if (roomToJoin->getHasPassword())
								{
									GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
								}
								else
								{
									// no password - just join it
									PeerRequest req;
									req.peerRequestType = PeerRequest::PEERREQUEST_JOINSTAGINGROOM;
									req.text = srmIt->second->getGameName().str();
									req.stagingRoom.id = selectedID;
									req.password = "";
									TheGameSpyPeerMessageQueue->addRequest(req);
								}
							}
						}
						else
						{
							GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameInfo"), NULL);
						}
					}
					else
					{
						GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), NULL);
					}
					*/
				}
				else if ( controlID == buttonBuddyID )
				{
					GameSpyToggleOverlay( GSOVERLAY_BUDDY );
				}
				else if ( controlID == buttonGameListTypeToggleID )
				{
					ToggleGameListType();
				}
				else if ( controlID == buttonEmoteID )
				{
				// read the user's input and clear the entry box
					UnicodeString txtInput;
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					txtInput.trim();
					if (!txtInput.isEmpty())
					{
						// Send the message
						NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->SendChatMessageToCurrentRoom(txtInput, false);
					}
				}
				
				break;
			}// case GBM_SELECTED:

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if (s_tryingToHostOrJoin)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == comboLobbyGroupRoomsID ) 
				{
					int rowSelected = -1;
					GadgetComboBoxGetSelectedPos(control, &rowSelected);
				
					DEBUG_LOG(("Row selected = %d", rowSelected));
					if (rowSelected >= 0)
					{
						Int groupID;
						groupID = (Int)GadgetComboBoxGetItemData(comboLobbyGroupRooms, rowSelected);
						DEBUG_LOG(("ItemData was %d, current Group Room is %d", groupID, TheGameSpyInfo->getCurrentGroupRoom()));
// did it change?
						if (groupID != NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetCurrentRoomID())
						{
							// join
							NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->JoinRoom(groupID, [=]()
								{
									//GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Attempting to join room"), GameMakeColor(255, 194, 15, 255), -1, -1);
								},
								[=]()
								{
									// TODO_NGMP: There are two cb lamdas for this, flatten them
									GadgetListBoxReset(listboxLobbyChat);

									UnicodeString msg;
									msg.format(TheGameText->fetch("GUI:LobbyJoined"), NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().at(groupID).GetRoomDisplayName().str());
									GadgetListBoxAddEntryText(listboxLobbyChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);


									// process flag related info messages
									ERoomFlags flags = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().at(groupID).GetRoomFlags();
									if (flags == ERoomFlags::ROOM_FLAGS_SHOW_ALL_MATCHES)
									{
										GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"\t INFO: This is a special room where the lobby list shows lobbies from ALL rooms, not just the current room, to make it easier to find a match without room hopping."), GameMakeColor(255, 194, 15, 255), -1, -1);
										GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"\t INFO: Lobbies created here will only show in here. The members list & chat will also only show players in this room."), GameMakeColor(255, 194, 15, 255), -1, -1);
									}

									// refresh on join
									refreshPlayerList(TRUE);

									RefreshGameListBoxes();

									populateGroupRoomListbox(comboLobbyGroupRooms);
								});
						}

						// TODO_NGMP: What does TheGameSpyConfig->restrictGamesToLobby() do?
						/*						if (groupID && groupID != TheGameSpyInfo->getCurrentGroupRoom())
						{
							TheGameSpyInfo->leaveGroupRoom();
							TheGameSpyInfo->joinGroupRoom(groupID);

							if (TheGameSpyConfig->restrictGamesToLobby())
							{
								TheGameSpyInfo->clearStagingRoomList();
								RefreshGameListBoxes();
								PeerRequest req;
								req.peerRequestType = PeerRequest::PEERREQUEST_STARTGAMELIST;
								req.gameList.restrictGameList = TRUE;
								TheGameSpyPeerMessageQueue->addRequest(req);
							}
						}
						*/
					}
				}
			} // case GCM_SELECTED
			break;

		//---------------------------------------------------------------------------------------------
		case GLM_DOUBLE_CLICKED:
			{
				if (buttonPushed)
					break;

				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if (controlID == GetGameListBoxID())
				{
					int rowSelected = mData2;
				
					if (rowSelected >= 0)
					{
						GadgetListBoxSetSelected( control, rowSelected );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonJoinID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED, 
																								(WindowMsgData)button, buttonJoinID );
					}
				}
				break;
			}// case GLM_DOUBLE_CLICKED:

		//---------------------------------------------------------------------------------------------
		case GLM_RIGHT_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if( controlID == listboxLobbyPlayersID ) 
				{
					// TODO_NGMP: enable social again
					break;

					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = NULL;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					GPProfile profileID = 0;
					AsciiString aName;
					aName.translate(GadgetListBoxGetText(control, rc->pos, COLUMN_PLAYERNAME));
					PlayerInfoMap::iterator it = TheGameSpyInfo->getPlayerInfoMap()->find(aName);
					if (it != TheGameSpyInfo->getPlayerInfoMap()->end())
						profileID = it->second.m_profileID;

					Bool isBuddy = FALSE;
					if (profileID <= 0)
						rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCNoProfileMenu.wnd"));	
					else
					{
						if (profileID == TheGameSpyInfo->getLocalProfileID())
						{
							rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCLocalPlayerMenu.wnd"));
						}
						else if(TheGameSpyInfo->isBuddy(profileID))
						{
							rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCBuddiesMenu.wnd"));
							isBuddy = TRUE;
						}
						else
							rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCNonBuddiesMenu.wnd"));
					}
					if(!rcLayout)
						break;
					
					GadgetListBoxSetSelected(control, rc->pos);
					
					rcMenu = rcLayout->getFirstWindow();
					rcMenu->winGetLayout()->runInit();
					rcMenu->winBringToTop();
					rcMenu->winHide(FALSE);
					setUnignoreText( rcLayout, aName, profileID);
					ICoord2D rcSize, rcPos;
					rcMenu->winGetSize(&rcSize.x, &rcSize.y);
					rcPos.x = rc->mouseX;
					rcPos.y = rc->mouseY;
					if(rc->mouseX + rcSize.x > TheDisplay->getWidth())
						rcPos.x = TheDisplay->getWidth() - rcSize.x;
					if(rc->mouseY + rcSize.y > TheDisplay->getHeight())
						rcPos.y = TheDisplay->getHeight() - rcSize.y;
					rcMenu->winSetPosition(rcPos.x, rcPos.y);
					
					GameSpyRCMenuData *rcData = NEW GameSpyRCMenuData;
					rcData->m_id = profileID;
					rcData->m_nick = aName;
					rcData->m_itemType = (isBuddy)?ITEM_BUDDY:ITEM_NONBUDDY;
					rcMenu->winSetUserData((void *)rcData);
					TheWindowManager->winSetLoneWindow(rcMenu);
				}
				else if( controlID == GetGameListBoxID() ) 
				{
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = NULL;
					GameWindow *rcMenu;
					if(rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					Int selectedID = (Int)GadgetListBoxGetItemData(control, rc->pos);
					if (selectedID > 0)
					{
						StagingRoomMap *srm = TheGameSpyInfo->getStagingRoomList();
						StagingRoomMap::iterator srmIt = srm->find(selectedID);
						if (srmIt != srm->end())
						{
							GameSpyStagingRoom *theRoom = srmIt->second;
							if (!theRoom)
								break;
							const LadderInfo *linfo = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
							if (linfo)
							{
								rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCGameDetailsMenu.wnd"));	
								if (!rcLayout)
									break;

								GadgetListBoxSetSelected(control, rc->pos);

								rcMenu = rcLayout->getFirstWindow();
								rcMenu->winGetLayout()->runInit();
								rcMenu->winBringToTop();
								rcMenu->winHide(FALSE);
								rcMenu->winSetPosition(rc->mouseX, rc->mouseY);
								
								rcMenu->winSetUserData((void *)selectedID);
								TheWindowManager->winSetLoneWindow(rcMenu);
							}
						}
					}
				}
				break;
			}

//		//---------------------------------------------------------------------------------------------
//		case GSM_SLIDER_TRACK:
//		{
//				if (buttonPushed)
//					break;
//
//			GameWindow *control = (GameWindow *)mData1;
//			Int val = (Int)mData2;
//			Int controlID = control->winGetWindowId();
//			if (controlID == sliderChatAdjustID)
//			{
//				doSliderTrack(control, val);
//			}
//			break;
//		}

		//---------------------------------------------------------------------------------------------
		case GEM_EDIT_DONE:
			{
				if (buttonPushed)
					break;

				// read the user's input and clear the entry box
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryChat ));
				GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				txtInput.trim();
				if (!txtInput.isEmpty())
				{
					// Send the message
					if (!handleLobbySlashCommands(txtInput))
					{
						AsciiString txtInputAscii;
						txtInputAscii.translate(txtInput);
						NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_RoomChatMessage(txtInputAscii.str(), false);
						// TODO_NGMP: Support private message again
						//TheGameSpyInfo->sendChat( txtInput, false, listboxLobbyPlayers );
					}
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}//Switch

	return MSG_HANDLED;
}// WOLLobbyMenuSystem
