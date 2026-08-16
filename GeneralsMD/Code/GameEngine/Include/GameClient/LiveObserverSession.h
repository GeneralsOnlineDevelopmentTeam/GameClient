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
// FILE: LiveObserverSession.h
// Description: Queues and pumps a live-observer session. Joining a livestream is a two-step
// affair that no single screen can own: the intent is recorded from whichever screen picked the
// game, and the connect-then-start-playback sequence is pumped by whichever screen the player is
// standing on when it completes. This module holds that state machine so no menu has to.
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Lib/BaseType.h"

#if defined(GENERALS_ONLINE)

/// Queue a live-observer session for the given lobby. Records the intent only: the join waits on
/// the relay's HEADER and then starts a game, neither of which may happen while a screen is still
/// animating, so the screen the player lands on performs it via LiveObserverStartPendingSession().
///
/// password is the password of a password-protected stream (sent with the watch-ticket request);
/// displayName is the lobby's name, used to title the password reprompt popup; delaySeconds is
/// the lobby's broadcast delay (-1 = unknown), which pre-seeds the countdown and join timeout
/// until GO's admission hold or the relay's ROLE pins the real value.
void StartLiveObserverSession(const AsciiString& lobbyId,
	const AsciiString& password = AsciiString::TheEmptyString,
	const AsciiString& displayName = AsciiString::TheEmptyString,
	Int delaySeconds = -1);

/// Abort a queued or connecting live-observer session. The only other way out is the join
/// timeout; the read-only lobby view needs LEAVE to cancel mid-wait.
void CancelLiveObserverPendingSession(void);

/// TRUE while a live-observer session is queued or connecting (between
/// StartLiveObserverSession and playback starting). Used by the lobby-observer screen to tell
/// "still waiting" from "the join was abandoned" without owning the session's internals.
Bool LiveObserverPendingSessionActive(void);

/// Start a queued live-observer session if one is pending and the shell has settled; a no-op
/// otherwise. Returns TRUE when playback actually started, which means the calling screen should
/// now stand itself down so the running game is visible.
///
/// Must be pumped from every shell screen the browser can be reached from, because a session is
/// queued from whichever one the player happens to be on and only that screen can tear itself
/// down afterwards.
Bool LiveObserverStartPendingSession(void);

#endif // GENERALS_ONLINE
