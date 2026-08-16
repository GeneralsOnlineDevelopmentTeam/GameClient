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
// FILE: LiveObserverSession.cpp
// The live-observer join state machine. Every shell screen that can queue or pump a session calls
// in here - the Watch Live browser, the pre-game lobby view, the Online welcome screen, the main
// menu and the password popup - and none of them owns the state.
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#if defined(GENERALS_ONLINE)

#include "GameClient/LiveObserverSession.h"

#include "Common/GlobalData.h"
#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "GameClient/ClientInstance.h"
#include "GameClient/GameText.h"
#include "GameClient/GameWindowTransitions.h"
#include "GameClient/GUICallbacks.h"			// liveWatchOpenPasswordPopup
#include "GameClient/Shell.h"
#include "GameNetwork/GameSpyOverlay.h"

#include <windows.h>						// timeGetTime

// The queued session. Written by StartLiveObserverSession from whichever screen picked the
// game, read by the pump below from whichever screen the player is standing on.
static Bool startLiveObserverGame = FALSE;
static AsciiString m_liveObserverStartLobbyId;
static AsciiString s_liveObserverPassword;		// password for a password-protected stream
static AsciiString s_liveObserverDisplayName;	// lobby name, for the password reprompt title
static Int s_liveObserverDelaySeconds = -1;		// lobby broadcast delay, -1 = unknown

// Records the intent only. doLiveObserverGameStart() blocks waiting for the relay's HEADER, so it
// must not run while another screen is up; the pump fires it once the shell has settled.
void StartLiveObserverSession(const AsciiString& lobbyId,
	const AsciiString& password, const AsciiString& displayName, Int delaySeconds)
{
	if (lobbyId.isEmpty())
		return;

	// Deliberately does not set m_afterIntro: that re-enters the intro/movie machinery, which sets
	// m_breakTheMovie and disables rendering until a menu or load screen clears it, freezing the
	// screen while the logic keeps running. The shell map stays off so nothing competes with the
	// replay about to start.
	if (TheWritableGlobalData)
	{
		TheWritableGlobalData->m_playIntro = FALSE;
		TheWritableGlobalData->m_playSizzle = FALSE;
		TheWritableGlobalData->m_shellMapOn = FALSE;
	}

	// Multi-instance support, like replay mode.
	rts::ClientInstance::setMultiInstance(TRUE);
	rts::ClientInstance::skipPrimaryInstance();

	m_liveObserverStartLobbyId = lobbyId;
	s_liveObserverPassword = password;
	s_liveObserverDisplayName = displayName;
	s_liveObserverDelaySeconds = delaySeconds;
	startLiveObserverGame = TRUE;

	liveObserverLog("StartLiveObserverSession: queued lobby %s\n", lobbyId.str());
}

// The join splits into a non-blocking connect and a playback start. The wait between them, for
// the relay to deliver the header plus enough body to cover the broadcast delay, must not block
// the main loop or the shell cannot draw the countdown explaining it. Readiness itself lives on
// LiveObserver::isPlaybackReady; this file only sequences it.
enum ObserverJoinPhase
{
	kObserverJoinIdle,		// nothing pending
	kObserverJoinWaiting,	// connected; waiting for the file to cover the delay
};
static ObserverJoinPhase s_observerJoinPhase = kObserverJoinIdle;

// Mirrors the timeout path in LiveObserverStartPendingSession.
void CancelLiveObserverPendingSession(void)
{
	liveObserverLog("CancelLiveObserverPendingSession: cancelling queued observer join\n");

	if (s_observerJoinPhase == kObserverJoinWaiting && TheLiveObserver != nullptr)
	{
		liveObserverEndSession();
	}

	s_observerJoinPhase = kObserverJoinIdle;
	startLiveObserverGame = FALSE;
	m_liveObserverStartLobbyId.clear();

	// A cancelled session must not leak its password into the next join.
	s_liveObserverPassword.clear();
	s_liveObserverDisplayName.clear();
	s_liveObserverDelaySeconds = -1;
}

Bool LiveObserverPendingSessionActive(void)
{
	return startLiveObserverGame;
}

// Phase 1: end any previous session and start connecting. Non-blocking - the network thread
// does the work and publishes the header and watermarks as it goes.
static Bool doLiveObserverConnect(void)
{
	liveObserverInitLog(m_liveObserverStartLobbyId.str());

	// End any previous session outright. This destroys the old LiveObserver and releases the
	// Recorder's read handle on its file - both are needed, because the live file is named after
	// the streamer's game, so rejoining a game already watched targets the same path, which
	// openLiveFile() cannot delete or recreate while either handle is open.
	liveObserverEndSession();

	TheLiveObserver = createLiveObserver();
	if (!TheLiveObserver)
	{
		liveObserverLog("doLiveObserverConnect: createLiveObserver() returned NULL!\n");
		return FALSE;
	}

	liveObserverLog("doLiveObserverConnect: connecting to relay for lobby %s\n",
		m_liveObserverStartLobbyId.str());
	TheLiveObserver->connect(m_liveObserverStartLobbyId, s_liveObserverPassword.str(),
		s_liveObserverDelaySeconds);
	return TRUE;
}

// Phase 2: the file is playable (LiveObserver::isPlaybackReady), so start playback. Returns TRUE
// only when playback actually started, so the shell screen that pumped this can tear itself down
// and reveal the game.
static Bool doLiveObserverStartPlayback(void)
{
	if (TheLiveObserver == nullptr)
		return FALSE;

	const AsciiString filename = TheLiveObserver->getLiveReplayFilename();
	if (!TheRecorder->startLiveObserverPlayback(filename))
	{
		liveObserverLog("doLiveObserverStartPlayback: FAILED - playbackFile returned false\n");
		liveObserverEndSession();
		return FALSE;
	}

	liveObserverLog("doLiveObserverStartPlayback: playback started\n");
	return TRUE;
}

// Fire a queued session once the shell has settled, then keep pumping while the relay builds the
// file. Returns FALSE while still buffering, so the shell keeps drawing the countdown, and TRUE
// once playback has started, so the caller can stand itself down and reveal the game.
Bool LiveObserverStartPendingSession(void)
{
	if (!startLiveObserverGame)
		return FALSE;

	if (s_observerJoinPhase == kObserverJoinIdle)
	{
		if (!TheShell->isAnimFinished() || !TheTransitionHandler->isFinished())
			return FALSE;

		if (!doLiveObserverConnect())
		{
			startLiveObserverGame = FALSE;
			return FALSE;
		}
		s_observerJoinPhase = kObserverJoinWaiting;
		liveObserverLog("LiveObserverStartPendingSession: connected, waiting for the file to cover the delay (up to %ums)\n",
			TheLiveObserver->getJoinTimeoutMs());
		return FALSE;
	}

	// kObserverJoinWaiting - pump the wait. Every failure path clears the join state.
	if (TheLiveObserver == nullptr)
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	if (TheLiveObserver->isPlaybackReady())
	{
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return doLiveObserverStartPlayback();
	}

	// The one reprompt path: it covers both a wrong password typed into the browser popup and the
	// pre-game handoff of a passworded lobby.
	if (TheLiveObserver->isPasswordRejected())
	{
		liveObserverLog("LiveObserverStartPendingSession: password rejected for lobby %s\n",
			m_liveObserverStartLobbyId.str());

		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;

		// The queue statics still hold the lobby id and display name - they are cleared by
		// CancelLiveObserverPendingSession or overwritten by the next StartLiveObserverSession.
		GSMessageBoxOk(TheGameText->fetch("GUI:JoinFailedDefault"),
			TheGameText->fetch("GUI:JoinFailedBadPassword"), []()
			{
				liveWatchOpenPasswordPopup(m_liveObserverStartLobbyId, s_liveObserverDisplayName, FALSE);
			});

		return FALSE;
	}

	if (timeGetTime() > TheLiveObserver->getJoinDeadlineMs())
	{
		liveObserverLog("LiveObserverStartPendingSession: timed out waiting for a playable file - abandoning the join "
			"(now=%ums deadline=%ums connected=%d serverHeld=%d delayWait=%d)\n",
			timeGetTime(), TheLiveObserver->getJoinDeadlineMs(),
			TheLiveObserver->isConnected() ? 1 : 0,
			TheLiveObserver->isServerHeld() ? 1 : 0,
			TheLiveObserver->isWaitingForBroadcastDelay() ? 1 : 0);
		liveObserverEndSession();
		s_observerJoinPhase = kObserverJoinIdle;
		startLiveObserverGame = FALSE;
		return FALSE;
	}

	return FALSE;
}

#endif // GENERALS_ONLINE
