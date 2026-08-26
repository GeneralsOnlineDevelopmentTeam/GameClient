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
// FILE: LobbyObserverMenu.cpp
// Read-only pre-game lobby view. The observer is never a lobby member: this screen subscribes to
// GO's pending-observer queue over the existing websocket, renders from GET /Lobby/{id} fetches
// triggered by pushes, and hands off to the normal join machinery once the stream is live.
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(GENERALS_ONLINE)

#include "GameClient/LobbyObserverMenu.h"

#include "Common/GameEngine.h"
#include "Common/LiveObserver.h"
#include "Common/MultiplayerSettings.h"
#include "Common/PlayerTemplate.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"	// GadgetButtonSetText (map start buttons)
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/LiveGamesMenu.h"	// LiveGamesMenuEnterLiveGamesMode: re-arm the browser
#include "GameClient/LiveObserverSession.h"	// pending-session queue/cancel
#include "GameClient/MapUtil.h"		// TheMapCache / MapMetaData
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameNetwork/GameSpyOverlay.h"		// GameSpyIsOverlayOpen (password popup guard)
#include "GameNetwork/GeneralsOnline/NGMP_include.h"	// from_utf8
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GUIUtil.h"	// GetTeamUiColor
#include "GameNetwork/GameInfo.h"	// PLAYERTEMPLATE_RANDOM / PLAYERTEMPLATE_MIN

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <set>
#include <windows.h>

// The map preview + start-position buttons are drawn by the same helper the real lobby
// screens use (WOLGameSetupMenu -> positionStartSpots). Defined in SkirmishGameOptionsMenu.
void positionStartSpots(AsciiString mapName, GameWindow* buttonMapStartPositions[], GameWindow* mapWindow);
UnsignedInt GetTeamUiColor(Int teamNumber);

// ============================================================================
// Mode flag
// ============================================================================

static Bool s_observerModeActive = FALSE;
static AsciiString s_observerLobbyId;
static AsciiString s_observerLobbyName;
static AsciiString s_observerPassword;	// for a password-protected lobby; sent at stream-live handoff

void SetLobbyObserverMode(const char* lobbyId)
{
	s_observerModeActive = TRUE;
	s_observerLobbyId = (lobbyId == nullptr) ? AsciiString::TheEmptyString : AsciiString(lobbyId);
}

void SetLobbyObserverModeWithPassword(const char* lobbyId, const char* password)
{
	SetLobbyObserverMode(lobbyId);
	s_observerPassword = (password == nullptr) ? AsciiString::TheEmptyString : AsciiString(password);
}

Bool LobbyObserverModeActive(void)
{
	return s_observerModeActive;
}

// ============================================================================
// Phase machine
// ============================================================================

enum class LobbyObserverPhase
{
	kIdle,			// parked in the lobby view, waiting for the match to start
	kCountdown,		// match starting: 5..0 ticked into the chat box
	kWaiting,		// match started; waiting for the stream to go live
	kJoining,		// join queued; the shell screens pump the pending-session machinery
};

static LobbyObserverPhase s_phase = LobbyObserverPhase::kIdle;
static Int s_countdownValue = 5;
static UnsignedInt s_countdownNextMs = 0;
static UnsignedInt s_lastLobbyFetchMs = 0;
static UnsignedInt s_gameStartedAtMs = 0;
static Bool s_warnedNoStream = FALSE;
static Bool s_streamNotStartedShown = FALSE;	// amber "not started yet" line printed once
static Bool s_joinQueued = FALSE;
static UnsignedInt s_joinQueuedAtMs = 0;
static Bool s_streamLivePending = FALSE;	// stream-live arrived mid-countdown; join at 0
static Bool s_lobbyGone = FALSE;
static UnsignedInt s_lobbyGoneAtMs = 0;
static Bool s_loadingStatusShown = FALSE;	// "Loading game..." printed once, pre-roll
static Int s_lastStartCountdown = -1;		// last "Starting in Ns" printed, pre-roll

// Written by the websocket receive thread, consumed by the main thread's Update. The
// receive thread never touches gadgets, so the handoff is flags only.
static std::atomic<bool> s_refetchRequested{false};
static std::atomic<bool> s_gameStartSignal{false};
static std::atomic<bool> s_streamLiveSignal{false};

// ============================================================================
// Gadgets
// ============================================================================

static const Int MAX_OBSERVER_SLOTS = 8;

static GameWindow* s_parent = nullptr;
static GameWindow* s_mapLabel = nullptr;
static GameWindow* s_mapWindow = nullptr;
static GameWindow* s_startButtons[MAX_OBSERVER_SLOTS] = {};
static GameWindow* s_chatListbox = nullptr;
static GameWindow* s_backButton = nullptr;
static GameWindow* s_titleLabel = nullptr;
static GameWindow* s_cashCombo = nullptr;
static GameWindow* s_checkUseStats = nullptr;
static GameWindow* s_checkLimitArmies = nullptr;
static GameWindow* s_checkLimitSuperweapons = nullptr;
static Int s_streamDelay = -1;	// GO's StreamDelaySeconds; -1 = never set
static Bool s_firstLobbyFetchDone = FALSE;	// first successful /Lobby/{id} fetch happened
static Int s_delayRemaining = 0;		// this viewer's remaining broadcast-delay hold, 0 = none
static Bool s_delayHoldShown = FALSE;	// the hold line was announced in the chat
static GameWindow* s_slotCombos[MAX_OBSERVER_SLOTS] = {};
static GameWindow* s_colorCombos[MAX_OBSERVER_SLOTS] = {};
static GameWindow* s_templateCombos[MAX_OBSERVER_SLOTS] = {};
static GameWindow* s_teamCombos[MAX_OBSERVER_SLOTS] = {};

// ============================================================================
// Last known lobby state (rendered on every fetch)
// ============================================================================

static AsciiString s_mapName;
static AsciiString s_mapPath;		// raw MapPath as GO sends it (relative)
static AsciiString s_mapPathLocal;	// rewritten to a path this machine can open
static UnicodeString s_slotNames[MAX_OBSERVER_SLOTS];
static Bool s_slotOccupied[MAX_OBSERVER_SLOTS];	// player or AI
static Int s_slotUserIds[MAX_OBSERVER_SLOTS];	// GO user ids, -1 = not a player (chat colour)
static Int s_slotStates[MAX_OBSERVER_SLOTS];	// raw EPlayerType int from the JSON
static Int s_slotSides[MAX_OBSERVER_SLOTS];		// player template ids, -1 = none
static Int s_slotColors[MAX_OBSERVER_SLOTS];		// color ids, -1 = none
static Int s_slotTeams[MAX_OBSERVER_SLOTS];		// team numbers, -1 = none
static Int s_slotStartPos[MAX_OBSERVER_SLOTS];	// start positions, -1 = none
static Int s_startingCash = -1;
static Bool s_trackStats = FALSE;
static Bool s_vanillaTeams = FALSE;
static Bool s_limitSuperweapons = FALSE;
static Int s_lobbyState = -1;	// ELobbyState as int; -1 = unknown
static Bool s_isStreaming = FALSE;
static Bool s_countdownStarted = FALSE;	// lobby JSON CountdownStarted (host's countdown)
static Bool s_countdownKnown = FALSE;	// the JSON carried CountdownStarted at all (old GO omits it)
// Host kill switch for observer chat. Defaults OFF here on purpose: absent on an old GO means
// the message can never be delivered, so the box must stay dead rather than swallow input.
// (Opposite of the server's own default, deliberately - absent-means-off is the safe read.)
static Bool s_allowObserverChat = FALSE;
static GameWindow* s_chatEntry = nullptr;	// the chat entry, enabled/disabled with s_allowObserverChat
static UnsignedInt s_lastObserverChatMs = 0;	// client-side slowmode gate (courtesy; server re-checks)



// ============================================================================
// Small helpers
// ============================================================================

static void observerChat(const UnicodeString& text, Color color)
{
	if (s_chatListbox != nullptr)
		GadgetListBoxAddEntryText(s_chatListbox, text, color, -1, -1);
}

// Client-side slowmode for observer chat, mirroring WOLLobbyMenu's LobbyChatSlowmodeAllowsSend
// (3 s, red "sending too quickly" line). Purely courtesy - the server re-checks per session.
static bool observerChatSlowmodeAllowsSend()
{
	const UnsignedInt nowMs = timeGetTime();
	const UnsignedInt delta = (nowMs >= s_lastObserverChatMs) ? (nowMs - s_lastObserverChatMs) : 0;
	if (s_lastObserverChatMs != 0 && delta < 3000)
	{
		observerChat(UnicodeString(L"You are sending messages too quickly. Please wait a moment."),
			GameMakeColor(255, 0, 0, 255));
		return false;
	}
	s_lastObserverChatMs = nowMs;
	return true;
}

// Drive the chat entry from the host's AllowObserverChat flag. The entry starts dead and only
// this (via applyLobbyFetch) can bring it to life.
static void applyObserverChatEntryState()
{
	if (s_chatEntry == nullptr)
		return;
	s_chatEntry->winEnable(s_allowObserverChat ? TRUE : FALSE);
	if (!s_allowObserverChat)
		GadgetTextEntrySetText(s_chatEntry, UnicodeString::TheEmptyString);
}
// The lobby chat's receive path colours each line by the sender's slot, which it resolves from
// the lobby roster - and an observer is not a member, so that roster is empty. Resolve it from
// this screen's own /Lobby/{id} fetch instead, or every line arrives in the generic colour.
// Same thread as the fetch that fills it: the websocket dispatch runs inside WebSocket::Tick().
Int LobbyObserverSlotForUserID(Int64 userID)
{
	if (!s_observerModeActive || userID < 0)
		return -1;

	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		if (s_slotUserIds[i] >= 0 && (Int64)s_slotUserIds[i] == userID)
			return i;
	}

	return -1;
}

// GO's lobby DTOs serialize PascalCase; tolerate the camelCase spelling too so a
// serializer change cannot silently blank the screen.
static bool jsonGetInt(const nlohmann::json& obj, const char* name, int& out)
{
	if (obj.contains(name) && obj[name].is_number_integer())
	{
		out = obj[name].get<int>();
		return true;
	}
	return false;
}

static bool jsonGetBool(const nlohmann::json& obj, const char* name, bool& out)
{
	if (obj.contains(name) && obj[name].is_boolean())
	{
		out = obj[name].get<bool>();
		return true;
	}
	return false;
}

static std::string jsonGetString(const nlohmann::json& obj, const char* name)
{
	if (obj.contains(name) && obj[name].is_string())
		return obj[name].get<std::string>();
	return std::string();
}

static bool jsonGetIntFlexible(const nlohmann::json& obj, const char* pascalCase, const char* lowerCase, int& out)
{
	return jsonGetInt(obj, pascalCase, out) || jsonGetInt(obj, lowerCase, out);
}

static bool jsonGetBoolFlexible(const nlohmann::json& obj, const char* pascalCase, const char* lowerCase, bool& out)
{
	return jsonGetBool(obj, pascalCase, out) || jsonGetBool(obj, lowerCase, out);
}

static std::string jsonGetStringFlexible(const nlohmann::json& obj, const char* pascalCase, const char* lowerCase)
{
	std::string value = jsonGetString(obj, pascalCase);
	if (value.empty())
		value = jsonGetString(obj, lowerCase);
	return value;
}

/// Parse an ISO-8601 UTC timestamp ("2026-08-11T12:34:56.789Z") into a time_t (UTC epoch).
static bool parseIsoUtc(const std::string& text, time_t& out)
{
	if (text.empty())
		return false;

	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
	if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d",
		&year, &month, &day, &hour, &minute, &second) < 6)
	{
		return false;
	}

	std::tm t = {};
	t.tm_year = year - 1900;
	t.tm_mon = month - 1;
	t.tm_mday = day;
	t.tm_hour = hour;
	t.tm_min = minute;
	t.tm_sec = second;
	out = _mkgmtime(&t);	// treat as UTC, like the Z suffix says
	return out != (time_t)-1;
}

/// Fill a combo with a single read-only entry and select it.
static void setReadOnlyComboEntry(GameWindow* combo, const UnicodeString& text)
{
	if (combo == nullptr)
		return;

	GadgetComboBoxReset(combo);
	GadgetComboBoxAddEntry(combo, text, GameMakeColor(255, 255, 255, 255));
	GadgetComboBoxSetSelectedPos(combo, 0);
}

/// Select the entry carrying itemData; fall back to fallbackPos (usually the "Random" /
/// unassigned entry) when nothing matches - exactly how the real lobby settles unset slots.
static void selectComboByItemData(GameWindow* combo, void* itemData, Int fallbackPos = 0)
{
	if (combo == nullptr)
		return;

	const Int count = GadgetComboBoxGetLength(combo);
	for (Int i = 0; i < count; ++i)
	{
		if (GadgetComboBoxGetItemData(combo, i) == itemData)
		{
			GadgetComboBoxSetSelectedPos(combo, i);
			return;
		}
	}

	GadgetComboBoxSetSelectedPos(combo, fallbackPos);
}

/// Faction combo, read-only: mirror PopulatePlayerTemplateComboBox's entry list (Random +
/// one entry per SIDE, deduped), then select the slot's template by item data.
static void populateTemplateComboReadOnly(GameWindow* combo, Int side)
{
	if (combo == nullptr)
		return;

	GadgetComboBoxReset(combo);

	const Color entryColor = GameMakeColor(255, 255, 255, 255);
	Int idx = GadgetComboBoxAddEntry(combo, TheGameText->fetch("GUI:Random"), entryColor);
	GadgetComboBoxSetItemData(combo, idx, (void*)PLAYERTEMPLATE_RANDOM);

	std::set<AsciiString> seenSides;
	if (ThePlayerTemplateStore != nullptr)
	{
		for (Int c = 0; c < ThePlayerTemplateStore->getPlayerTemplateCount(); ++c)
		{
			const PlayerTemplate* fac = ThePlayerTemplateStore->getNthPlayerTemplate(c);
			if (fac == nullptr || fac->getStartingBuilding().isEmpty())
				continue;

			AsciiString sideKey;
			sideKey.format("SIDE:%s", fac->getSide().str());
			if (seenSides.find(sideKey) != seenSides.end())
				continue;
			seenSides.insert(sideKey);

			idx = GadgetComboBoxAddEntry(combo, TheGameText->fetch(sideKey), entryColor);
			GadgetComboBoxSetItemData(combo, idx, (void*)c);
		}
	}

	selectComboByItemData(combo, (void*)(intptr_t)side);
}

/// Team combo, read-only: mirror PopulateTeamComboBox's entries (Team:0 = unassigned,
/// then Team:1..4), select the slot's team by item data.
static void populateTeamComboReadOnly(GameWindow* combo, Int team)
{
	if (combo == nullptr)
		return;

	GadgetComboBoxReset(combo);

	Int idx = GadgetComboBoxAddEntry(combo, TheGameText->fetch("Team:0"), GameMakeColor(255, 255, 255, 255));
	GadgetComboBoxSetItemData(combo, idx, (void*)-1);

	for (Int c = 0; c < MAX_SLOTS / 2; ++c)
	{
		AsciiString teamKey;
		teamKey.format("Team:%d", c + 1);
		idx = GadgetComboBoxAddEntry(combo, TheGameText->fetch(teamKey.str()), GetTeamUiColor(c));
		GadgetComboBoxSetItemData(combo, idx, (void*)(intptr_t)c);
	}

	selectComboByItemData(combo, (void*)(intptr_t)team);
}

/// Color combo, read-only: mirror PopulateColorComboBox's entries (??? = unassigned, then
/// one entry per color), select the slot's color by item data.
static void populateColorComboReadOnly(GameWindow* combo, Int color)
{
	if (combo == nullptr)
		return;

	GadgetComboBoxReset(combo);

	Int idx = GadgetComboBoxAddEntry(combo, TheGameText->fetch("GUI:???"), GameMakeColor(255, 255, 255, 255));
	GadgetComboBoxSetItemData(combo, idx, (void*)-1);

	if (TheMultiplayerSettings != nullptr)
	{
		for (Int c = 0; c < TheMultiplayerSettings->getNumColors(); ++c)
		{
			MultiplayerColorDefinition* def = TheMultiplayerSettings->getColor(c);
			if (def == nullptr)
				continue;

			UnicodeString colorName;
			Bool found = FALSE;
			colorName = TheGameText->fetch(def->getTooltipName().str(), &found);
			if (!found)
				colorName.format(L"%hs", def->getTooltipName().str());

			idx = GadgetComboBoxAddEntry(combo, colorName, def->getColor());
			GadgetComboBoxSetItemData(combo, idx, (void*)(intptr_t)c);
		}
	}

	selectComboByItemData(combo, (void*)(intptr_t)color);
}

// ============================================================================
// Phase transitions
// ============================================================================

static void beginCountdown(void)
{
	if (s_phase != LobbyObserverPhase::kIdle || s_lobbyGone)
		return;

	s_countdownValue = 5;
	s_countdownNextMs = timeGetTime() + 1000;
	s_gameStartedAtMs = timeGetTime();
	s_phase = LobbyObserverPhase::kCountdown;
}

static void queueObserverJoin(void)
{
	if (s_joinQueued || s_lobbyGone || s_observerLobbyId.isEmpty())
		return;

	observerChat(UnicodeString(L"Stream is live - connecting..."), GameMakeColor(120, 255, 120, 255));

	// Queue the standard observer join, which fetches its own watch ticket. The lobby name rides
	// along so a password reprompt is titled, the password entered when this view was gated is
	// sent with the ticket request, and the delay pre-seeds the countdown during GO's hold.
	StartLiveObserverSession(s_observerLobbyId, s_observerPassword, s_observerLobbyName,
		s_streamDelay);
	s_joinQueued = TRUE;
	s_joinQueuedAtMs = timeGetTime();
	s_phase = LobbyObserverPhase::kJoining;
}

static void onStreamLive(void)
{
	if (s_lobbyGone || s_observerLobbyId.isEmpty())
		return;

	// The countdown is the player's "the match is starting" cue, so never cut it short: join the
	// moment it hits zero, or immediately when none is running.
	if (s_phase == LobbyObserverPhase::kCountdown)
	{
		s_streamLivePending = TRUE;
		return;
	}

	if (s_phase == LobbyObserverPhase::kIdle || s_phase == LobbyObserverPhase::kWaiting)
		queueObserverJoin();
}

static void onCountdownCancelled(void)
{
	// The host aborted the match-start countdown: stand this screen's countdown down too and
	// forget any stream-live that arrived mid-countdown, or it would wait forever for a stream
	// that is not coming. The lobby view stays open - the host may restart the match.
	if (s_phase != LobbyObserverPhase::kCountdown && s_phase != LobbyObserverPhase::kWaiting)
		return;

	s_streamLivePending = FALSE;
	s_phase = LobbyObserverPhase::kIdle;
	observerChat(UnicodeString(L"The match start was cancelled - waiting for the host to start"),
		GameMakeColor(255, 194, 15, 255));
}

static void doLeave(void)
{
	// Cancel a queued/connecting join first: the screen is the only place the queue can
	// be abandoned before the join timeout.
	if (s_joinQueued && LiveObserverPendingSessionActive())
		CancelLiveObserverPendingSession();

	if (!s_observerLobbyId.isEmpty())
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
			NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		if (pLobbyInterface != nullptr)
			pLobbyInterface->UnsubscribeFromLobbyObserver(_atoi64(s_observerLobbyId.str()));
	}

	// The pop re-runs ReplayMenuInit on the Watch Live browser below. The browser was
	// shut down - clearing its live-games mode - when this lobby view was pushed on top
	// of it, so re-arm the mode or the browser comes back as the legacy replay-file list.
	LiveGamesMenuEnterLiveGamesMode();

	// The shutdown hook (LobbyObserverShutdown) resets all the statics.
	TheShell->pop();
}

// ============================================================================
// Lobby state fetch + render
// ============================================================================

static void renderLobby(void)
{
	// Map name label - the same lookup the real lobby does: prefer the local map cache's display
	// name for the locally resolved map path, fall back to the path-stripped raw name. The names
	// arrive from GO as UTF-8, so convert rather than translate(), which mangles non-ASCII.
	if (s_mapLabel != nullptr)
	{
		UnicodeString displayName;
		const MapMetaData* md = TheMapCache->findMap(s_mapPathLocal);
		if (md != nullptr)
			displayName = md->m_displayName;
		else if (!s_mapName.isEmpty())
		{
			AsciiString raw = s_mapName;
			const char* slash = raw.reverseFind('\\');
			if (slash != nullptr)
				raw = AsciiString(slash + 1);
			displayName = from_utf8(raw.str()).c_str();
		}

		if (!displayName.isEmpty())
			GadgetStaticTextSetText(s_mapLabel, displayName);
	}

	// Map preview + start positions - the same call the real lobby makes. It draws the preview
	// image (or the UnknownMap placeholder) and the numbered start buttons.
	if (s_mapWindow != nullptr && !s_mapPathLocal.isEmpty())
		positionStartSpots(s_mapPathLocal, s_startButtons, s_mapWindow);

	// Paint the start buttons the way the real lobby's updateMapStartSpots does on its non-load-screen
	// branch: each occupied slot with an *assigned* position shows its player number (1..8) there, in
	// the player's team color. A slot set to random shows nothing.
	//
	// Deliberately only what the lobby itself has decided. This screen used to resolve the random
	// roll here and show the outcome, which handed observers the factions and start positions before
	// the players who are about to play them - and before the broadcast delay means anything, since
	// the pre-game lobby has no delay. The roll belongs on the load screen, where the players see it
	// too.
	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		if (s_startButtons[i] == nullptr)
			continue;
		GadgetButtonSetText(s_startButtons[i], UnicodeString::TheEmptyString);
		if (!s_slotOccupied[i])
			continue;

		// An observer in the lobby holds no start position, whatever the JSON carries for it -
		// same rule the load screen applies.
		if (s_slotSides[i] <= PLAYERTEMPLATE_MIN)
			continue;

		const Int posIdx = s_slotStartPos[i];
		if (posIdx < 0 || posIdx >= MAX_OBSERVER_SLOTS || s_startButtons[posIdx] == nullptr)
			continue;

		AsciiString displayNumber;
		displayNumber.format("NUMBER:%d", i + 1);
		GadgetButtonSetText(s_startButtons[posIdx], TheGameText->fetch(displayNumber));

		const UnsignedInt col = (s_slotTeams[i] >= 0)
			? GetTeamUiColor(s_slotTeams[i])
			: GameMakeColor(255, 255, 255, 255);
		GadgetTextEntrySetTextColor(s_startButtons[posIdx], col);
	}

	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		// Player combo: every slot shows something - a name, an AI level or Open/Closed -
		// exactly like the real lobby.
		if (s_slotCombos[i] != nullptr)
		{
			GadgetComboBoxReset(s_slotCombos[i]);
			if (!s_slotNames[i].isEmpty())
			{
				GadgetComboBoxAddEntry(s_slotCombos[i], s_slotNames[i], GameMakeColor(255, 255, 255, 255));
				GadgetComboBoxSetSelectedPos(s_slotCombos[i], 0);
			}
		}

		// Faction/color/team only mean something for an occupied slot. An empty slot gets the
		// real lobby's look - Random army, no color, no team - not the layout's placeholders.
		if (!s_slotOccupied[i])
		{
			populateTemplateComboReadOnly(s_templateCombos[i], -1);
			populateColorComboReadOnly(s_colorCombos[i], -1);
			populateTeamComboReadOnly(s_teamCombos[i], -1);
			continue;
		}

		// Whatever the lobby says, and nothing more: a slot set to random reads "Random" here, exactly
		// as it does for the players sitting in it. What it rolls into is load-screen news.
		populateTemplateComboReadOnly(s_templateCombos[i], s_slotSides[i]);
		populateColorComboReadOnly(s_colorCombos[i], s_slotColors[i]);
		populateTeamComboReadOnly(s_teamCombos[i], s_slotTeams[i]);
	}

	if (s_cashCombo != nullptr && s_startingCash >= 0)
	{
		UnicodeString text;
		text.format(TheGameText->fetch("GUI:StartingMoneyFormat"), s_startingCash);
		setReadOnlyComboEntry(s_cashCombo, text);
	}

	// Lobby options, read-only display (the controls themselves are disabled).
	if (s_checkUseStats != nullptr)
		GadgetCheckBoxSetChecked(s_checkUseStats, s_trackStats);
	if (s_checkLimitArmies != nullptr)
		GadgetCheckBoxSetChecked(s_checkLimitArmies, s_vanillaTeams);
	if (s_checkLimitSuperweapons != nullptr)
		GadgetCheckBoxSetChecked(s_checkLimitSuperweapons, s_limitSuperweapons);

	if (s_titleLabel != nullptr && !s_observerLobbyName.isEmpty())
	{
		UnicodeString lobbyName(from_utf8(s_observerLobbyName.str()).c_str());
		GadgetStaticTextSetText(s_titleLabel, lobbyName);
	}

	// Fallbacks for a missed push: the state/IsStreaming flags arrive on every fetch.
	if (s_lobbyState == 1 /* ELobbyState::INGAME */)
		beginCountdown();
	if (s_isStreaming)
		onStreamLive();

	// The countdown is lobby state (CountdownStarted in the JSON), so the refetch drives this
	// screen: start when the host starts, stand down when cancelled. The INGAME guard stops the
	// start-of-match clear reading as a cancel, and the grace window stops a refetch that was
	// already in flight when the countdown started from standing it down on stale state.
	if (s_countdownKnown && s_countdownStarted)
	{
		if (s_phase == LobbyObserverPhase::kIdle)
			beginCountdown();
	}
	else if (s_countdownKnown && s_lobbyState != 1
		&& (s_phase == LobbyObserverPhase::kCountdown || s_phase == LobbyObserverPhase::kWaiting)
		&& s_gameStartedAtMs != 0 && (timeGetTime() - s_gameStartedAtMs) > 1500)
	{
		onCountdownCancelled();
	}

	// Broadcast-delay hold: GO holds this viewer's ticket until the match has run for the host's
	// delay, and the queued join waits it out in its retry loop. Announced once, so the wait in
	// the lobby view has something to look at.
	if (s_delayRemaining > 0 && s_phase != LobbyObserverPhase::kIdle && !s_delayHoldShown)
	{
		s_delayHoldShown = TRUE;
		UnicodeString text;
		text.format(L"Broadcast delay: %ds - joining automatically when it ends", s_delayRemaining);
		observerChat(text, GameMakeColor(255, 194, 15, 255));
	}
}

static void applyLobbyFetch(Bool success, Int statusCode, const AsciiString& body)
{
	if (statusCode == 404)
	{
		// The lobby is gone - the pre-game was cancelled or the lobby closed. Leave the
		// view after a short pause so the player sees why.
		if (!s_lobbyGone)
		{
			s_lobbyGone = TRUE;
			s_lobbyGoneAtMs = timeGetTime();
			observerChat(UnicodeString(L"The lobby is no longer available - returning to Watch Live"),
				GameMakeColor(255, 120, 120, 255));
		}
		return;
	}

	if (!success || statusCode != 200)
	{
		observerChat(UnicodeString(L"Could not reach GeneralsOnline"), GameMakeColor(255, 120, 120, 255));
		return;
	}

	try
	{
		nlohmann::json response = nlohmann::json::parse(body.str());
		if (!response.is_object())
			return;

		// GET /Lobby/{id} wraps the lobby in a result envelope: { "lobby": {...} }.
		if (response.contains("lobby") && response["lobby"].is_object())
			response = response["lobby"];

		const std::string mapName = jsonGetStringFlexible(response, "MapName", "mapName");
		if (!mapName.empty())
			s_mapName = mapName.c_str();

		const std::string mapPath = jsonGetStringFlexible(response, "MapPath", "mapPath");
		if (!mapPath.empty())
			s_mapPath = mapPath.c_str();

		// GO's MapPath is relative; the local map cache keys on full paths. Rewrite it the
		// same way the real lobby does (UpdateRoomDataCache): official maps live under
		// maps\, custom maps under the user map directory.
		bool isOfficial = false;
		jsonGetBoolFlexible(response, "IsMapOfficial", "isMapOfficial", isOfficial);
		s_mapPathLocal.clear();
		if (!s_mapPath.isEmpty())
		{
			if (isOfficial)
				s_mapPathLocal.format("maps\\%s", s_mapPath.str());
			else
			{
				AsciiString userMapDir = TheMapCache->getUserMapDir(true);
				userMapDir.toLower();
				s_mapPathLocal.format("%s\\%s", userMapDir.str(), s_mapPath.str());
			}
		}

		const std::string lobbyName = jsonGetStringFlexible(response, "Name", "name");
		if (!lobbyName.empty())
			s_observerLobbyName = lobbyName.c_str();

		for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
		{
			s_slotNames[i].clear();
			s_slotOccupied[i] = FALSE;
			s_slotStates[i] = -1;
			s_slotSides[i] = -1;
			s_slotColors[i] = -1;
			s_slotTeams[i] = -1;
			s_slotStartPos[i] = -1;
			s_slotUserIds[i] = -1;
		}

		const nlohmann::json* members = nullptr;
		if (response.contains("Members") && response["Members"].is_array())
			members = &response["Members"];
		else if (response.contains("members") && response["members"].is_array())
			members = &response["members"];

		if (members != nullptr)
		{
			for (const auto& member : *members)
			{
				if (!member.is_object())
					continue;

				Int slotIndex = -1;
				Int slotState = -1;
				Int userID = -1;
				jsonGetIntFlexible(member, "SlotIndex", "slotIndex", slotIndex);
				jsonGetIntFlexible(member, "SlotState", "slotState", slotState);
				jsonGetIntFlexible(member, "UserID", "userID", userID);

				if (slotIndex < 0 || slotIndex >= MAX_OBSERVER_SLOTS)
					continue;

				s_slotStates[slotIndex] = slotState;

				// EPlayerType: SLOT_OPEN=0, SLOT_CLOSED=1, EASY/MED/BRUTAL_AI=2/3/4,
				// SLOT_PLAYER=5. Mirror the stock lobby's slot labels.
				switch (slotState)
				{
				case 5:	// SLOT_PLAYER
					if (userID >= 0)
					{
						const std::string displayName = jsonGetStringFlexible(member, "DisplayName", "displayName");
						if (!displayName.empty())
							s_slotNames[slotIndex] = from_utf8(displayName).c_str();
						s_slotOccupied[slotIndex] = TRUE;
						s_slotUserIds[slotIndex] = userID;
						jsonGetIntFlexible(member, "Side", "side", s_slotSides[slotIndex]);
						jsonGetIntFlexible(member, "Color", "color", s_slotColors[slotIndex]);
						jsonGetIntFlexible(member, "Team", "team", s_slotTeams[slotIndex]);
						jsonGetIntFlexible(member, "StartingPosition", "startingPosition", s_slotStartPos[slotIndex]);
					}
					break;
				case 2:	// SLOT_EASY_AI
				case 3:	// SLOT_MED_AI
				case 4:	// SLOT_BRUTAL_AI
					s_slotNames[slotIndex] = TheGameText->fetch(
						slotState == 2 ? "GUI:EasyAI" : (slotState == 3 ? "GUI:MediumAI" : "GUI:HardAI"));
					s_slotOccupied[slotIndex] = TRUE;
					jsonGetIntFlexible(member, "Side", "side", s_slotSides[slotIndex]);
					jsonGetIntFlexible(member, "Color", "color", s_slotColors[slotIndex]);
					jsonGetIntFlexible(member, "Team", "team", s_slotTeams[slotIndex]);
					jsonGetIntFlexible(member, "StartingPosition", "startingPosition", s_slotStartPos[slotIndex]);
					break;
				case 0:	// SLOT_OPEN
					s_slotNames[slotIndex] = TheGameText->fetch("GUI:Open");
					break;
				case 1:	// SLOT_CLOSED
					s_slotNames[slotIndex] = TheGameText->fetch("GUI:Closed");
					break;
				}
			}
		}

		// Slots GO did not enumerate at all read as open, so nothing shows "entry".
		for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
		{
			if (s_slotNames[i].isEmpty())
				s_slotNames[i] = TheGameText->fetch("GUI:Open");
		}

		jsonGetIntFlexible(response, "State", "state", s_lobbyState);
		jsonGetIntFlexible(response, "StartingCash", "startingCash", s_startingCash);
		// StreamDelaySeconds is null until the host has chosen a broadcast delay; the JSON
		// null fails the is_number_integer check and leaves -1.
		jsonGetIntFlexible(response, "StreamDelaySeconds", "streamDelaySeconds", s_streamDelay);
		bool bCountdown = false;
		// An older GO omits CountdownStarted entirely, and the state rules must stay inert then -
		// otherwise every refetch reads "not counting" and instantly stands the countdown down.
		// The GAME_STARTING push still drives it in that case.
		s_countdownKnown = jsonGetBoolFlexible(response, "CountdownStarted", "countdownStarted", bCountdown) ? TRUE : FALSE;
		s_countdownStarted = bCountdown ? TRUE : FALSE;
		bool bTrackStats = false;
		jsonGetBoolFlexible(response, "IsTrackingStats", "isTrackingStats", bTrackStats);
		s_trackStats = bTrackStats ? TRUE : FALSE;
		bool bVanillaTeams = false;
		jsonGetBoolFlexible(response, "IsVanillaTeamsOnly", "isVanillaTeamsOnly", bVanillaTeams);
		s_vanillaTeams = bVanillaTeams ? TRUE : FALSE;
		bool bLimitSuperweapons = false;
		jsonGetBoolFlexible(response, "IsLimitSuperweapons", "isLimitSuperweapons", bLimitSuperweapons);
		s_limitSuperweapons = bLimitSuperweapons ? TRUE : FALSE;
		bool streaming = false;
		jsonGetBoolFlexible(response, "IsStreaming", "isStreaming", streaming);
		s_isStreaming = streaming ? TRUE : FALSE;
		// Host kill switch for observer chat. Absent (old GO) -> stays false, so the entry
		// remains dead; a refetch that flips it enables/disables the entry and says so.
		bool bAllowObserverChat = false;
		jsonGetBoolFlexible(response, "AllowObserverChat", "allowObserverChat", bAllowObserverChat);
		if (s_allowObserverChat != (bAllowObserverChat ? TRUE : FALSE))
		{
			s_allowObserverChat = bAllowObserverChat ? TRUE : FALSE;
			observerChat(s_allowObserverChat
					? UnicodeString(L"Observer chat is enabled")
					: UnicodeString(L"Observer chat has been disabled by the host"),
				GameMakeColor(255, 194, 15, 255));
			applyObserverChatEntryState();
		}

		// The remaining hold is derived, not serialized: GO computes it in the livestream
		// controller as TimeMatchStarted + StreamDelaySeconds, and this repeats that from the two
		// fields the lobby JSON does carry. Zero when not held.
		s_delayRemaining = 0;
		time_t matchStarted = 0;
		if (s_streamDelay > 0 &&
			parseIsoUtc(jsonGetStringFlexible(response, "TimeMatchStarted", "timeMatchStarted"), matchStarted))
		{
			const Int elapsed = (Int)(time(nullptr) - matchStarted);
			s_delayRemaining = (elapsed < s_streamDelay) ? (s_streamDelay - elapsed) : 0;
		}

		// Entered a game that is already started (the "STARTED / wait" row from the browser): skip
		// the match-starting countdown and wait for the stream, or for the end of the hold,
		// directly. The GAME_STARTING push fired before we subscribed and cannot arrive any more.
		if (!s_firstLobbyFetchDone)
		{
			s_firstLobbyFetchDone = TRUE;
			if (s_lobbyState == 1 /* ELobbyState::INGAME */ && s_phase == LobbyObserverPhase::kIdle)
			{
				observerChat(UnicodeString(L"Game already started - waiting for the stream"),
					GameMakeColor(200, 200, 200, 255));
				s_gameStartedAtMs = timeGetTime();
				s_phase = LobbyObserverPhase::kWaiting;
			}
		}

		renderLobby();
	}
	catch (const nlohmann::json::exception&)
	{
	}
}

// ============================================================================
// Public entry points (called by the observer-mode branch of WOLGameSetupMenu)
// ============================================================================

void LobbyObserverInit(WindowLayout* layout, void* userData)
{
	s_parent = TheWindowManager->winGetWindowFromId(nullptr,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:GameSpyGameOptionsMenuParent"));
	if (s_parent == nullptr)
		return;

	s_mapLabel = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:TextEntryMapDisplay"));
	s_mapWindow = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:MapWindow"));
	s_chatListbox = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:ListboxChatWindowGameSpyGameSetup"));
	s_backButton = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:ButtonBack"));
	s_titleLabel = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:StaticTextGameName"));
	s_cashCombo = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:ComboBoxStartingCash"));
	s_checkUseStats = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:CheckBoxUseStats"));
	s_checkLimitArmies = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:CheckBoxLimitArmies"));
	s_checkLimitSuperweapons = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:CheckboxLimitSuperweapons"));

	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		AsciiString tmp;
		tmp.format("GameSpyGameOptionsMenu.wnd:ComboBoxPlayer%d", i);
		s_slotCombos[i] = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));

		tmp.format("GameSpyGameOptionsMenu.wnd:ComboBoxColor%d", i);
		s_colorCombos[i] = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));

		tmp.format("GameSpyGameOptionsMenu.wnd:ComboBoxPlayerTemplate%d", i);
		s_templateCombos[i] = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));

		tmp.format("GameSpyGameOptionsMenu.wnd:ComboBoxTeam%d", i);
		s_teamCombos[i] = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));

		tmp.format("GameSpyGameOptionsMenu.wnd:ButtonMapStartPosition%d", i);
		s_startButtons[i] = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));
	}

	// The observer screen is a read-only lobby: every interactive control is dead, the
	// Back button is the one live button (relabelled LEAVE).
	static const char* const kDisabledControls[] =
	{
		"GameSpyGameOptionsMenu.wnd:ButtonStart",
		"GameSpyGameOptionsMenu.wnd:ButtonSelectMap",
		"GameSpyGameOptionsMenu.wnd:ButtonEmote",
		"GameSpyGameOptionsMenu.wnd:ButtonCommunicator",
		"GameSpyGameOptionsMenu.wnd:CheckBoxUseStats",
		"GameSpyGameOptionsMenu.wnd:CheckboxLimitSuperweapons",
		"GameSpyGameOptionsMenu.wnd:CheckBoxLimitArmies",
		nullptr
	};
	for (Int i = 0; kDisabledControls[i] != nullptr; ++i)
	{
		GameWindow* win = TheWindowManager->winGetWindowFromId(s_parent,
			TheNameKeyGenerator->nameToKey(kDisabledControls[i]));
		if (win != nullptr)
			win->winEnable(FALSE);
	}
	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		if (s_colorCombos[i] != nullptr)
			s_colorCombos[i]->winEnable(FALSE);
		if (s_templateCombos[i] != nullptr)
			s_templateCombos[i]->winEnable(FALSE);
		if (s_teamCombos[i] != nullptr)
			s_teamCombos[i]->winEnable(FALSE);

		AsciiString tmp;
		tmp.format("GameSpyGameOptionsMenu.wnd:ButtonAccept%d", i);
		GameWindow* win = TheWindowManager->winGetWindowFromId(s_parent, TheNameKeyGenerator->nameToKey(tmp));
		if (win != nullptr)
			win->winHide(TRUE);
	}

	if (s_backButton != nullptr)
		s_backButton->winSetText(UnicodeString(L"LEAVE"));
	// The click arrives via WOLGameSetupMenuSystem -> LobbyObserverInput: a button's GBM_SELECTED
	// goes to its owner chain, never to the button's own system function, so the button must keep
	// GadgetPushButtonSystem.

	// Subscribe to the pending-observer queue and to the pushes. The callback runs on the
	// websocket thread: it only raises flags, all UI work happens in Update.
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{
		pLobbyInterface->RegisterForLobbyObserverEvent(
			[](NGMP_OnlineServices_LobbyInterface::ELobbyObserverEventType eventType, int64_t lobbyId)
			{
				if (s_observerLobbyId.isEmpty() || lobbyId != _atoi64(s_observerLobbyId.str()))
					return;

				switch (eventType)
				{
				case NGMP_OnlineServices_LobbyInterface::ELobbyObserverEventType::LOBBY_CHANGED:
					s_refetchRequested.store(true);
					break;
				case NGMP_OnlineServices_LobbyInterface::ELobbyObserverEventType::GAME_STARTING:
					s_gameStartSignal.store(true);
					break;
				case NGMP_OnlineServices_LobbyInterface::ELobbyObserverEventType::STREAM_LIVE:
					// The relay confirmed it holds the streamer's header. This is the only
					// liveness signal; never guess "live" from the match having started.
					s_streamLiveSignal.store(true);
					break;
				case NGMP_OnlineServices_LobbyInterface::ELobbyObserverEventType::GAME_STARTED:
					// Deliberately ignored: a started match says nothing about a stream, since
					// everyone may have streaming off. Wait for STREAM_LIVE or the IsStreaming
					// flag on the lobby fetch.
					break;
				}
			});

		// The members' lobby chat, read-only. GO fans it out to pending observers as well, and the
		// receive path has no membership check, so this listbox is the only thing that was missing.
		// The dispatch runs inside WebSocket::Tick(), i.e. on this thread, exactly like the real
		// lobby's own chat callback (WOLGameSetupMenu) - no handoff needed.
		pLobbyInterface->RegisterForChatCallback([](UnicodeString strMessage, Color color)
			{
				observerChat(strMessage, color);
			});

		pLobbyInterface->SubscribeToLobbyObserver(_atoi64(s_observerLobbyId.str()));
	}

	// The chat entry is the observer's own send box, live only while the host allows observer
	// chat; keep it empty rather than the layout's placeholder text. Starts dead until the
	// first /Lobby/{id} fetch says otherwise (s_allowObserverChat defaults FALSE).
	s_chatEntry = TheWindowManager->winGetWindowFromId(s_parent,
		TheNameKeyGenerator->nameToKey("GameSpyGameOptionsMenu.wnd:TextEntryChat"));
	if (s_chatEntry != nullptr)
		GadgetTextEntrySetText(s_chatEntry, UnicodeString::TheEmptyString);
	applyObserverChatEntryState();

	// Intro line, then the first fetch.
	observerChat(UnicodeString(L"Waiting for the match to start..."), GameMakeColor(200, 200, 200, 255));

	s_lastLobbyFetchMs = 0;
	layout->hide(FALSE);
	TheWindowManager->winSetFocus(s_parent);
}

void LobbyObserverUpdate(WindowLayout* layout, void* userData)
{
	const UnsignedInt now = timeGetTime();

	// -- Lobby fetch pump (triggered by pings, throttled, with a 30s safety net) --
	AsciiString body;
	Bool fetchOk = FALSE;
	Int statusCode = 0;
	if (liveRelayPollFetch(body, fetchOk, statusCode))
		applyLobbyFetch(fetchOk, statusCode, body);

	const Bool refetchNow = s_refetchRequested.exchange(false);
	const UnsignedInt sinceLast = (now > s_lastLobbyFetchMs) ? (now - s_lastLobbyFetchMs) : 0;
	if (!liveRelayFetchInFlight() && (refetchNow ? sinceLast >= 1000 : sinceLast >= 30000))
	{
		s_lastLobbyFetchMs = now;
		AsciiString url;
		url.format("%s/%s", liveServicesEndpoint("Lobby").str(), s_observerLobbyId.str());
		liveRelayBeginFetch(url);
	}

	// -- Push signals --
	if (s_gameStartSignal.exchange(false))
		beginCountdown();
	if (s_streamLiveSignal.exchange(false))
		onStreamLive();

	// -- Countdown ticks --
	if (s_phase == LobbyObserverPhase::kCountdown && now >= s_countdownNextMs)
	{
		--s_countdownValue;
		if (s_countdownValue > 0)
		{
			s_countdownNextMs = now + 1000;
		}
		else
		{
			observerChat(UnicodeString(L"Match starting - waiting for the stream"),
				GameMakeColor(255, 194, 15, 255));
			s_phase = LobbyObserverPhase::kWaiting;
			if (s_streamLivePending)
			{
				s_streamLivePending = FALSE;
				onStreamLive();
			}
		}
	}

	// Join handoff. The pending-session pump belongs to the shell screens below, so this screen
	// must neither consume it nor pop itself when playback starts: a normal game start never pops
	// its setup menu (MSG_NEW_GAME's hideShell hides it, and the game-end re-init handles the
	// stale screen). Popping here re-inits the Watch Live browser underneath, which then renders
	// on top of the running game.
	if (s_phase == LobbyObserverPhase::kJoining)
	{
		// While GO holds the watch ticket the observer is not connected yet, so the pre-roll
		// block below has nothing to show. Report the remaining hold in the chat instead.
		if (TheLiveObserver != nullptr && TheLiveObserver->isWaitingForBroadcastDelay())
		{
			const Int seconds = TheLiveObserver->getBroadcastDelayRemainingSeconds();
			if (seconds != s_lastStartCountdown)
			{
				s_lastStartCountdown = seconds;
				UnicodeString text;
				text.format(L"Broadcast delay: %ds", seconds);
				observerChat(text, GameMakeColor(255, 194, 15, 255));
			}
		}
		// Pre-roll status in the chat: while the relayed stream has no complete frames yet the
		// players are still loading, and once frames flow this reports the remaining broadcast
		// delay. This screen stays up for the whole wait, so the chat is where it belongs.
		else if (TheLiveObserver != nullptr && TheLiveObserver->isConnected() && !TheLiveObserver->hasPlaybackStarted())
		{
			if (TheLiveObserver->getMaxCompleteFrame() == 0)
			{
				if (!s_loadingStatusShown)
				{
					s_loadingStatusShown = TRUE;
					observerChat(UnicodeString(L"Loading game..."), GameMakeColor(255, 194, 15, 255));
				}
			}
			else
			{
				const Int seconds = TheLiveObserver->getSecondsUntilPlaybackReady();
				if (seconds != s_lastStartCountdown)
				{
					s_lastStartCountdown = seconds;
					UnicodeString text;
					text.format(L"Starting in %ds", seconds);
					observerChat(text, GameMakeColor(255, 194, 15, 255));
				}
			}
		}

		if (!LiveObserverPendingSessionActive() && (now - s_joinQueuedAtMs) > 3000 &&
			!GameSpyIsOverlayOpen(GSOVERLAY_GAMEPASSWORD))
		{
			// The pump gave up (join timeout, or the stream never materialised) and cleared the
			// queue itself, so drop back to the lobby view. The password reprompt popup
			// suppresses this for its own duration.
			observerChat(UnicodeString(L"Timed out waiting for the stream - the host may not be streaming"),
				GameMakeColor(255, 120, 120, 255));
			s_joinQueued = FALSE;
			s_phase = LobbyObserverPhase::kIdle;
		}
	}

	// Host-never-streams warning: the match started but the relay reports no stream. Two stages,
	// because GO's match-start is the host's START_GAME - before any client has loaded the map -
	// while the streamer registers only once in-game, so a slow load easily outlasts 20s. At 60s
	// it is not coming, which is also when GO drops the game from Watch Live. A live stream or a
	// held join keeps s_joinQueued / s_isStreaming true and trips neither.
	if (s_gameStartedAtMs != 0 && !s_joinQueued &&
		s_lobbyState == 1 /* ELobbyState::INGAME */ && !s_isStreaming)
	{
		if ((now - s_gameStartedAtMs) > 60000)
		{
			if (!s_warnedNoStream)
			{
				s_warnedNoStream = TRUE;
				observerChat(UnicodeString(L"The stream has not started - the host may not be streaming. LEAVE to stop waiting."),
					GameMakeColor(255, 120, 120, 255));
			}
		}
		else if ((now - s_gameStartedAtMs) > 20000 && !s_streamNotStartedShown)
		{
			s_streamNotStartedShown = TRUE;
			observerChat(UnicodeString(L"The stream has not started yet - waiting for the host to start streaming"),
				GameMakeColor(255, 194, 15, 255));
		}
	}

	// -- Lobby gone -> auto-return --
	if (s_lobbyGone && (now - s_lobbyGoneAtMs) > 2000)
		doLeave();
}

void LobbyObserverShutdown(WindowLayout* layout, void* userData)
{
	// The join hand-off also lands here, because the shell tears down when the game starts. By
	// then the pending-session pump has cleared its own queue, so the cancel below only fires for
	// a real leave or an aborted join.
	if (s_joinQueued && LiveObserverPendingSessionActive())
		CancelLiveObserverPendingSession();

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{
		pLobbyInterface->DeregisterForLobbyObserverEvent();
		// The listbox this writes into is torn down below, so drop the callback first.
		pLobbyInterface->DeregisterForChatCallback();
		if (!s_observerLobbyId.isEmpty())
			pLobbyInterface->UnsubscribeFromLobbyObserver(_atoi64(s_observerLobbyId.str()));
	}

	s_observerModeActive = FALSE;
	s_observerLobbyId.clear();
	s_observerLobbyName.clear();
	s_observerPassword.clear();
	s_phase = LobbyObserverPhase::kIdle;
	s_joinQueued = FALSE;
	s_streamLivePending = FALSE;
	s_lobbyGone = FALSE;
	s_warnedNoStream = FALSE;
	s_streamNotStartedShown = FALSE;
	s_loadingStatusShown = FALSE;
	s_lastStartCountdown = -1;
	s_gameStartedAtMs = 0;
	s_mapName.clear();
	s_mapPath.clear();
	s_mapPathLocal.clear();
	s_startingCash = -1;
	s_trackStats = FALSE;
	s_vanillaTeams = FALSE;
	s_limitSuperweapons = FALSE;
	s_lobbyState = -1;
	s_isStreaming = FALSE;
	s_countdownStarted = FALSE;
	s_countdownKnown = FALSE;
	s_allowObserverChat = FALSE;
	s_lastObserverChatMs = 0;
	s_streamDelay = -1;
	s_firstLobbyFetchDone = FALSE;
	s_delayRemaining = 0;
	s_delayHoldShown = FALSE;
	s_refetchRequested.store(false);
	s_gameStartSignal.store(false);
	s_streamLiveSignal.store(false);

	s_parent = nullptr;
	s_mapLabel = nullptr;
	if (s_mapWindow != nullptr)
		s_mapWindow->winSetUserData(nullptr);
	s_mapWindow = nullptr;
	s_chatListbox = nullptr;
	s_backButton = nullptr;
	s_titleLabel = nullptr;
	s_cashCombo = nullptr;
	s_checkUseStats = nullptr;
	s_checkLimitArmies = nullptr;
	s_checkLimitSuperweapons = nullptr;
	s_chatEntry = nullptr;
	for (Int i = 0; i < MAX_OBSERVER_SLOTS; ++i)
	{
		s_slotCombos[i] = nullptr;
		s_colorCombos[i] = nullptr;
		s_templateCombos[i] = nullptr;
		s_teamCombos[i] = nullptr;
		s_startButtons[i] = nullptr;
		s_slotNames[i].clear();
		s_slotOccupied[i] = FALSE;
		s_slotSides[i] = -1;
		s_slotColors[i] = -1;
		s_slotTeams[i] = -1;
		s_slotStates[i] = -1;
		s_slotStartPos[i] = -1;
		s_slotUserIds[i] = -1;
	}

	// Complete the shell's pop/shutdown. The stock setup menu finishes this from its own update
	// loop (reverseAnimateWindow -> isAnimFinished -> shutdownComplete), but observer mode skips
	// the animations, so without this the pending pop never completes and the layout stays on the
	// stack on top of the running game.
	if (layout != nullptr)
	{
		layout->hide(TRUE);
		TheShell->shutdownComplete(layout);
	}
}

WindowMsgHandledType LobbyObserverInput(GameWindow* window, UnsignedInt msg,
	WindowMsgData mData1, WindowMsgData mData2)
{
	switch (msg)
	{
		case GBM_SELECTED:
		{
			GameWindow* control = (GameWindow*)mData1;
			if (control != nullptr && control == s_backButton)
			{
				doLeave();
				return MSG_HANDLED;
			}
			break;
		}

		case GEM_EDIT_DONE:
		{
			// The observer's own chat entry. The server re-broadcasts the line as an ordinary
			// LOBBY_CHAT_FROM_SERVER with [Name] formatting, so no local echo is needed.
			GameWindow* control = (GameWindow*)mData1;
			if (control != nullptr && s_chatEntry != nullptr && control == s_chatEntry)
			{
				if (!s_allowObserverChat)
					return MSG_HANDLED;

				UnicodeString txtInput;
				txtInput.set(GadgetTextEntryGetText(s_chatEntry));
				GadgetTextEntrySetText(s_chatEntry, UnicodeString::TheEmptyString);
				txtInput.trim();
				if (!txtInput.isEmpty() && observerChatSlowmodeAllowsSend())
				{
					NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
						NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
					if (pLobbyInterface != nullptr && !s_observerLobbyId.isEmpty())
					{
						pLobbyInterface->SendObserverChatMessage(_atoi64(s_observerLobbyId.str()), txtInput);
					}
				}
				return MSG_HANDLED;
			}
			break;
		}

		case GWM_CHAR:
		{
			UnsignedByte key = (UnsignedByte)mData1;
			UnsignedByte state = (UnsignedByte)mData2;
			if (key == KEY_ESC && BitIsSet(state, KEY_STATE_UP))
			{
				doLeave();
				return MSG_HANDLED;
			}
			break;
		}
	}

	return MSG_IGNORED;
}

#endif // defined(GENERALS_ONLINE)
