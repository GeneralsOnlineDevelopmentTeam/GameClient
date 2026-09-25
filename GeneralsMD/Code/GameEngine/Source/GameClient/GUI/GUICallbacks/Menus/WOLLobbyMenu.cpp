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
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

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
#include "GameNetwork/GeneralsOnline/OnlineServices_Moderation.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

void refreshGameList( Bool forceRefresh = FALSE );
void refreshPlayerList( Bool forceRefresh = FALSE );

#ifdef DEBUG_LOGGING
#define PERF_TEST
static LogClass s_perfLog("Perf.txt");
#define PERF_LOG(x) s_perfLog.log x
#else // DEBUG_LOGGING
#define PERF_LOG(x)
#endif // DEBUG_LOGGING

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
static Bool isShuttingDown = false;
static Bool buttonPushed = false;
static const char *nextScreen = nullptr;
static Bool raiseMessageBoxes = false;
static UnsignedInt s_lobbyMenuGeneration = 0;
static time_t gameListRefreshTime = 0;
static const time_t gameListRefreshInterval = 4000;
static time_t playerListRefreshTime = 0;
static const time_t playerListRefreshInterval = 4000;

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
static GameWindow *parentWOLLobby = nullptr;
static GameWindow *buttonBack = nullptr;
static GameWindow *buttonHost = nullptr;
static GameWindow *buttonRefresh = nullptr;
static GameWindow *buttonJoin = nullptr;
static GameWindow *buttonBuddy = nullptr;
static GameWindow *buttonEmote = nullptr;
static GameWindow *textEntryChat = nullptr;
static GameWindow *listboxLobbyPlayers = nullptr;
static GameWindow *listboxLobbyChat = nullptr;
static GameWindow *comboLobbyGroupRooms = nullptr;
static GameWindow *parent = nullptr;

static Int groupRoomToJoin = 0;
static Int	initialGadgetDelay = 2;
static Bool justEntered = FALSE;

// Preserve rejected messages while the server enforces the limit.
static std::deque<std::chrono::steady_clock::time_point> s_lobbyChatMessageTimes;

static bool LobbyChatRateLimitAllowsSend()
{
	using namespace std::chrono;

	const auto now = steady_clock::now();
	const auto window = seconds(9);
	while (!s_lobbyChatMessageTimes.empty() && now - s_lobbyChatMessageTimes.front() >= window)
	{
		s_lobbyChatMessageTimes.pop_front();
	}

	if (s_lobbyChatMessageTimes.size() >= 3)
	{
		ShowChatRateLimitNotice(
			"Rate limit: Please wait before sending another message.",
			"room");
		return false;
	}

	s_lobbyChatMessageTimes.push_back(now);
	return true;
}

#if defined(RTS_DEBUG)
Bool g_fakeCRC = FALSE;
Bool g_debugSlots = FALSE;
#endif

std::list<PeerResponse> TheLobbyQueuedUTMs;

// Slash commands -------------------------------------------------------------------------
extern "C" {
int getQR2HostingStatus();
}
extern int isThreadHosting;

Bool handleLobbySlashCommands(UnicodeString uText, Bool *wasRateLimited)
{
	if (wasRateLimited != nullptr)
		*wasRateLimited = FALSE;

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
		TheGameSpyInfo->addText(s, GameSpyColor[GSCOLOR_DEFAULT], nullptr);
		*/
		return TRUE; // was a slash command
	}
	else if (token == "me" && uText.getLength()>4)
	{
		if (!LobbyChatRateLimitAllowsSend())
		{
			if (wasRateLimited != nullptr)
				*wasRateLimited = TRUE;
			return TRUE;
		}

		UnicodeString msg = UnicodeString(uText.str() + 4); // skip the /me
		NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
		if (pRoomsInterface != nullptr)
		{
			pRoomsInterface->SendChatMessageToCurrentRoom(msg, true);
		}
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
		UnicodeString newName(uText.str() + 6); // skip the /name or nick

		if (newName.getLength() < 3 || newName.getLength() > 16)
		{
			GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Your new name must be between 3 and 16 characters."), GameMakeColor(255, 0, 0, 255), -1, -1);
		}
		else
		{
			std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
			if (pWS != nullptr)
			{
				pWS->SendData_ChangeName(newName);
			}
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
		TheGameSpyInfo->addText(L"Toggled CRC fakery", GameSpyColor[GSCOLOR_DEFAULT], nullptr);
		return TRUE; // was a slash command
	}
	else if (token == "slots")
	{
		g_debugSlots = !g_debugSlots;
		TheGameSpyInfo->addText(L"Toggled SlotList debug", GameSpyColor[GSCOLOR_DEFAULT], nullptr);
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

	// TODO_NGMP: This causes issues with duplicate names. We should have better ways of looking this up + perhaps only allow unique names
	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
	NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pRoomsInterface != nullptr && pAuthInterface != nullptr && pStatsInterface != nullptr && pSocialInterface != nullptr)
	{
		int profileID = (int)GadgetListBoxGetItemData(listboxLobbyPlayers, row, 0);
		NetworkRoomMember* roomMember = pRoomsInterface->GetRoomMemberFromID(profileID);

		// TODO_NGMP: This is an async call, we should block future popups until it returns to avoid weirdness
		if (col > 0)
		{
			if (roomMember != nullptr)
			{
				// Capture user_id by value to avoid use-after-free when m_mapMembers is reassigned
				uint64_t userID = roomMember->user_id;
				
				// new
				pStatsInterface->findPlayerStatsByID(userID, [=](bool bSuccess, PSPlayerStats stats)
					{
						if (!bSuccess)
						{
							TheMouse->setCursorTooltip(UnicodeString(L"Error: 1"), -1, NULL, 1.5f);
						}
						else
						{
							UnicodeString tooltip = UnicodeString::TheEmptyString;
							if (userID == pAuthInterface->GetUserID())
							{
								tooltip.format(TheGameText->fetch("TOOLTIP:LocalPlayer"), uName.str());
							}
							else
							{
								// not us
								bool bIsFriend = pSocialInterface->IsUserFriend(userID);
								if (bIsFriend)
								{
									// buddy
									tooltip.format(TheGameText->fetch("TOOLTIP:BuddyPlayer"), uName.str());
								}
								else
								{
									// non-buddy profiled player
									tooltip.format(TheGameText->fetch("TOOLTIP:ProfiledPlayer"), uName.str());

									// NOTE: Removed non-profiled generic player, this doesn't exist on Generals Online, everyone has a profile
								}
							}

							bool bIgnored = pSocialInterface->IsUserIgnored(userID);
							if (bIgnored)
							{
								tooltip.concat(TheGameText->fetch("TOOLTIP:IgnoredModifier"));
							}

							// ELO data
							UnicodeString tmp;
							tmp.format(L"\n\nOverall Elo Rating: %d (in %d matches)", stats.elo_rating, stats.elo_num_matches);
							tooltip.concat(tmp);
							tmp.format(L"\nWS Elo Rating: %d", stats.monthly_elo_rating);
							tooltip.concat(tmp);
							Int rankPoints = CalculateRank(stats);
							Int rank = 0;
							Int i = 0;
							if (TheRankPointValues != nullptr)
							{
								while (i + 1 < MAX_RANKS && rankPoints >= TheRankPointValues->m_ranks[i + 1])
									++i;
							}
							rank = i;

							// determine favorite side
							Int mostGames = 0;
							Int favorite = 0;
							for (auto it = stats.games.begin(); it != stats.games.end(); ++it)
							{
								if (it->second >= mostGames)
								{
									mostGames = it->second;
									favorite = it->first;
								}
							}

							AsciiString sideName = "GUI:RandomSide";
							if (mostGames > 0)
							{
								if (favorite > 1) // cant be civilian or observer
								{
									const PlayerTemplate* fac = ThePlayerTemplateStore->getNthPlayerTemplate(favorite);
									if (fac)
									{
										sideName.format("SIDE:%s", fac->getSide().str());
									}
								}
							}
							AsciiString rankName;
							rankName.format("GUI:GSRank%d", rank);
							
							tmp.clear();
							tmp.format(L"\n\nFavorite Side: %ls\nRank: %ls", TheGameText->fetch(sideName).str(), TheGameText->fetch(rankName).str());
							tooltip.concat(tmp);

							int totalWins = 0;
							int totalLosses = 0;
							int totalDC = 0;
							int totalWinsInRow = 0;
							int totalLossesInRow = 0;
							int totalDCInRow = 0;
							int maxWinsInRow = 0;
							int maxLossesInRow = 0;
							int maxDCInRow = 0;

							for (int i = 0; i < stats.wins.size(); ++i) { totalWins += stats.wins[i]; }
							for (int i = 0; i < stats.losses.size(); ++i) { totalLosses += stats.losses[i]; }
							for (int i = 0; i < stats.discons.size(); ++i) { totalDC += stats.discons[i]; }

                            totalWinsInRow = stats.winsInARow;
                            totalLossesInRow = stats.lossesInARow;
                            totalDCInRow = stats.disconsInARow;

                            maxWinsInRow = stats.maxWinsInARow;
                            maxLossesInRow = stats.maxLossesInARow;

							tmp.clear();
							tmp.format(L"\n\nTotal Wins: %d\nTotal Losses: %d\nTotal Disconnects: %d\n\nCurrent Win Streak: %d\nCurrent Loss Streak: %d\nCurrent Disconnect Streak: %d\n\nLongest Win Streak: %d\nLongest Loss Streak: %d\nLongest Disconnect Streak: %d",
								totalWins,
								totalLosses,
								totalDC,
								totalWinsInRow,
								totalLossesInRow,
								totalDCInRow,
								maxWinsInRow,
								maxLossesInRow,
								maxDCInRow);
							tooltip.concat(tmp);

							if (pRoomsInterface != nullptr && pAuthInterface != nullptr)
							{
								NetworkRoomMember* localMember = pRoomsInterface->GetRoomMemberFromID(pAuthInterface->GetUserID());
								if (localMember != nullptr && localMember->m_bIsAdmin)
								{
									UnicodeString idLine;
									idLine.format(L"\n\nUser ID: %lld", roomMember->user_id);
									tooltip.concat(idLine);
								}
							}

							TheMouse->setCursorTooltip(tooltip, -1, NULL, 1.5f); // the text and width are the only params used.  the others are the default values.
						}
					}, EStatsRequestPolicy::RESPECT_CACHE_ALLOW_REQUEST);
			}
			else
			{
				TheMouse->setCursorTooltip(UnicodeString(L"Error: 1"), -1, NULL, 1.5f); // the text and width are the only params used.  the others are the default values.
			}



		}
	}


	return;

	// TODO_NGMP:
	/*
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

	if (!TheRankPointValues)
		return;

	Int rank = 0;
	Int i = 0;
	while (i + 1 < MAX_RANKS && info->m_rankPoints >= TheRankPointValues->m_ranks[i + 1])
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

	TheMouse->setCursorTooltip( tooltip, -1, nullptr, 1.5f ); // the text and width are the only params used.  the others are the default values.
	*/
}

// Set while repopulating the combo box, because setting the selection re-sends GCM_SELECTED as if the user had picked it.
static Bool s_populatingLobbyCombo = FALSE;
static const Int LOBBY_COMBO_SEPARATOR_ITEM_DATA = -1;

static Int FindRoomIndexByID(const std::vector<NetworkRoom>& rooms, Int roomID)
{
	for (Int roomIndex = 0; roomIndex < (Int)rooms.size(); ++roomIndex)
	{
		if (rooms[roomIndex].GetRoomID() == roomID)
		{
			return roomIndex;
		}
	}

	return -1;
}

static Bool RoomHasLaterSibling(const std::vector<NetworkRoom>& rooms, Int roomIndex)
{
	const Int parentRoomID = rooms[roomIndex].GetParentRoomID();
	for (Int siblingIndex = roomIndex + 1; siblingIndex < (Int)rooms.size(); ++siblingIndex)
	{
		if (rooms[siblingIndex].GetParentRoomID() == parentRoomID)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static UnicodeString FormatRoomLabel(const std::vector<NetworkRoom>& rooms, Int roomIndex)
{
	UnicodeString label;
	const NetworkRoom& room = rooms[roomIndex];
	std::vector<Int> ancestors;
	Int parentRoomID = room.GetParentRoomID();

	while (parentRoomID >= 0 && ancestors.size() < rooms.size())
	{
		const Int parentIndex = FindRoomIndexByID(rooms, parentRoomID);
		if (parentIndex < 0)
		{
			break;
		}

		ancestors.push_back(parentIndex);
		parentRoomID = rooms[parentIndex].GetParentRoomID();
	}

	std::reverse(ancestors.begin(), ancestors.end());
	for (size_t ancestorIndex = 1; ancestorIndex < ancestors.size(); ++ancestorIndex)
	{
		label.concat(RoomHasLaterSibling(rooms, ancestors[ancestorIndex]) ? L"\u2502  " : L"   ");
	}

	if (!ancestors.empty())
	{
		label.concat(RoomHasLaterSibling(rooms, roomIndex) ? L"\u251C\u2500 " : L"\u2514\u2500 ");
	}
	label.concat(room.GetRoomDisplayName());
	return label;
}

static void PopulateLobbyFilterComboBox(GameWindow* comboBox)
{
	if (comboBox == nullptr)
		return;

	extern LobbyGameModeFilter theLobbyFilter;
	s_populatingLobbyCombo = TRUE;
	GadgetComboBoxReset(comboBox);

	static const struct
	{
		const wchar_t* label;
		LobbyGameModeFilter filter;
	} filterEntries[] =
	{
		{ L"Filter: All",			LOBBY_FILTER_ALL },
		{ L"Filter: 1v1",			LOBBY_FILTER_1V1 },
		{ L"Filter: Team Games",	LOBBY_FILTER_TEAM },
		{ L"Filter: FFA",			LOBBY_FILTER_FFA },
		{ L"Filter: AOD",			LOBBY_FILTER_AOD },
		{ L"Filter: Buddies",		LOBBY_FILTER_BUDDIES },
	};

	Int idx;
	Int selectedRoomIdx = -1;
	UnicodeString selectedRoomName;

	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRoomsInterface != nullptr)
	{
		const std::vector<NetworkRoom> rooms = pRoomsInterface->GetGroupRooms();
		const Int currentRoomIndex = pRoomsInterface->GetCurrentRoomIndex();
		const Int numRooms = (Int)rooms.size();
		for (Int i = 0; i < numRooms; ++i)
		{
			const UnicodeString roomLabel = FormatRoomLabel(rooms, i);
			idx = GadgetComboBoxAddEntry(comboBox, roomLabel,
				GameSpyColor[i == currentRoomIndex ? GSCOLOR_CURRENTROOM : GSCOLOR_ROOM]);
			// Room entries use values below the negative separator value.
			GadgetComboBoxSetItemData(comboBox, idx, (void*)(intptr_t)(-(i + 2)));

			if (i == currentRoomIndex)
			{
				selectedRoomIdx = idx;
				selectedRoomName = rooms[i].GetRoomDisplayName();
			}
		}

		if (numRooms > 0)
		{
			idx = GadgetComboBoxAddEntry(comboBox, UnicodeString(L" "), GameSpyColor[GSCOLOR_ROOM]);
			GadgetComboBoxSetItemData(comboBox, idx, (void*)(intptr_t)LOBBY_COMBO_SEPARATOR_ITEM_DATA);
		}
	}

	for (const auto& filterEntry : filterEntries)
	{
		const Bool isActiveFilter = (filterEntry.filter == theLobbyFilter);
		idx = GadgetComboBoxAddEntry(comboBox, UnicodeString(filterEntry.label),
			GameSpyColor[isActiveFilter ? GSCOLOR_CURRENTROOM : GSCOLOR_DEFAULT]);
		GadgetComboBoxSetItemData(comboBox, idx, (void*)filterEntry.filter);
	}

	// The collapsed combo always identifies the room. The active filter is indicated by its color only when expanded.
	GadgetComboBoxSetSelectedPos(comboBox, selectedRoomIdx);
	if (selectedRoomIdx >= 0)
	{
		GadgetComboBoxSetText(comboBox, selectedRoomName);
	}
	s_populatingLobbyCombo = FALSE;
}

static void HandleNetworkRoomChanged(int roomIndex, bool effectiveRoomChanged)
{
	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRoomsInterface == nullptr)
		return;

	const std::vector<NetworkRoom>& rooms = pRoomsInterface->GetGroupRooms();
	if (roomIndex < 0 || roomIndex >= (int)rooms.size())
		return;

	if (effectiveRoomChanged)
	{
		GadgetListBoxReset(listboxLobbyChat);
		refreshPlayerList(TRUE);
	}

	UnicodeString msg;
	msg.format(TheGameText->fetch("GUI:LobbyJoined"), rooms[roomIndex].GetRoomDisplayName().str());
	GadgetListBoxAddEntryText(listboxLobbyChat, msg, GameSpyColor[GSCOLOR_DEFAULT], -1, -1);

	refreshGameList(TRUE);
	PopulateLobbyFilterComboBox(comboLobbyGroupRooms);
}

static const char *const rankNames[] = {
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
static_assert(ARRAY_SIZE(rankNames) == MAX_RANKS, "Incorrect array size");


const Image* LookupSmallRankImage(Int side, Int rankPoints)
{
	if (rankPoints == 0 || !TheRankPointValues)
		return nullptr;

	Int rank = 0;
	Int i = 0;
	while (i + 1 < MAX_RANKS && rankPoints >= TheRankPointValues->m_ranks[i + 1])
		++i;
	rank = i;

	if (rank < 0 || rank >= 10)
		return nullptr;

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

// Lobby player list: rebuilt only when the roster changes; stats are fetched only for visible rows.

struct LobbyPlayerRow
{
	int64_t     userID = 0;
	Bool        isAdmin = FALSE;
	Bool        isFriend = FALSE;
	Bool        isIgnored = FALSE;
	Bool        iconResolved = FALSE; // icon painted from fresh stats
	std::string displayName;
	std::string sortKey; // lowercase display name
};

static std::vector<LobbyPlayerRow> s_lobbyPlayerRows;   // index == row
static std::string s_lobbyRosterSignature;              // rebuild when it changes

// Last resolved (rank points, favorite side) per user; avoids icon blink on a cache miss.
static std::unordered_map<int64_t, std::pair<Int, Int>> s_lastKnownRankByUser;

// One batch stats request at a time; overlapping requests supersede each other's responses.
static Bool s_statsBatchInFlight = FALSE;
static UnsignedInt s_statsBatchStartTime = 0;
static UnsignedInt s_statsBatchGeneration = 0; // bumped on lobby init
static const UnsignedInt STATS_BATCH_WATCHDOG_MS = 30000; // recover from a lost response

static Int s_lastVisibleTop = -1;
static Int s_lastVisibleBottom = -1;
static const Int VISIBLE_STATS_BUFFER = 8; // prefetch rows around the viewport

static UnsignedInt s_lastPlayerListRebuild = 0;
static const UnsignedInt PLAYERLIST_MIN_REBUILD_MS = 1000; // floor between full rebuilds

//-------------------------------------------------------------------------------------------------
static Int LobbyRankIconExtent()
{
	const Image* preorderImg = TheMappedImageCollection->findImageByName("OfficersClubsmall");
	Int w = (preorderImg) ? preorderImg->getImageWidth() : 10;
	if (listboxLobbyPlayers != nullptr)
		w = min(GadgetListBoxGetColumnWidth(listboxLobbyPlayers, 0), w);
	return w;
}

//-------------------------------------------------------------------------------------------------
/** Rank icon from cached stats, falling back to the last resolved value. */
static const Image* ResolveRankIconForUser(int64_t userID, NGMP_OnlineServices_StatsInterface* pStatsInterface)
{
	Int rankPoints = 0;
	Int favoriteSide = 0;
	Bool bResolved = FALSE;

	PSPlayerStats stats = PSPlayerStats();
	if (pStatsInterface != nullptr
		&& pStatsInterface->getPlayerStatsFromCache(userID, &stats)
		&& stats.id != 0)
	{
		rankPoints = CalculateRank(stats);
		favoriteSide = GetFavoriteSide(stats);
		s_lastKnownRankByUser[userID] = std::make_pair(rankPoints, favoriteSide);
		bResolved = TRUE;
	}

	if (!bResolved)
	{
		auto it = s_lastKnownRankByUser.find(userID);
		if (it != s_lastKnownRankByUser.end())
		{
			rankPoints = it->second.first;
			favoriteSide = it->second.second;
		}
	}

	if (favoriteSide < 2) // no real faction
		return nullptr;

	return LookupSmallRankImage(favoriteSide, rankPoints);
}

//-------------------------------------------------------------------------------------------------
/** Repaint visible rows' rank icons from the stats cache (no network). */
static void RefreshVisibleLobbyRowIcons()
{
	if (listboxLobbyPlayers == nullptr || s_lobbyPlayerRows.empty())
		return;

	NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();

	const Int rowCount = (Int)s_lobbyPlayerRows.size();
	Int top = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);
	Int bottom = GadgetListBoxGetBottomVisibleEntry(listboxLobbyPlayers);
	if (top < 0) top = 0;
	if (bottom < 0 || bottom >= rowCount) bottom = rowCount - 1;

	const Int firstRow = max(0, top - VISIBLE_STATS_BUFFER);
	const Int lastRow = min(rowCount - 1, bottom + VISIBLE_STATS_BUFFER);
	const Int iconExtent = LobbyRankIconExtent();

	for (Int row = firstRow; row <= lastRow; ++row)
	{
		LobbyPlayerRow& playerRow = s_lobbyPlayerRows[row];

		// fresh-stats icons don't change; skip
		if (playerRow.iconResolved)
			continue;

		const Image* rankImg = ResolveRankIconForUser(playerRow.userID, pStatsInterface);
		GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, row, 0, iconExtent, iconExtent);

		if (pStatsInterface != nullptr && pStatsInterface->HasFreshPlayerStats(playerRow.userID))
			playerRow.iconResolved = TRUE;
	}
}

//-------------------------------------------------------------------------------------------------
/** Request stats for visible rows; no-op if the window hasn't moved unless bForce. */
static void EnsureVisibleLobbyStats(Bool bForce)
{
	if (listboxLobbyPlayers == nullptr || s_lobbyPlayerRows.empty())
		return;

	NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
	if (pStatsInterface == nullptr)
		return;

	const Int rowCount = (Int)s_lobbyPlayerRows.size();
	Int top = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);
	Int bottom = GadgetListBoxGetBottomVisibleEntry(listboxLobbyPlayers);
	if (top < 0) top = 0;
	if (bottom < 0 || bottom >= rowCount) bottom = rowCount - 1;

	if (!bForce && top == s_lastVisibleTop && bottom == s_lastVisibleBottom)
		return;
	s_lastVisibleTop = top;
	s_lastVisibleBottom = bottom;

	RefreshVisibleLobbyRowIcons();

	const Int firstRow = max(0, top - VISIBLE_STATS_BUFFER);
	const Int lastRow = min(rowCount - 1, bottom + VISIBLE_STATS_BUFFER);

	std::vector<int64_t> vecUserStatsToRequest;
	for (Int row = firstRow; row <= lastRow; ++row)
	{
		const int64_t userID = s_lobbyPlayerRows[row].userID;
		if (!pStatsInterface->HasFreshPlayerStats(userID))
			vecUserStatsToRequest.push_back(userID);
	}

	if (vecUserStatsToRequest.empty())
		return;

	const UnsignedInt now = timeGetTime();
	if (s_statsBatchInFlight && (now - s_statsBatchStartTime) < STATS_BATCH_WATCHDOG_MS)
		return;

	s_statsBatchInFlight = TRUE;
	s_statsBatchStartTime = now;
	const UnsignedInt generation = s_statsBatchGeneration;
	pStatsInterface->findPlayerStatsByBatch(vecUserStatsToRequest, [generation](bool /*bSuccess*/)
		{
			// response from a previous lobby visit
			if (generation != s_statsBatchGeneration)
				return;

			s_statsBatchInFlight = FALSE;

			// lobby was left
			if (listboxLobbyPlayers == nullptr)
				return;

			RefreshVisibleLobbyRowIcons();
		});
}

//-------------------------------------------------------------------------------------------------
/** Room roster in display order: name, then admins, then friends. */
static void CollectLobbyPlayerRows(std::vector<LobbyPlayerRow>& outRows)
{
	outRows.clear();

	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pRoomsInterface == nullptr)
		return;

	auto membersMap = pRoomsInterface->GetMembersListForCurrentRoom();
	outRows.reserve(membersMap.size());

	for (auto& [id, member] : membersMap)
	{
		LobbyPlayerRow row;
		row.userID = member.user_id;
		row.displayName = member.display_name;
		row.isAdmin = member.m_bIsAdmin ? TRUE : FALSE;
		row.isFriend = (pSocialInterface != nullptr && pSocialInterface->IsUserFriend(member.user_id)) ? TRUE : FALSE;
		row.isIgnored = (pSocialInterface != nullptr && pSocialInterface->IsUserIgnored(member.user_id)) ? TRUE : FALSE;

		row.sortKey.resize(row.displayName.size());
		std::transform(row.displayName.begin(), row.displayName.end(), row.sortKey.begin(),
			[](unsigned char c) { return std::tolower(c); });

		outRows.emplace_back(std::move(row));
	}

	std::sort(outRows.begin(), outRows.end(),
		[](const LobbyPlayerRow& a, const LobbyPlayerRow& b) { return a.sortKey < b.sortKey; });

	auto afterAdmins = std::stable_partition(outRows.begin(), outRows.end(),
		[](const LobbyPlayerRow& x) { return x.isAdmin != FALSE; });

	std::stable_partition(afterAdmins, outRows.end(),
		[](const LobbyPlayerRow& x) { return x.isFriend != FALSE; });
}

//-------------------------------------------------------------------------------------------------
static std::string BuildLobbyRosterSignature(const std::vector<LobbyPlayerRow>& rows)
{
	std::string sig;
	sig.reserve(rows.size() * 24);
	for (const LobbyPlayerRow& row : rows)
	{
		const int flags = (row.isAdmin ? 1 : 0) | (row.isFriend ? 2 : 0) | (row.isIgnored ? 4 : 0);
		sig += std::to_string(row.userID);
		sig += '/';
		sig += std::to_string(flags);
		sig += '/';
		sig += row.displayName;
		sig += ';';
	}
	return sig;
}

//-------------------------------------------------------------------------------------------------
/** Full listbox rebuild - only call when the roster / ordering changed. */
static void RebuildLobbyPlayerList(const std::vector<LobbyPlayerRow>& rows)
{
	if (listboxLobbyPlayers == nullptr)
		return;

	NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	const int64_t localUserID = (pAuthInterface != nullptr) ? pAuthInterface->GetUserID() : 0;

	// preserve selection and scroll
	Int selectedIndex = -1;
	GadgetListBoxGetSelected(listboxLobbyPlayers, &selectedIndex);
	const int64_t selectedUserID = (selectedIndex >= 0 && selectedIndex < (Int)s_lobbyPlayerRows.size())
		? s_lobbyPlayerRows[selectedIndex].userID : 0;
	const Int previousTopIndex = GadgetListBoxGetTopVisibleEntry(listboxLobbyPlayers);

	GadgetListBoxReset(listboxLobbyPlayers);
	s_lobbyPlayerRows = rows;

	const Int iconExtent = LobbyRankIconExtent();
	Int indexToSelect = -1;

	// batch: avoids per-row height recompute
	{
		GadgetListBoxBatchAddScope batchScope(listboxLobbyPlayers);
		for (const LobbyPlayerRow& row : s_lobbyPlayerRows)
		{
			Color colorToUse = GameSpyColor[GSCOLOR_PLAYER_NORMAL];
			if (row.isAdmin)
				colorToUse = GameSpyColor[GSCOLOR_PLAYER_OWNER];
			else if (row.isFriend)
				colorToUse = GameSpyColor[GSCOLOR_PLAYER_BUDDY];
			else if (row.isIgnored)
				colorToUse = GameSpyColor[GSCOLOR_PLAYER_IGNORED];
			else if (row.userID == localUserID)
				colorToUse = GameSpyColor[GSCOLOR_PLAYER_SELF];

			const Image* rankImg = ResolveRankIconForUser(row.userID, pStatsInterface);

			Int index = GadgetListBoxAddEntryImage(listboxLobbyPlayers, rankImg, -1, 0, iconExtent, iconExtent);
			GadgetListBoxAddEntryText(listboxLobbyPlayers, UnicodeString(from_utf8(row.displayName).c_str()), colorToUse, index, 1);
			GadgetListBoxSetItemData(listboxLobbyPlayers, (void*)(Int)row.userID, index);

			if (selectedUserID != 0 && row.userID == selectedUserID)
				indexToSelect = index;
		}
	}

	// drop cached icons for departed users
	{
		std::unordered_set<int64_t> present;
		present.reserve(s_lobbyPlayerRows.size());
		for (const LobbyPlayerRow& row : s_lobbyPlayerRows)
			present.insert(row.userID);

		for (auto it = s_lastKnownRankByUser.begin(); it != s_lastKnownRankByUser.end(); )
		{
			if (present.find(it->first) == present.end())
				it = s_lastKnownRankByUser.erase(it);
			else
				++it;
		}
	}

	GadgetListBoxSetTopVisibleEntry(listboxLobbyPlayers, previousTopIndex);
	if (indexToSelect >= 0)
		GadgetListBoxSetSelected(listboxLobbyPlayers, indexToSelect);
	else if (selectedIndex >= 0)
		TheWindowManager->winSetLoneWindow(NULL);

	s_lobbyRosterSignature = BuildLobbyRosterSignature(s_lobbyPlayerRows);
	s_lastVisibleTop = -1;
	s_lastVisibleBottom = -1;
	EnsureVisibleLobbyStats(TRUE);
}

//-------------------------------------------------------------------------------------------------
void PopulateLobbyPlayerListbox()
{
	if (listboxLobbyPlayers == nullptr)
		return;

	std::vector<LobbyPlayerRow> rows;
	CollectLobbyPlayerRows(rows);

	const Bool rosterChanged = (BuildLobbyRosterSignature(rows) != s_lobbyRosterSignature);

	// rebuild is costly; the signature stays stale until one runs, so nothing is lost
	const UnsignedInt now = timeGetTime();
	const Bool rebuildAllowed = (s_lastPlayerListRebuild == 0)
		|| (now - s_lastPlayerListRebuild) >= PLAYERLIST_MIN_REBUILD_MS;

	if (rosterChanged && rebuildAllowed)
	{
		s_lastPlayerListRebuild = now;
		RebuildLobbyPlayerList(rows);
	}
	else
	{
		// no rebuild: just refresh visible stats
		EnsureVisibleLobbyStats(TRUE);
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

        case EJoinLobbyResult::JoinLobbyResult_AnticheatMismatch:
            s = TheGameText->fetchOrSubstitute("GUI:JoinFailedAnticheatMismatch", L"You are running a different anticheat from this lobby host.");
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
	const UnsignedInt lobbyMenuGeneration = ++s_lobbyMenuGeneration;

	// for safety (and sanity)
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{
		pLobbyInterface->LeaveCurrentLobby();
	}
	
	nextScreen = nullptr;
	buttonPushed = false;
	isShuttingDown = false;

	SetLobbyAttemptHostJoin(FALSE); // not trying to host or join

	gameListRefreshTime = 0;
	playerListRefreshTime = 0;
	s_statsBatchInFlight = FALSE;
	++s_statsBatchGeneration;
	s_lastKnownRankByUser.clear();
	s_lobbyPlayerRows.clear();
	s_lobbyRosterSignature.clear();
	s_lastVisibleTop = -1;
	s_lastVisibleBottom = -1;
	s_lastPlayerListRebuild = 0;

	parentWOLLobbyID = TheNameKeyGenerator->nameToKey( "WOLCustomLobby.wnd:WOLLobbyMenuParent" );
	parent = TheWindowManager->winGetWindowFromId(nullptr, parentWOLLobbyID);

	buttonBackID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBack");
	buttonBack = TheWindowManager->winGetWindowFromId(parent, buttonBackID);

	buttonHostID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonHost");
	buttonHost = TheWindowManager->winGetWindowFromId(parent, buttonHostID);

	buttonRefreshID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonRefresh");
	buttonRefresh = TheWindowManager->winGetWindowFromId(parent, buttonRefreshID);

	buttonJoinID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonJoin");
	buttonJoin = TheWindowManager->winGetWindowFromId(parent, buttonJoinID);
	buttonJoin->winEnable(FALSE);

	buttonBuddyID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonBuddy");
	buttonBuddy = TheWindowManager->winGetWindowFromId(parent, buttonBuddyID);

	buttonEmoteID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ButtonEmote");
	buttonEmote = TheWindowManager->winGetWindowFromId(parent, buttonEmoteID);

	textEntryChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:TextEntryChat");
	textEntryChat = TheWindowManager->winGetWindowFromId(parent, textEntryChatID);

	listboxLobbyPlayersID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxPlayers");
	listboxLobbyPlayers = TheWindowManager->winGetWindowFromId(parent, listboxLobbyPlayersID);
	GadgetListBoxRemoveMultiSelect(listboxLobbyPlayers);
	listboxLobbyPlayers->winSetTooltipFunc(playerTooltip);
	SetListBoxRowAnimMode(listboxLobbyPlayers, LIST_ROW_ANIM_ID);

	listboxLobbyChatID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ListboxChat");
	listboxLobbyChat = TheWindowManager->winGetWindowFromId(parent, listboxLobbyChatID);
	SetListBoxRowAnimMode(listboxLobbyChat, LIST_ROW_ANIM_SLOT);

	comboLobbyGroupRoomsID = TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:ComboBoxGroupRooms");
	comboLobbyGroupRooms = TheWindowManager->winGetWindowFromId(parent, comboLobbyGroupRoomsID);

	//GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Welcome to Generals Online for Zero Hour!"), GameMakeColor(255, 194, 15, 255), -1, -1);

	GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);

	PopulateLobbyFilterComboBox(comboLobbyGroupRooms);

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
	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pLobbyInterface != nullptr && pRoomsInterface != nullptr)
	{
		pLobbyInterface->RegisterForCreateLobbyCallback(NGMP_WOLLobbyMenu_CreateLobbyCallback);

		// NGMP: Join lobby callback
		pLobbyInterface->RegisterForJoinLobbyCallback(NGMP_WOLLobbyMenu_JoinLobbyCallback);

		// NGMP: Request lobbies

		//GadgetListBoxSetItemData(listboxLobbyChat, (void*)-1, index);

		// TODO_NGMP: player list change callbacks

		// register for chat events
		pRoomsInterface->RegisterForChatCallback([](UnicodeString strMessage, Color color)
			{
				GadgetListBoxAddEntryText(listboxLobbyChat, strMessage, color, -1, -1);
			});

		// register for roster events (throttled path)
		pRoomsInterface->RegisterForRosterNeedsRefreshCallback([]()
				{
					refreshPlayerList(false);
			});

		pRoomsInterface->RegisterForRoomChangedCallback(HandleNetworkRoomChanged);
	}

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

	raiseMessageBoxes = true;

	TheLobbyQueuedUTMs.clear();
	justEntered = TRUE;
	initialGadgetDelay = 2;
	GameWindow *win = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("WOLCustomLobby.wnd:GadgetParent"));
	if(win)
		win->winHide(TRUE);
	DontShowMainMenu = TRUE;


#if defined(GENERALS_ONLINE)
// upon entry, retrieve room list

	NGMP_OnlineServices_RoomsInterface* pRoomsInterfaceOuter = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRoomsInterfaceOuter != nullptr)
	{
		pRoomsInterfaceOuter->GetRoomList([=](bool success)
			{
				if (lobbyMenuGeneration != s_lobbyMenuGeneration || buttonPushed || isShuttingDown || listboxLobbyChat == nullptr)
				{
					return;
				}

				const std::vector<NetworkRoom>& rooms = pRoomsInterfaceOuter->GetGroupRooms();
				if (!success || rooms.empty())
				{
					GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"\t ERROR: No rooms are available. Try logging in again."), GameMakeColor(255, 0, 0, 255), -1, -1);
					return;
				}

				pRoomsInterfaceOuter->JoinRoom(0);
			});
	}

	// Update the communicator button anytime we get notifications
    NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pSocialInterface != nullptr)
	{
		// notifiactions callback
		pSocialInterface->RegisterForCallback_OnNumberGlobalNotificationsChanged([](int numNotifications)
			{
				// update communicator button
				if (buttonBuddy != nullptr)
				{
					UnicodeString buttonText;
                    if (numNotifications > 0)
                    {
                        buttonText.format(L"%s [%d]", TheGameText->fetch("GUI:Buddies").str(), numNotifications);
                    }
                    else
                    {
                        buttonText.format(L"%s", TheGameText->fetch("GUI:Buddies").str());
                    }
					buttonBuddy->winSetText(buttonText);
				}
			});
	}

	// And also initialize it
    if (buttonBuddy != nullptr && pSocialInterface->GetNumTotalNotifications() > 0)
    {
        UnicodeString buttonText;
        buttonText.format(L"%s [%d]", TheGameText->fetch("GUI:Buddies").str(), pSocialInterface->GetNumTotalNotifications());
        buttonBuddy->winSetText(buttonText);
    }
#endif
}

//-------------------------------------------------------------------------------------------------
/** This is called when a shutdown is complete for this menu */
//-------------------------------------------------------------------------------------------------
static void shutdownComplete( WindowLayout *layout )
{

	isShuttingDown = false;

	// hide the layout
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout, (nextScreen != nullptr) );

	if (nextScreen != nullptr)
	{
		TheShell->push(nextScreen);
	}

	nextScreen = nullptr;

}

//-------------------------------------------------------------------------------------------------
/** WOL Lobby Menu shutdown method */
//-------------------------------------------------------------------------------------------------
void WOLLobbyMenuShutdown( WindowLayout *layout, void *userData )
{
	++s_lobbyMenuGeneration;

	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRoomsInterface != nullptr)
	{
		pRoomsInterface->DeregisterForChatCallback();
		pRoomsInterface->DeregisterForRosterNeedsRefreshCallback();
		pRoomsInterface->DeregisterForRoomChangedCallback();
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{
		pLobbyInterface->DeregisterForCreateLobbyCallback();
		pLobbyInterface->DeregisterForJoinLobbyCallback();
		pLobbyInterface->DeregisterForSearchForLobbiesCallback();
	}

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

	listboxLobbyChat = nullptr;
	listboxLobbyPlayers = nullptr;

	isShuttingDown = true;

	// if we are shutting down for an immediate pop, skip the animations
	Bool popImmediate = *(Bool *)userData;
	if( popImmediate )
	{

		shutdownComplete( layout );
		return;

	}

	TheShell->reverseAnimatewindow();
	DontShowMainMenu = FALSE;

	RaiseGSMessageBox();
	TheTransitionHandler->reverse("WOLCustomLobbyFade");

}

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
void refreshGameList( Bool forceRefresh )
{
	// TODO_NGMP: rate limit this like before
	//RefreshGameListBoxes();

	Int refreshInterval = gameListRefreshInterval;

	if (forceRefresh || ((gameListRefreshTime == 0) || ((gameListRefreshTime + refreshInterval) <= timeGetTime())))
	{
#if defined(GENERALS_ONLINE)
		RefreshGameListBoxes();
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		if (pLobbyInterface != nullptr)
		{
			pLobbyInterface->ConsumeLobbyListDirtyFlag();
		}
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
		//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
	}
}
//-------------------------------------------------------------------------------------------------
/** refreshPlayerList
		The Bool is used to force refresh if the refresh button was hit.*/
//-------------------------------------------------------------------------------------------------
void refreshPlayerList( Bool forceRefresh )
{
		Int refreshInterval = playerListRefreshInterval;

		if (forceRefresh ||((playerListRefreshTime == 0) || ((playerListRefreshTime + refreshInterval) <= timeGetTime())))
		{
				PopulateLobbyPlayerListbox();
				playerListRefreshTime = timeGetTime();
		}
		else
		{
				// fetches only if the visible window moved
				EnsureVisibleLobbyStats(FALSE);
		}
}

void ExitState()
{
	if (s_tryingToHostOrJoin)
		return;

	// Leave any group room, then pop off the screen
	auto pOnlineServicesManager = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesManager != nullptr)
	{
		NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
		if (pRoomsInterface != nullptr)
		{
			pRoomsInterface->LeaveRoom();
		}
	}

	SetLobbyAttemptHostJoin(TRUE); // pretend, since we don't want to queue up another action
	buttonPushed = true;

	if (pOnlineServicesManager == nullptr || pOnlineServicesManager->IsPendingFullTeardown()) // go back to the front end
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
			TheWindowManager->winSetFocus(textEntryChat);
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
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr && pLobbyInterface->IsLobbyListDirty() && !isShuttingDown && !buttonPushed && !pLobbyInterface->IsInLobby() && pLobbyInterface->GetLobbyTryingToJoin().lobbyID == -1)
	{
		const bool bShouldAutoRefresh = true;

		if (bShouldAutoRefresh)
		{
			refreshGameList(false);
		}
		else
		{
			GadgetListBoxAddEntryText(listboxLobbyChat, UnicodeString(L"Your lobby list is outdated. Hit refresh to see the latest servers."), GameMakeColor(255, 194, 15, 255), -1, -1);
		}

	}

#if defined(GENERALS_ONLINE) // GO needs to tick this, so notifications disappear etc
	HandleBuddyResponses();
#endif

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
						TheGameSpyInfo->addText(msg, GameSpyColor[GSCOLOR_DEFAULT], nullptr);
					}
				}
				else
				{
					DEBUG_LOG(("WOLLobbyMenuUpdate() - joining best group room"));
					TheGameSpyInfo->joinBestGroupRoom();
				}
				PopulateLobbyFilterComboBox(comboLobbyGroupRooms);
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
								if (strcmp(hostName.str(), firstPlayer) == 0)
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
						//TheGameSpyInfo->addText( L"gameList: PEER_CLEAR", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
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
						if (resp.stagingRoomMapName.empty())
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
									else if (!resp.stagingRoomPlayerNames[i].empty())
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
								//TheGameSpyInfo->addText( L"gameList: PEER_ADD", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
							else
							{
								TheGameSpyInfo->updateStagingRoom(room);
								//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
							}
						}
						else
						{
							room.setID(resp.stagingRoom.id);
							TheGameSpyInfo->removeStagingRoom(room);
							//TheGameSpyInfo->addText( L"gameList: PEER_UPDATE FAILED", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						}
						break;
					}
					case PEER_REMOVE:
						room.setID(resp.stagingRoom.id);
						TheGameSpyInfo->removeStagingRoom(room);
						//TheGameSpyInfo->addText( L"gameList: PEER_REMOVE", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
						break;
					default:
						//TheGameSpyInfo->addText( L"gameList: Unknown", GameSpyColor[GSCOLOR_DEFAULT], listboxLobbyChat );
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
			//DEBUG_LOG(("gameListRefreshTime: %d refreshInterval: %d", gameListRefreshTime, refreshInterval));
		}
#else
	refreshGameList();
#endif
	}
}

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

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;
}

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
			}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
			{
				break;
			}

		//---------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
			{
				// if we're givin the opportunity to take the keyboard focus we must say we want it
				if( mData1 == TRUE )
					*(Bool *)mData2 = TRUE;

				return MSG_HANDLED;
			}

		//---------------------------------------------------------------------------------------------
		case GLM_SELECTED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if ( controlID == GetGameListBoxID() )
				{
					int rowSelected = mData2;
					Int lobbyID = rowSelected >= 0 ? (Int)GadgetListBoxGetItemData(control, rowSelected, 0) : 0;
					if( lobbyID >= 0 )
					{
						buttonJoin->winEnable(TRUE);
						static UnsignedInt lastFrame = 0;
						static Int lastID = -1;
						UnsignedInt now = TheGameClient->getFrame();

						PeerRequest req;
						req.peerRequestType = PeerRequest::PEERREQUEST_GETEXTENDEDSTAGINGROOMINFO;
						req.stagingRoom.id = lobbyID;

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
				}

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

				}
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
					NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
					if (pLobbyInterface == nullptr)
					{
						break;
					}

					// TODO_NGMP: Support re-ordering again
					Int selected;
					GadgetListBoxGetSelected(GetGameListBox(), &selected);
					if (selected >= 0)
					{
						Int selectedID = (Int)GadgetListBoxGetItemData(GetGameListBox(), selected);
						if (selectedID >= 0)
						{
							auto Lobby = pLobbyInterface->GetLobbyFromID(selectedID);

							if (Lobby.lobbyID == -1) // -1 is invalid
							{
								break;
							}

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

							
							pLobbyInterface->SetLobbyTryingToJoin(Lobby);

							if (Lobby.passworded)
							{
								GameSpyOpenOverlay(GSOVERLAY_GAMEPASSWORD);
							}
							else
							{
								pLobbyInterface->JoinLobby(Lobby, std::string());

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
								Bool unknownLadder = (roomToJoin->getLadderPort() && TheLadderList->findLadder(roomToJoin->getLadderIP(), roomToJoin->getLadderPort()) == nullptr);
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
							GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameInfo"), nullptr);
						}
					}
					else
					{
						GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:NoGameSelected"), nullptr);
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
					// read the user's input
					UnicodeString txtInput;
					txtInput.set(GadgetTextEntryGetText( textEntryChat ));
					txtInput.trim();
					if (txtInput.isEmpty())
					{
						GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
						break;
					}

					if (!LobbyChatRateLimitAllowsSend())
					{
						break;
					}

					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
					if (pRoomsInterface != nullptr)
					{
						pRoomsInterface->SendChatMessageToCurrentRoom(txtInput, false);
					}
				}

				break;
			}

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if (s_tryingToHostOrJoin || s_populatingLobbyCombo)
					break;

				NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();

				extern LobbyGameModeFilter theLobbyFilter;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();
				if( controlID == comboLobbyGroupRoomsID )
				{
					Int pos = -1;
					GadgetComboBoxGetSelectedPos(comboLobbyGroupRooms, &pos);
					if (pos >= 0)
					{
						Int itemData = (Int)GadgetComboBoxGetItemData(comboLobbyGroupRooms, pos);
						if (itemData == LOBBY_COMBO_SEPARATOR_ITEM_DATA)
						{
							PopulateLobbyFilterComboBox(comboLobbyGroupRooms);
						}
						else if (itemData < 0)
						{
							if (pRoomsInterface != nullptr)
							{
								const Int roomIndex = -itemData - 2;
								const std::vector<NetworkRoom>& rooms = pRoomsInterface->GetGroupRooms();
								if (roomIndex >= 0 && roomIndex < (Int)rooms.size()
									&& roomIndex != pRoomsInterface->GetCurrentRoomIndex())
								{
									theLobbyFilter = LOBBY_FILTER_ALL;
									pRoomsInterface->JoinRoom(roomIndex);
								}
							}
							PopulateLobbyFilterComboBox(comboLobbyGroupRooms);
						}
						else
						{
							theLobbyFilter = (LobbyGameModeFilter)itemData;
							refreshGameList(TRUE);
							PopulateLobbyFilterComboBox(comboLobbyGroupRooms);
						}
					}
				}
			}
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
			}

		//---------------------------------------------------------------------------------------------
		case GLM_RIGHT_CLICKED:
			{
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (controlID == listboxLobbyPlayersID)
				{
#if defined(GENERALS_ONLINE)
					RightClickStruct* rc = (RightClickStruct*)mData2;
					WindowLayout* rcLayout = NULL;
					GameWindow* rcMenu;
					if (rc->pos < 0)
					{
						GadgetListBoxSetSelected(control, -1);
						break;
					}

					// TODO_NGMP: This causes issues with duplicate names. We should have better ways of looking this up + perhaps only allow unique names
					NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
					NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
					NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
					NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
					if (pRoomsInterface != nullptr && pAuthInterface != nullptr && pStatsInterface != nullptr && pSocialInterface != nullptr)
					{

						int profileID = (int)GadgetListBoxGetItemData(listboxLobbyPlayers, rc->pos, 0);
						NetworkRoomMember* roomMember = pRoomsInterface->GetRoomMemberFromID(profileID);

						if (rc->pos >= 0)
						{
							if (roomMember != nullptr)
							{
								AsciiString aName = AsciiString(roomMember->display_name.c_str());
								int64_t localuserID = pAuthInterface->GetUserID();

								Bool isBuddy = pSocialInterface->IsUserFriend(profileID);
								if (profileID <= 0)
									rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCNoProfileMenu.wnd"));
								else
								{
									if (profileID == localuserID)
									{
										rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCLocalPlayerMenu.wnd"));
									}
									else if (isBuddy)
									{
										rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCBuddiesMenu.wnd"));
									}
									else
										rcLayout = TheWindowManager->winCreateLayout(AsciiString("Menus/RCNonBuddiesMenu.wnd"));
								}
								if (!rcLayout)
									break;

								GadgetListBoxSetSelected(control, rc->pos);

								rcMenu = rcLayout->getFirstWindow();
								rcMenu->winGetLayout()->runInit();
								rcMenu->winBringToTop();
								rcMenu->winHide(FALSE);
								setUnignoreText(rcLayout, aName, profileID);
								ICoord2D rcSize, rcPos;
								rcMenu->winGetSize(&rcSize.x, &rcSize.y);
								rcPos.x = rc->mouseX;
								rcPos.y = rc->mouseY;
								if (rc->mouseX + rcSize.x > TheDisplay->getWidth())
									rcPos.x = TheDisplay->getWidth() - rcSize.x;
								if (rc->mouseY + rcSize.y > TheDisplay->getHeight())
									rcPos.y = TheDisplay->getHeight() - rcSize.y;
								rcMenu->winSetPosition(rcPos.x, rcPos.y);

								GameSpyRCMenuData* rcData = NEW GameSpyRCMenuData;
								rcData->m_id = profileID;
								rcData->m_nick = aName;
								rcData->m_itemType = (isBuddy) ? ITEM_BUDDY : ITEM_NONBUDDY;
								rcMenu->winSetUserData((void*)rcData);
								TheWindowManager->winSetLoneWindow(rcMenu);
							}
							else if (controlID == GetGameListBoxID())
							{
								// TODO_NGMP: enable right click for ladders again
								break;

								RightClickStruct* rc = (RightClickStruct*)mData2;
								WindowLayout* rcLayout = NULL;
								GameWindow* rcMenu;
								if (rc->pos < 0)
								{
									GadgetListBoxSetSelected(control, -1);
									break;
								}

								Int selectedID = (Int)GadgetListBoxGetItemData(control, rc->pos);
								if (selectedID > 0)
								{
									StagingRoomMap* srm = TheGameSpyInfo->getStagingRoomList();
									StagingRoomMap::iterator srmIt = srm->find(selectedID);
									if (srmIt != srm->end())
									{
										GameSpyStagingRoom* theRoom = srmIt->second;
										if (!theRoom)
											break;
										const LadderInfo* linfo = TheLadderList->findLadder(theRoom->getLadderIP(), theRoom->getLadderPort());
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

											rcMenu->winSetUserData((void*)selectedID);
											TheWindowManager->winSetLoneWindow(rcMenu);
										}
									}
								}
							}
						}
					}
				}
				break;
		}
#else
					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
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
						rcLayout = TheWindowManager->winCreateLayout("Menus/RCNoProfileMenu.wnd");
					else
					{
						if (profileID == TheGameSpyInfo->getLocalProfileID())
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCLocalPlayerMenu.wnd");
						}
						else if(TheGameSpyInfo->isBuddy(profileID))
						{
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCBuddiesMenu.wnd");
							isBuddy = TRUE;
						}
						else
							rcLayout = TheWindowManager->winCreateLayout("Menus/RCNonBuddiesMenu.wnd");
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
					// TODO_NGMP: enable right click for ladders again
					break;

					RightClickStruct *rc = (RightClickStruct *)mData2;
					WindowLayout *rcLayout = nullptr;
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
								rcLayout = TheWindowManager->winCreateLayout("Menus/RCGameDetailsMenu.wnd");
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
#endif

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

				// read the user's input
				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText( textEntryChat ));
				txtInput.trim();
				if (txtInput.isEmpty())
				{
					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					break;
				}

				Bool wasRateLimited = FALSE;
				if (handleLobbySlashCommands(txtInput, &wasRateLimited))
				{
					if (!wasRateLimited)
						GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
				}
				else
				{
					if (!LobbyChatRateLimitAllowsSend())
					{
						break;
					}

					GadgetTextEntrySetText(textEntryChat, UnicodeString::TheEmptyString);
					std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
					if (pWS != nullptr)
					{
						pWS->SendData_RoomChatMessage(txtInput, false);
					}
					// TODO_NGMP: Support private message again
					//TheGameSpyInfo->sendChat( txtInput, false, listboxLobbyPlayers );
				}
				break;
			}

		//---------------------------------------------------------------------------------------------
		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;
}
