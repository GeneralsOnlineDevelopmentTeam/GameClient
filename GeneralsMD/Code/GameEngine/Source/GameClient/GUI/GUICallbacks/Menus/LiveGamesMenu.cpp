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
// FILE: LiveGamesMenu.cpp
// The Watch Live browser. Reached from Online -> Watch Live, which pushes Menus/ReplayMenu.wnd
// in live-games mode; this module owns that mode. The layout is shared with the replay-file
// menu, so the mode is guarded by s_liveGamesMode and restored on shutdown.
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(GENERALS_ONLINE)

#include "GameClient/LiveGamesMenu.h"

#include "Common/LiveObserver.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GUICallbacks.h"	// liveWatchOpenPasswordPopup
#include "GameClient/LiveObserverSession.h"	// StartLiveObserverSession
#include "GameClient/MessageBox.h"
#include "GameClient/Shell.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/LobbyObserverMenu.h"	// SetLobbyObserverMode opens the read-only lobby view

#include <vector>
#include <windows.h>

// ============================================================================
// State
// ============================================================================

// This screen is reused rather than duplicated: the .wnd layouts live inside an archive we cannot
// read to copy one from, and borrowing this one gives the real frame, listbox, scrollbar and
// hover states for free. The cost is that both modes share one control set, so everything below
// is guarded by s_liveGamesMode and the replay behaviour is untouched when it is off.
static Bool s_liveGamesMode = FALSE;
static std::vector<AsciiString> s_liveGameIds;	///< game id per listbox row
static std::vector<Bool> s_liveGameIsLive;		///< TRUE = join now (watch_action 2)
static std::vector<Bool> s_liveGamePassworded;	///< stream (or lobby) is password protected
static std::vector<AsciiString> s_liveGameNames;	///< lobby display name, for the password popup
static std::vector<Int> s_liveGameDelaySeconds;	///< broadcast delay per row, -1 = unknown
static UnsignedInt s_lastLiveFetchMs = 0;
static GameWindow* s_liveTitleWindow = nullptr;
static UnicodeString s_savedTitleText;
static UnicodeString s_savedLoadText;
static UnicodeString s_savedDeleteText;

enum { LIVE_GAMES_REFRESH_INTERVAL_MS = 5000 };

// The module's own view of the shared controls, resolved in LiveGamesMenuInit by the same
// name keys ReplayMenuInit uses - both sides address the same windows, no conflict.
static GameWindow* s_listbox = nullptr;
static GameWindow* s_buttonLoad = nullptr;
static GameWindow* s_buttonDelete = nullptr;
static GameWindow* s_buttonCopy = nullptr;
static Int s_listboxID = 0;
static Int s_buttonLoadID = 0;
static Int s_buttonDeleteID = 0;
static Int s_buttonCopyID = 0;

// ============================================================================
// Helpers
// ============================================================================

void LiveGamesMenuEnterLiveGamesMode(void) { s_liveGamesMode = TRUE; }
Bool LiveGamesMenuIsLiveGamesMode(void) { return s_liveGamesMode; }

static void liveGamesRequestList(void);
static void liveGamesApplyResponse(Bool success, Int statusCode, const AsciiString& body);

/// Depth-first search for the first static-text window carrying any text. Must recurse: the
/// heading is not a direct child of ParentReplayMenu, since the layout nests controls under
/// GadgetParent.
static GameWindow* findFirstStaticTextWithText(GameWindow* parent)
{
	if (parent == nullptr)
		return nullptr;

	for (GameWindow* child = parent->winGetChild(); child != nullptr; child = child->winGetNext())
	{
		WinInstanceData* data = child->winGetInstanceData();
		if (data != nullptr &&
			(data->m_style & GWS_STATIC_TEXT) != 0 &&
			!child->winGetText().isEmpty())
		{
			return child;
		}

		GameWindow* nested = findFirstStaticTextWithText(child);
		if (nested != nullptr)
			return nested;
	}

	return nullptr;
}

/// Find the screen's heading. The layout is inside an unreadable archive, so the control name
/// cannot be confirmed here; try the conventional names first and only then fall back to
/// searching the subtree. The fallback is a guess - "first static text carrying text" is the
/// heading by luck, not by rule - so it logs what it settled on.
static GameWindow* findTitleWindow(GameWindow* parent)
{
	static const char* const TITLE_CONTROL_NAMES[] = {
		"ReplayMenu.wnd:StaticTextTitle",
		"ReplayMenu.wnd:StaticTextHeader",
		// ReplayMenu.wnd's heading has an empty control name - the layout declares it as
		// NAME = "ReplayMenu.wnd:" - so the two conventional names above can never match it.
		"ReplayMenu.wnd:",
		nullptr
	};

	for (Int i = 0; TITLE_CONTROL_NAMES[i] != nullptr; ++i)
	{
		GameWindow* win = TheWindowManager->winGetWindowFromId(
			parent, (Int)TheNameKeyGenerator->nameToKey(TITLE_CONTROL_NAMES[i]));
		// "ReplayMenu.wnd:" is shared by TWO controls - the heading static text and a large
		// panel. Only a static text can be the heading; accepting the first name match would
		// retitle the panel and the screen would stay LOAD REPLAY.
		if (win != nullptr && win->winGetInstanceData() != nullptr &&
			(win->winGetInstanceData()->m_style & GWS_STATIC_TEXT) != 0)
		{
			liveObserverLog("ReplayMenu: title control found by name '%s'\n", TITLE_CONTROL_NAMES[i]);
			return win;
		}
	}

	GameWindow* fallback = findFirstStaticTextWithText(parent);
	if (fallback != nullptr)
	{
		AsciiString text;
		text.translate(fallback->winGetText());
		liveObserverLog("ReplayMenu: title control not found by name; fell back to id='%s' text='%s'\n",
			KEYNAME((NameKeyType)fallback->winGetWindowId()).str(), text.str());
	}
	else
	{
		liveObserverLog("ReplayMenu: no title control found at all - heading will not be retitled\n");
	}

	return fallback;
}

static void liveGamesRequestList(void)
{
	if (liveRelayFetchInFlight())
		return;

	// GO owns the list of what is being streamed: the relay cannot tell which of its sessions a
	// given player is allowed to see.
	s_lastLiveFetchMs = timeGetTime();
	liveRelayBeginFetch(liveServicesEndpoint("Livestreams"));
}

static void liveGamesApplyResponse(Bool success, Int statusCode, const AsciiString& body)
{
	if (s_listbox == nullptr || !s_liveGamesMode)
		return;

	// Repopulating clears the selection, so remember it and restore by game id afterwards -
	// by id and not row, since a game ending shifts every row beneath it.
	AsciiString previouslySelected;
	{
		Int wasSelected = -1;
		GadgetListBoxGetSelected(s_listbox, &wasSelected);
		if (wasSelected >= 0 && wasSelected < (Int)s_liveGameIds.size())
			previouslySelected = s_liveGameIds[wasSelected];
	}

	GadgetListBoxReset(s_listbox);
	s_liveGameIds.clear();

	if (!success || statusCode != 200)
	{
		// This screen is only reachable from the Online welcome menu, so a session always exists
		// by the time the list is fetched: a failure here is a failure to reach GO, and saying
		// anything else would send the player looking for a login they have already done.
		GadgetListBoxAddEntryText(s_listbox,
			UnicodeString(L"Could not reach GeneralsOnline"),
			GameMakeColor(255, 120, 120, 255), -1);
		return;
	}

	// The wire format is LiveObserver's business; this only lays rows out.
	std::vector<LiveGameEntry> games;
	if (!liveServicesParseLivestreams(body, games))
	{
		GadgetListBoxAddEntryText(s_listbox,
			UnicodeString(L"Unexpected reply from GeneralsOnline"),
			GameMakeColor(255, 120, 120, 255), -1);
		return;
	}

	if (games.empty())
	{
		GadgetListBoxAddEntryText(s_listbox,
			UnicodeString(L"No games right now"),
			GameMakeColor(200, 200, 200, 255), -1);
		return;
	}

	// GO owns the row order (priority-player matches first, then join -> wait -> pre-game), so
	// the list is rendered exactly as delivered. Re-sorting here would undo the server's
	// priority ordering.
	for (std::vector<LiveGameEntry>::const_iterator it = games.begin(); it != games.end(); ++it)
	{
		const LiveGameEntry& game = *it;
		const Bool isLive = (game.watchAction == 2);
		const Bool isWaiting = (game.watchAction == 1);
		// Priority-player matches render gold (all row kinds) so they stand out on top of
		// the priority-first sort.
		const Color rowColor = game.priority ? GameMakeColor(255, 215, 0, 255)
			: isLive ? GameMakeColor(255, 255, 255, 255)
			: GameMakeColor(200, 200, 200, 255);
		UnicodeString text;
		AsciiString tmp;

		// Four columns, laid out for the replay list, reused as map / running-for / delay /
		// players. Append column 0 and use the row index it returns for the rest, exactly as
		// PopulateReplayFileListbox does; a precomputed row merges the cells together.
		text.translate(game.mapName);
		const Int row = GadgetListBoxAddEntryText(s_listbox, text, rowColor, -1, 0);
		if (row < 0)
			continue;

		if (isLive)
		{
			tmp.format("%dm in", game.ageSeconds / 60);
			text.translate(tmp);
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 1);

			tmp.format("%ds delay", game.delaySeconds);
			text.translate(tmp);
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 2);

			tmp.format("%s (%d watching)", game.players.str(), game.observerCount);
			text.translate(tmp);
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 3);
		}
		else if (isWaiting)
		{
			// Started game, but this viewer cannot join yet: the stream is not live, or the
			// ticket is held behind the broadcast delay. The columns read
			// map / STARTED / hold or PASSWORDED / players (N waiting).
			text = UnicodeString(L"STARTED");
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 1);

			if (game.delayRemainingSeconds > 0)
			{
				tmp.format("starts in %ds", game.delayRemainingSeconds);
				text.translate(tmp);
				GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 2);
			}
			else
			{
				text = game.passworded ? UnicodeString(L"PASSWORDED") : UnicodeString(L"");
				GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 2);
			}

			tmp.format("%s (%d waiting)", game.players.str(), game.pendingObserverCount);
			text.translate(tmp);
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 3);
		}
		else
		{
			// Pre-game lobby: nothing is running yet, so the columns read
			// map / PRE-GAME / PASSWORDED? / players (N waiting).
			text = UnicodeString(L"PRE-GAME");
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 1);

			text = game.passworded ? UnicodeString(L"PASSWORDED") : UnicodeString(L"");
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 2);

			tmp.format("%s (%d waiting)", game.players.str(), game.pendingObserverCount);
			text.translate(tmp);
			GadgetListBoxAddEntryText(s_listbox, text, rowColor, row, 3);
		}

		// Index by the row the listbox actually used, so a lookup on selection cannot
		// drift out of step with the rows if one is ever skipped.
		if ((Int)s_liveGameIds.size() <= row)
		{
			s_liveGameIds.resize(row + 1);
			s_liveGameIsLive.resize(row + 1);
			s_liveGamePassworded.resize(row + 1);
			s_liveGameNames.resize(row + 1);
			s_liveGameDelaySeconds.resize(row + 1);
		}
		s_liveGameIds[row] = game.lobbyId;
		s_liveGameIsLive[row] = isLive;
		s_liveGamePassworded[row] = game.passworded;
		s_liveGameNames[row] = game.name;
		s_liveGameDelaySeconds[row] = game.delaySeconds;
	}

	if (!previouslySelected.isEmpty())
	{
		for (Int i = 0; i < (Int)s_liveGameIds.size(); ++i)
		{
			if (s_liveGameIds[i] == previouslySelected)
			{
				GadgetListBoxSetSelected(s_listbox, i);
				break;
			}
		}
	}
}

/// Connect to the selected game, or open the read-only observer view for a pre-game lobby.
/// Returns TRUE if an action was started.
static Bool liveGamesConnectSelected(void)
{
	Int selected = -1;
	GadgetListBoxGetSelected(s_listbox, &selected);
	if (selected < 0 || selected >= (Int)s_liveGameIds.size())
		return FALSE;

	// A password-protected live stream asks for the password before the session is queued. The
	// browser stays up behind the modal popup, exactly like custom games.
	if (selected < (Int)s_liveGameIsLive.size() && s_liveGameIsLive[selected] &&
		selected < (Int)s_liveGamePassworded.size() && s_liveGamePassworded[selected])
	{
		const AsciiString& displayName = (selected < (Int)s_liveGameNames.size())
			? s_liveGameNames[selected] : AsciiString::TheEmptyString;
		liveWatchOpenPasswordPopup(s_liveGameIds[selected], displayName, TRUE);
		return TRUE;
	}

	if (selected < (Int)s_liveGameIsLive.size() && !s_liveGameIsLive[selected])
	{
		// Pre-game lobby, or a started game this viewer cannot join yet: open the read-only
		// observer view as the waiting room. It subscribes to the pending-observer queue itself
		// and hands off to StartLiveObserverSession once the stream goes live or the hold ends.
		// A passworded lobby is gated here too, with the password sent at that handoff.
		if (selected < (Int)s_liveGamePassworded.size() && s_liveGamePassworded[selected])
		{
			const AsciiString& displayName = (selected < (Int)s_liveGameNames.size())
				? s_liveGameNames[selected] : AsciiString::TheEmptyString;
			liveWatchOpenObservePasswordPopup(s_liveGameIds[selected], displayName);
			return TRUE;
		}

		SetLobbyObserverMode(s_liveGameIds[selected].str());
		TheShell->push("Menus/GameSpyGameOptionsMenu.wnd");
		return TRUE;
	}

	// Hand over the lobby id alone: GO mints the single-use watch ticket and returns the relay
	// URL that carries it. The row's delay pre-seeds the countdown and join timeout while GO
	// holds the ticket behind the delay gate.
	StartLiveObserverSession(s_liveGameIds[selected], AsciiString::TheEmptyString,
		AsciiString::TheEmptyString,
		(selected < (Int)s_liveGameDelaySeconds.size()) ? s_liveGameDelaySeconds[selected] : -1);
	TheShell->pop();
	return TRUE;
}

// ============================================================================
// Public entry points (called by ReplayMenu's callbacks)
// ============================================================================

void LiveGamesMenuInit(void)
{
	GameWindow* parent = TheWindowManager->winGetWindowFromId(nullptr,
		TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:ParentReplayMenu"));
	s_listbox = TheWindowManager->winGetWindowFromId(parent,
		TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:ListboxReplayFiles"));
	s_buttonLoad = TheWindowManager->winGetWindowFromId(parent,
		TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:ButtonLoadReplay"));
	s_buttonDelete = TheWindowManager->winGetWindowFromId(parent,
		TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:ButtonDeleteReplay"));
	s_buttonCopy = TheWindowManager->winGetWindowFromId(parent,
		TheNameKeyGenerator->nameToKey("ReplayMenu.wnd:ButtonCopyReplay"));

	if (s_listbox != nullptr)
		s_listboxID = s_listbox->winGetWindowId();
	if (s_buttonLoad != nullptr)
		s_buttonLoadID = s_buttonLoad->winGetWindowId();
	if (s_buttonDelete != nullptr)
		s_buttonDeleteID = s_buttonDelete->winGetWindowId();
	if (s_buttonCopy != nullptr)
		s_buttonCopyID = s_buttonCopy->winGetWindowId();

	// Retitle and repurpose the action buttons. Copy is hidden rather than relabelled:
	// a browser has no sensible third action, and a dead button is worse than a gap.
	s_liveTitleWindow = findTitleWindow(parent);
	if (s_liveTitleWindow)
	{
		s_savedTitleText = s_liveTitleWindow->winGetText();
		// GadgetStaticTextSetText, not winSetText: a static text draws from its TextData display
		// string, which only the gadget's GGM_SET_LABEL handler updates. winSetText changes the
		// instance-data text, which the static-text draw never reads.
		GadgetStaticTextSetText(s_liveTitleWindow, UnicodeString(L"LIVE GAMES"));
	}
	if (s_buttonLoad)
	{
		s_savedLoadText = s_buttonLoad->winGetText();
		// One label for both row kinds: connecting to a live stream and parking in a
		// pre-game lobby are the same act (observing), so the button must not flip
		// between CONNECT and OBSERVE as the selection moves.
		s_buttonLoad->winSetText(UnicodeString(L"OBSERVE"));
	}
	if (s_buttonDelete)
	{
		s_savedDeleteText = s_buttonDelete->winGetText();
		s_buttonDelete->winSetText(UnicodeString(L"REFRESH"));
	}
	if (s_buttonCopy)
		s_buttonCopy->winHide(TRUE);

	// The replay tooltip reads the hovered file off disk; these rows are not files.
	if (s_listbox != nullptr)
		s_listbox->winSetTooltipFunc(nullptr);

	// The caller has already reset the listbox; show the loading row and start the first fetch.
	if (s_listbox != nullptr)
	{
		GadgetListBoxAddEntryText(s_listbox,
			UnicodeString(L"Loading games..."), GameMakeColor(200, 200, 200, 255), -1);
	}
	liveGamesRequestList();
}

void LiveGamesMenuShutdown(void)
{
	if (!s_liveGamesMode)
		return;

	// Leave the screen as we found it. The controls belong to the shared layout, so a
	// retitled heading or a hidden Copy button would otherwise persist into the next visit
	// to the real replay menu.
	if (s_liveTitleWindow)
		GadgetStaticTextSetText(s_liveTitleWindow, s_savedTitleText);
	if (s_buttonLoad && !s_savedLoadText.isEmpty())
		s_buttonLoad->winSetText(s_savedLoadText);
	if (s_buttonDelete && !s_savedDeleteText.isEmpty())
		s_buttonDelete->winSetText(s_savedDeleteText);
	if (s_buttonCopy)
		s_buttonCopy->winHide(FALSE);

	s_liveTitleWindow = nullptr;
	s_liveGameIds.clear();
	s_liveGameIsLive.clear();
	s_liveGamePassworded.clear();
	s_liveGameNames.clear();
	s_liveGameDelaySeconds.clear();
	s_liveGamesMode = FALSE;

	s_listbox = nullptr;
	s_buttonLoad = nullptr;
	s_buttonDelete = nullptr;
	s_buttonCopy = nullptr;
	s_listboxID = 0;
	s_buttonLoadID = 0;
	s_buttonDeleteID = 0;
	s_buttonCopyID = 0;
}

void LiveGamesMenuUpdate(void)
{
	if (!s_liveGamesMode)
		return;

	// The relay fetch runs on its own thread, so collect its result here rather than via
	// a callback - nothing off the main thread may touch gadget state.
	AsciiString body;
	Bool fetchOk = FALSE;
	Int statusCode = 0;
	if (liveRelayPollFetch(body, fetchOk, statusCode))
		liveGamesApplyResponse(fetchOk, statusCode, body);

	// Keep the list current while it is open, so games starting and ending appear on
	// their own without the user thinking about refreshing.
	if (!liveRelayFetchInFlight() &&
		timeGetTime() - s_lastLiveFetchMs >= LIVE_GAMES_REFRESH_INTERVAL_MS)
	{
		liveGamesRequestList();
	}
}

Bool LiveGamesMenuHandleSystemMessage(UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2)
{
	if (!s_liveGamesMode)
		return FALSE;

	switch (msg)
	{
		case GLM_DOUBLE_CLICKED:
		{
			GameWindow* control = (GameWindow*)mData1;
			if (control != nullptr && control->winGetWindowId() == s_listboxID)
			{
				// Connects the listbox's current selection, not the clicked row.
				if ((Int)mData2 >= 0)
					liveGamesConnectSelected();
				return TRUE;
			}
			break;
		}

		case GBM_SELECTED:
		{
			GameWindow* control = (GameWindow*)mData1;
			if (control == nullptr)
				break;

			const Int controlID = control->winGetWindowId();
			if (controlID == s_buttonLoadID)
			{
				if (!liveGamesConnectSelected())
				{
					MessageBoxOk(UnicodeString(L"No game selected"),
						UnicodeString(L"Please select a live game to watch."), nullptr);
				}
				return TRUE;
			}
			else if (controlID == s_buttonDeleteID)
			{
				liveGamesRequestList();		// this button is REFRESH here
				return TRUE;
			}
			else if (controlID == s_buttonCopyID)
			{
				return TRUE;		// hidden in this mode; nothing to copy
			}
			break;
		}
	}

	return FALSE;
}

#endif // defined(GENERALS_ONLINE)
