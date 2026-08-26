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

// FILE: LiveObserver.h ///////////////////////////////////////////////////////////////////////////
// Receives a live match's replay bytes from the relay server and feeds them to the Recorder,
// so a third party can watch a game in progress without being a network peer.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#if defined(GENERALS_ONLINE)

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/UnicodeString.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <deque>
#include <string>

class File;

/**
 * Receives raw replay bytes (HEADER/PATCH/BODY/END) from the relay over a WebSocket and writes
 * them to a local "_live.rep" file, which the Recorder plays back in
 * RECORDERMODETYPE_LIVE_OBSERVER while this class's background thread appends to it.
 *
 * The Recorder must never read past getSafeReadOffset(); everything below it is whole records.
 */
class LiveObserver
{
public:
	LiveObserver();
	~LiveObserver();

	/// Start watching a livestream by GO lobby id. Non-blocking; spawns a background thread. The
	/// relay URL comes from GO with the single-use watch ticket, so callers never build it.
	/// expectedDelaySeconds (-1 = unknown) pre-seeds the countdown until the ticket or ROLE
	/// pins the real value.
	void connect(const AsciiString& lobbyId, const std::string& password = std::string(),
		Int expectedDelaySeconds = -1);

	/// GO answered the ticket request with 401: the stream is password protected and the password
	/// was missing or wrong. The join pump re-prompts instead of waiting out the retry window.
	Bool isPasswordRejected() const { return m_passwordRejected.load(); }

	/// Returns true once the HEADER has been received and playback can start.
	Bool isReady() const { return m_headerReceived.load(); }

	/// Returns true if connected to the relay server.
	Bool isConnected() const { return m_connected.load(); }

	/// Returns true if the streamer has ended the session.
	Bool isStreamEnded() const { return m_streamEnded.load(); }

	/// Latched by RecorderClass::startLiveObserverPlayback() once playback is actually running.
	/// Until then the session is still being set up, and clearing game data must not end it -
	/// see liveObserverOnGameCleared().
	void notePlaybackStarted() { m_playbackStarted = TRUE; }
	Bool hasPlaybackStarted() const { return m_playbackStarted; }

	/// The live file is safe to start playing: header in place, first body record on disk, and the
	/// buffer already covers the broadcast delay (or the stream ended). Waiting for this is what
	/// stops a not-yet-arrived first record from looking like the end of the replay.
	Bool isPlaybackReady() const;

	/// Whole seconds until isPlaybackReady(); 0 once it is. Shown by the shell while joining.
	Int getSecondsUntilPlaybackReady() const;

	/// How long the join may wait for isPlaybackReady(): the broadcast delay plus headroom for
	/// the connection, ticket minting and the first record.
	UnsignedInt getJoinTimeoutMs() const;

	/// Absolute deadline (timeGetTime ms) for the join to have started playback. While GO holds
	/// the ticket this is anchored to the hold end, never to when the join started, so time spent
	/// held cannot eat the budget. Otherwise join start plus getJoinTimeoutMs().
	UnsignedInt getJoinDeadlineMs() const;

	/// Returns the filename of the live replay file (e.g. "996C586F_live.rep").
	const AsciiString& getLiveReplayFilename() const { return m_liveFilename; }

	/// Highest frame held in a fully-received record. Published by the network thread as data
	/// arrives, so reading it is O(1) and always fresh.
	UnsignedInt getMaxCompleteFrame() const { return m_maxCompleteFrame.load(); }

	/// Where the live game actually is - the value the buffering gate reasons about, and the
	/// highest frame that is safe to simulate up to.
	///
	/// The record edge alone only advances on frames that carried input, so it sawtooths in ~1.7 s
	/// jumps and would stall the observer on a stream that is not late. The streamer's heartbeat
	/// (MSG_TICK) states the frame outright and is only emitted after that frame's records are
	/// flushed, so take whichever of the two is further ahead.
	UnsignedInt getLiveEdge() const
	{
		const UnsignedInt records = m_maxCompleteFrame.load();
		const UnsignedInt heartbeat = m_liveFrameHint.load();
		return heartbeat > records ? heartbeat : records;
	}

	/// Absolute file offset one past the last complete record. The Recorder must never read
	/// beyond this: a torn record at the growing tail misaligns the playback stream permanently.
	Int getSafeReadOffset() const { return m_safeReadOffset.load(); }

	/// File offset of the first body byte (the header length). The Recorder rewinds its read
	/// cursor here when starting playback, because playbackFile()'s seeding read leaves it
	/// past the first record's frame field - and the live loop reads the frame itself.
	Int getBodyStartOffset() const { return m_bodyStartOffset; }

	/// Close the connection and shut down the background thread.
	void close();

	// ---- Session policy: the broadcast delay and the buffering gate --------------------
	//
	// This state belongs to the session rather than the Recorder, so that ending a session is
	// destroying the object and nothing can leak into the next one.

	/// Re-evaluate the gate for this tick and apply the resulting pause.
	void updatePlaybackGate(UnsignedInt curFrame);

	/// Logic frames per second the source produced, measured over the last second as the slope of
	/// the live edge. 0 until the sample window has filled. This is the rate the streamer's own
	/// simulation actually ran at: a network match runs at its slowest peer's rate, so it sits
	/// below nominal whenever the match is under load.
	UnsignedInt getSourceFps() const { return m_sourceFps; }

	/// Logic frames per second this client is currently playing at. Equals LOGICFRAMES_PER_SECOND
	/// unless the pace controller has slowed playback to match the source.
	UnsignedInt getPaceFps() const { return m_paceFps; }

	/// The streamer's own logic frame rate and ping, as it reported them (MSG_STATS). 0 until the
	/// first stats frame arrives - an older streamer never sends one, so a readout must treat 0 as
	/// "unknown" rather than as a measurement. These are what the HUD shows: getSourceFps() above
	/// is the rate measured here after transport, which is the pace controller's input and not the
	/// number a player in the match would recognise.
	UnsignedInt getStreamerLogicFps() const { return m_srcLogicFps.load(); }
	UnsignedInt getStreamerPingMs() const { return m_srcPingMs.load(); }

	/// Whether playback follows the match's own logic rate (default) or runs at nominal speed.
	///
	/// Off is for a viewer sitting on a backlog: matching an 11 fps match means watching it at
	/// 11 fps *and staying two minutes behind*, where running at nominal spends the backlog and
	/// closes the gap. On is the default because once the backlog is gone there is nothing left
	/// to spend, and nominal playback then just outruns the source and stalls.
	Bool isPaceMatchingEnabled() const { return m_paceMatchingEnabled; }

	/// Flip it. Returns the new state, for the message the hotkey shows.
	Bool togglePaceMatching() { m_paceMatchingEnabled = !m_paceMatchingEnabled; return m_paceMatchingEnabled; }

	/// Put the frame pacer back the way this session found it. Must run before the session ends -
	/// the pacer is global engine state, and leaking a slowed logic scale into the next game would
	/// be indistinguishable from an engine bug.
	void restorePlaybackPace();

	/// The player's own pause intent, kept apart from the buffering gate's. updatePlaybackGate()
	/// ORs the two, so buffering can never undo a manual pause nor the reverse.
	void toggleUserPause() { m_userPaused = !m_userPaused; }

	/// Whether playback must wait rather than consume more records. Valid once
	/// updatePlaybackGate() has run this tick; the Recorder applies it, it does not decide it.
	Bool shouldHoldPlayback() const { return m_holdPlayback; }

	/// True only when playback is held *and* the source has genuinely stopped producing -
	/// not during the normal sawtooth of maintaining the delay at the boundary.
	Bool isStalled() const { return m_stalled; }

	/// Playback sits inside the broadcast delay, as close to the live game as it is allowed to
	/// get. Fast-forward is refused here, so it can only close a backlog and never catch up to
	/// the real game.
	Bool isWithinBroadcastDelay(UnsignedInt curFrame) const;

	/// The broadcast delay this session started with. Held in seconds because that is what a
	/// streamer configures, and it survives a change of logic tick rate.
	UnsignedInt getDelaySeconds() const { return m_delaySeconds.load(); }
	UnsignedInt getDelayFrames() const { return m_delaySeconds.load() * LOGICFRAMES_PER_SECOND; }

	/// How far behind the live edge this session aims to sit, in frames: the larger of the
	/// broadcast delay and the viewer's jitter buffer. Paid once at join as a fixed offset,
	/// never as a rate change - playback runs at exactly 100% so the observer's clock agrees
	/// with the real match clock.
	UnsignedInt getTargetLeadFrames() const;

	/// 0 when GO holds the stream server-side, since the ticket was only minted once the delay had
	/// elapsed and the stream is therefore already delayed; otherwise the session delay. The gate
	/// functions use this so a server-held stream plays at the live edge instead of double-holding.
	UnsignedInt getEffectiveDelaySeconds() const
	{
		return m_serverHeld.load() ? 0 : m_delaySeconds.load();
	}

	/// GO is holding this viewer's watch ticket behind the broadcast delay (423). The join pump
	/// and countdown show the hold rather than a failure.
	Bool isWaitingForBroadcastDelay() const { return m_delayWaitActive.load(); }

	/// Whole seconds left in the admission hold (rounded up), 0 when not waiting.
	Int getBroadcastDelayRemainingSeconds() const;

	/// TRUE once the watch ticket was minted with server_held=true: GO owns the broadcast
	/// delay, so this client must not hold playback itself.
	Bool isServerHeld() const { return m_serverHeld.load(); }

	/// The delay known before the ticket or ROLE arrives: connect()'s expected value when given,
	/// otherwise the current session delay. Used by the countdown until the hold publishes its
	/// exact remaining time.
	UnsignedInt getExpectedDelaySeconds() const
	{
		return (m_expectedDelaySeconds >= 0) ? (UnsignedInt)m_expectedDelaySeconds : m_delaySeconds.load();
	}

	/// Record the frame at which this client's simulation was first seen to diverge from the
	/// streamed one. Playback deliberately continues afterwards - the observer just needs to be
	/// told that what it is watching is no longer the real game. Only the first divergence is
	/// recorded; a desynced simulation diverges further every frame after it.
	void noteDesync(UnsignedInt frame);
	Bool isDesynced() const { return m_desyncFrame != 0; }
	UnsignedInt getDesyncFrame() const { return m_desyncFrame; }

	// ---- Chat --------------------------------------------------------------------------

	/// Manual spectator-chat mode (F7 cycles through these).
	enum SpectatorChatMode
	{
		SPECTATOR_CHAT_AUTO = 0,      ///< spoiler-gated: shown within 5s of the delay boundary
		SPECTATOR_CHAT_FORCED_ON,     ///< always shown (spoilers accepted)
		SPECTATOR_CHAT_OFF            ///< never shown
	};

	/// Pop and display queued chat: player chat released once playback reaches the streamer's
	/// frame, spectator chat live per SpectatorChatMode. Called once per logic frame, including
	/// during the pre-game phase, where player chat is held and spectator chat is dropped.
	void pollChatMessages(UnsignedInt curFrame);

	/// Cycle the spectator-chat mode (auto -> forced ON -> off -> auto). Returns the new mode.
	SpectatorChatMode toggleSpectatorChatMode()
	{
		m_spectatorChatMode = (SpectatorChatMode)((m_spectatorChatMode + 1) % 3);
		return m_spectatorChatMode;
	}

	/// Queue a spectator chat line for the network thread to send to the relay
	/// (MSG_SPECTATOR_CHAT). The sender's display name is stamped from the signed-in user.
	void sendSpectatorChat(const UnicodeString& text);

private:
	// Player chat is frame-stamped and released when playback reaches that frame. Spectator chat
	// is live and spoiler-gated: shown on arrival within 5s of the delay boundary, else dropped.
	struct ChatEntry
	{
		UnsignedInt frame;      ///< player chat: streamer-side frame; spectator chat: unused
		UnsignedInt colorArgb;  ///< display color (sender color for player chat)
		Bool spectator;         ///< live spectator chat (no frame gate, spoiler gate instead)
		Bool disaster = FALSE;  ///< stream-failure notice: shown unconditionally, never gated
		UnicodeString text;     ///< already-formatted line ("[name] msg" / "[Spec] name: msg")
	};
	std::deque<ChatEntry> m_chatQueue;          // written by the network thread (handleFrame)
	mutable std::mutex m_chatMutex;             // game thread (pollChatMessages) drains
	std::deque<std::vector<char>> m_outboundChatQueue;  // spectator chat to send
	mutable std::mutex m_outboundChatMutex;     // network thread drains
	SpectatorChatMode m_spectatorChatMode;      // F7 cycle: auto / forced on / off

	/// Display one chat entry in the HUD message log (shared player/spectator path).
	void displayChat(const ChatEntry& entry);

	/// The 5-second spoiler gate for spectator chat: TRUE while the observer is within
	/// ~5s of the broadcast-delay boundary (effectively watching live).
	Bool isSpectatorGateOpen(UnsignedInt curFrame) const;

	/// Background thread for network I/O.
	void networkThreadFunc();

	/// Consume complete records from newly-arrived body bytes and republish the
	/// watermarks. Called on the network thread only.
	void advanceParseCursor(Int chunkOffset, const unsigned char* data, size_t dataLen);

	/// Reset the parse cursor and watermarks (new session / disconnect).
	void resetParseCursor(Int bodyStartOffset);

	/// Re-evaluate the playback pace for this tick and drive TheFramePacer. Called by
	/// updatePlaybackGate, which owns every other pacing decision as well.
	void updatePlaybackPace(UnsignedInt nowMs, UnsignedInt gap, UnsignedInt targetLead, Bool streamEnded);

	/// Push a pace onto TheFramePacer, or leave it alone if nothing changed.
	void applyPaceFps(Int paceFps);

	/// Connect via WebSocket (called from network thread).
	bool connectToRelay();

	/// Ask GO for a single-use watch ticket for m_gameId, using the logged-in session token.
	/// On success outConnectUrl is the complete relay URL to connect to, ticket included.
	/// There is no fallback: without a ticket the relay refuses the connection.
	bool fetchWatchTicket(AsciiString& outConnectUrl);

	/// Send data over WebSocket binary (called from network thread).
	bool wsSendBinary(const unsigned char* data, size_t len);

	/// Outcome of one wsRecv() attempt. Three states rather than a bool because the drain loop
	/// must tell "curl has nothing more buffered" (stop draining, go back to polling) apart from
	/// "that message was not ours" - a keepalive ping read as "nothing more" would end the drain
	/// early and put the receive rate back on the poll timeout.
	enum WsRecvResult
	{
		WS_RECV_DATA,    ///< outBuffer holds a binary payload
		WS_RECV_SKIPPED, ///< a frame arrived but is not stream data (ping/pong/text/close)
		WS_RECV_NONE     ///< nothing buffered right now, or the connection is gone
	};

	/// Receive one WebSocket message (non-blocking). One call yields at most one message.
	WsRecvResult wsRecv(std::vector<char>& outBuffer);

	/// Open the local replay file for writing.
	bool openLiveFile();

	/// Process an incoming binary frame from the relay.
	void handleFrame(unsigned char type, const char* payload, size_t len);

	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;
	std::atomic<Bool> m_headerReceived;
	std::atomic<Bool> m_streamEnded;

	// How long the live edge must sit still before a hold counts as a stall rather than normal
	// delay maintenance. At the boundary the hold toggles every few ticks, so without this the
	// status bar reads WAITING FOR FRAMES during healthy playback.
	enum { LIVE_STALL_THRESHOLD_MS = 1000 };

	// Let the game get on its feet before holding it. GameClient::step() only runs on ticks where
	// logic runs, so holding at frame 1 leaves a loaded but never-composed scene - a black screen.
	enum { LIVE_PREROLL_WARMUP_FRAMES = 120 };

	// Lead the gate rebuilds past the target before resuming from a hold. Must stay at one
	// heartbeat interval (Recorder::LIVE_TICK_INTERVAL_FRAMES) or the gate re-engages every tick
	// and micro-stutters.
	enum { LIVE_GATE_RELEASE_MARGIN_FRAMES = 10 };

	// Silence this long - no stream bytes and none of the relay's ~20 s keepalive pings - means
	// the relay or the connection to it is gone, and the watch winds down like a stream END
	// instead of freezing on the last frame forever.
	enum { LIVE_RELAY_WATCHDOG_MS = 120000 };

	// ---- Rate-matched playback -----------------------------------------------------------
	//
	// A network match runs at the rate its slowest peer sustains, so the streamer's simulation
	// dips below nominal under load. Playing those frames back at a fixed nominal rate consumes
	// the lead faster than it is produced - a rate mismatch, which no buffer size can absorb,
	// because a bigger buffer only postpones the moment it runs out. So playback follows the
	// source's rate instead, and a pause is left as the floor case for when there is genuinely
	// nothing to play.
	//
	// Slowing down can never bring the observer closer to the live game, so it cannot weaken the
	// broadcast delay. The pace is therefore clamped at nominal and never above: catching up
	// stays the fast-forward gate's business, which already refuses inside the delay boundary.

	// Window the source rate is measured over. One second is long enough to average out the
	// arrival quantisation of individual records, short enough to follow a real dip.
	enum { LIVE_PACE_WINDOW_MS = 1000 };

	// Samples kept for that window. The gate runs once per rendered frame, so this is sized for a
	// high refresh rate; the window is enforced by timestamp, not by count.
	enum { LIVE_PACE_MAX_SAMPLES = 256 };

	// Floor for the pace. Corrected 2026-08-15: this was LOGICFRAMES_PER_SECOND / 2, on the
	// reasoning that below half nominal something is wrong that pacing should not paper over.
	// That reasoning was wrong. A match on a loaded host really does run at 11 logic frames per
	// second, and at a floor of 30 the observer outruns it threefold, drains its lead and pauses -
	// the exact symptom rate-matching exists to remove. The floor's only job is to keep a *stalled*
	// source (srcFps collapsing towards 0) from crawling the picture instead of stopping it
	// honestly, which the buffering gate and the stall indicator handle.
	enum { LIVE_PACE_MIN_FPS = 5 };

	// How quickly surplus or missing lead is repaid, in seconds. The pace carries a correction of
	// (gap - targetLead) / this, so a 10-frame surplus at 4 s adds 2.5 fps rather than lurching.
	enum { LIVE_PACE_CORRECTION_SECONDS = 4 };

	// Floor for that correction's magnitude, so a very slow source still gets *some* authority to
	// drift its lead back to target rather than being pinned exactly at the source rate forever.
	// The cap itself is relative (half the source rate) - see updatePlaybackPace.
	enum { LIVE_PACE_MIN_CORRECTION_FPS = 2 };

	// Do not touch the pacer for less than this, nor more often than this. A controller that
	// chases every sample hunts, and hunting is more visible than the lag it corrects.
	enum { LIVE_PACE_MIN_STEP_FPS = 2 };
	enum { LIVE_PACE_MIN_INTERVAL_MS = 500 };

	// Buffering-gate state. Game thread only - updatePlaybackGate() is the sole writer.
	Bool m_holdPlayback;			// playback must wait; the Recorder acts on this
	Bool m_nearLiveHeld;			// latched: the near-live gate is holding (hysteresis)
	Bool m_preRollComplete;			// latches TRUE once the initial buffer is first built
	Bool m_autoPaused;				// the buffering logic owns the current pause
	Bool m_userPaused;				// the player pressed P and wants it paused
	Bool m_stalled;					// held, and no new data has arrived for a while
	Bool m_playbackStarted;			// the Recorder is actually playing this session's file
	UnsignedInt m_lastSeenLiveEdge;
	UnsignedInt m_lastLiveEdgeChangeMs;
	UnsignedInt m_desyncFrame;		// frame of the first observed CRC divergence, 0 = none

	// Once-per-second gate trace. The previous sample is kept so the log can state playback and
	// source *rates* rather than raw counters: those two numbers side by side are what tells a
	// transport hiccup (source rate steady, playback starving) apart from the source itself
	// running below its nominal logic rate (both drop together), which no other log shows.
	UnsignedInt m_lastGateLogMs;
	UnsignedInt m_lastGateLogFrame;
	UnsignedInt m_lastGateLogEdge;
	UnsignedInt m_underrunCount;	// near-live gate engagements: the buffer ran dry this often

	// Pace-controller state. Game thread only - updatePlaybackGate is the sole writer, same as
	// the buffering gate above.
	struct PaceSample
	{
		UnsignedInt ms;
		UnsignedInt edge;
	};
	PaceSample m_paceSamples[LIVE_PACE_MAX_SAMPLES];
	Int m_paceSampleCount;			// entries in use, oldest first
	UnsignedInt m_sourceFps;		// measured source rate, 0 until the window has filled
	UnsignedInt m_paceFps;			// what we are currently playing at
	UnsignedInt m_lastPaceApplyMs;
	Bool m_paceMatchingEnabled;		// F8; see isPaceMatchingEnabled

	// The pacer is global engine state, so this session records what it found on first touch and
	// puts it back when it ends. Latched rather than assumed, because another feature (the replay
	// game-speed hotkey) drives the same knob.
	Bool m_pacerTouched;
	Int m_savedLogicScaleFps;
	Bool m_savedLogicScaleEnabled;

	// Written by the network thread when the relay's ROLE frame arrives, read by the game thread
	// every tick. Atomic because those are genuinely two threads.
	std::atomic<UnsignedInt> m_delaySeconds;

	// GO admission-hold state. Written by the network thread in fetchWatchTicket, read by the game
	// thread. The deadline is a steady-clock ms timestamp; m_serverHeld latches on the 200
	// response and switches the effective delay to 0 for the rest of the session.
	std::atomic<Bool> m_serverHeld;
	std::atomic<Bool> m_delayWaitActive;
	std::atomic<UnsignedInt> m_delayWaitDeadlineMs;

	// The lobby's delay as known before any ticket or ROLE response, -1 = unknown. Written on the
	// main thread before the network thread spawns, so thread creation orders it.
	Int m_expectedDelaySeconds;

	// Watermarks published by the network thread, read by the game thread.
	std::atomic<UnsignedInt> m_maxCompleteFrame;
	std::atomic<Int> m_safeReadOffset;

	// When connect() was called (timeGetTime ms) - the baseline for the non-held join deadline.
	// Re-based forward when the watch ticket is granted, so the post-admission budget is measured
	// from admission and not from the start of a possibly minutes-long GO hold. Written by the
	// main thread (connect) and the network thread (ticket grant), read by the game thread.
	std::atomic<UnsignedInt> m_joinStartedAtMs;

	// Timestamp of the last websocket frame of any kind, including the relay's ~20 s keepalive
	// pings. Feeds the relay watchdog (LIVE_RELAY_WATCHDOG_MS).
	std::atomic<UnsignedInt> m_lastFrameReceivedMs{ 0 };

	// Parse-cursor state. Owned exclusively by the network thread - no locking.
	std::vector<unsigned char> m_parseTail;   // bytes after the last complete record
	Int m_parseAbsOffset;                     // absolute file offset of m_parseTail[0]
	Int m_bodyStartOffset;                    // file offset of the first body byte (the header length)
	Bool m_parseCorrupt;                      // latched: watermark frozen, see advanceParseCursor
	Bool m_parseGapPending;                   // a chunk arrived out of order; bytes are missing behind the cursor

	// Newest frame stated by the streamer's heartbeat (MSG_TICK). Zero until the first tick,
	// which is why getLiveEdge() maxes rather than prefers: an older streamer never sends one.
	std::atomic<UnsignedInt> m_liveFrameHint;

	// The streamer's last reported logic rate and ping (MSG_STATS). Written by the network thread,
	// read by the game thread for the HUD. Display only - nothing simulates off them.
	std::atomic<UnsignedInt> m_srcLogicFps{ 0 };
	std::atomic<UnsignedInt> m_srcPingMs{ 0 };

	AsciiString m_gameId;

	// Password for a password-protected livestream, sent with the watch-ticket request. Written
	// on the main thread before the network thread spawns, read only by the network thread -
	// happens-before via thread creation, no lock needed.
	std::string m_password;
	std::atomic<Bool> m_passwordRejected{ FALSE };

	File* m_liveFile;
	AsciiString m_liveFilePath;
	AsciiString m_liveFilename;   // e.g. "996C586F_live.rep"

	void* m_curlEasy;
	void* m_curlMulti;

	std::thread m_networkThread;
};

extern LiveObserver* TheLiveObserver;
LiveObserver* createLiveObserver();

/// End the live-observer session: destroy the observer, then let the Recorder wind down its live
/// playback. Deliberately not stopPlayback(), which also exits the game.
///
/// Must run before starting another session: the live file is named after the streamer's game, so
/// rejoining a game already watched targets the same path, and Windows will not let the observer
/// recreate a file the Recorder still holds open.
void liveObserverEndSession(void);

/// One-shot "a live-observer game just ended" latch. Set in liveObserverEndSession() while a
/// game was actually running; consumed by the shell screens' game-end hooks
/// (WOLGameSetupMenuInit / MainMenuInit), which then return the player to the Watch Live
/// browser instead of the main menu. An aborted join (no game ran) must not set it.
Bool LiveObserverConsumeReturnedFromGame(void);

/// Called from GameLogic::clearGameData(), the one point every game-end path converges on.
///
/// Ends the live session, but only once it has actually started playing: clearing game data is
/// also the *first* thing a starting session does, because RecorderClass::playbackFile() tears
/// down the shell map and the shell map counts as a game.
void liveObserverOnGameCleared(void);

// ---------------------------------------------------------------------------------------
// Standalone relay HTTP fetch, for the live game browser.
//
// Deliberately not routed through HTTPManager, which lives behind NGMP_OnlineServicesManager and
// is not initialised on the main menu unless the player has signed in, so every request silently
// no-ops there. The request runs on its own thread and the result is collected by polling from
// the main loop rather than by callback, so nothing here touches gadget state.

/// Start an async GET. Returns FALSE if a fetch is already in flight.
Bool liveRelayBeginFetch(const AsciiString& url);

/// Collect a finished fetch. Returns TRUE exactly once per completed request.
Bool liveRelayPollFetch(AsciiString& outBody, Bool& outSuccess, Int& outStatusCode);

/// TRUE while a request is outstanding.
Bool liveRelayFetchInFlight();

// ---------------------------------------------------------------------------
// GO services calls
//
// Livestreams are orchestrated by GO, not by the relay: GO owns the list of what is being
// streamed and mints the single-use credentials for both watching and streaming. The relay only
// honours a credential GO issued, so every one of these calls needs the player's session token -
// which is why the live game browser requires a sign-in.

/// One live game, as GO describes it, already parsed and display-ready. GO's JSON shape stays
/// out of the menu, so a contract change is a change here rather than in a GUI callback.
struct LiveGameEntry
{
	AsciiString lobbyId;      ///< GO's LobbyID as decimal text; also the relay's session key.
	AsciiString name;         ///< The lobby's display name (popup titles, diagnostics).
	AsciiString mapName;      ///< Display name, never a path.
	AsciiString players;      ///< Human players, comma separated.
	Int observerCount;
	Int delaySeconds;
	Int ageSeconds;
	Int state;                ///< 1 = live stream, 0 = pre-game lobby (read-only observe).
	Bool passworded;
	Int pendingObserverCount; ///< Pre-game observers waiting on this lobby (0 when live).

	/// What GO says to do with this row, computed per viewer:
	/// 0 = observe (pre-game - enter the read-only lobby view),
	/// 1 = wait (stream not live yet, or this viewer is held behind the broadcast delay -
	///     enter the read-only lobby view and wait there),
	/// 2 = join (stream live and this viewer may mint now - connect directly, skip the lobby).
	Int watchAction;

	/// Remaining broadcast-delay hold in seconds for this viewer (0 when not held).
	Int delayRemainingSeconds;

	/// TRUE when the lobby is a priority-player match (latched by GO at create/join). The
	/// browser marks these rows gold.
	Bool priority;

	LiveGameEntry() : observerCount(0), delaySeconds(0), ageSeconds(0),
		state(1), passworded(FALSE), pendingObserverCount(0),
		watchAction(2), delayRemainingSeconds(0), priority(FALSE) {}
};

/// Parse a GO /Livestreams reply into entries. FALSE when the body is not usable at all;
/// an empty list with TRUE simply means nobody is streaming.
Bool liveServicesParseLivestreams(const AsciiString& body, std::vector<LiveGameEntry>& outGames);

/// Full URL for a GO services endpoint, e.g. liveServicesEndpoint("Livestreams").
AsciiString liveServicesEndpoint(const char* szEndpoint);

/// Blocking authenticated request against GO services. For callers already on a worker thread
/// (the observer's and streamer's own network threads) - never call this from the main loop.
/// Returns FALSE when not signed in or when the request could not be made at all; outStatusCode
/// carries GO's reply otherwise, which the caller must still check.
Bool liveServicesRequest(const AsciiString& url, Bool bPost, const char* szPostBody,
	AsciiString& outBody, Int& outStatusCode);

// Queueing and pumping a live-observer session lives in GameClient/LiveObserverSession.h -
// it sequences shell screens (TheShell, transitions, the password popup), which is not this
// layer's business.

// ---------------------------------------------------------------------------------------
// Live observer/streamer file logging.
//
// Controlled by the RTS_DEBUG_LIVE_OBSERVER cmake option (DEFAULT/ON/OFF), resolving the same way
// as DEBUG_LOGGING in Debug.h. Kept separate from RTS_DEBUG_LOGGING because this log flushes every
// line, so it survives a crash.
//
// Enable in a release build with:  cmake --preset win32 -DRTS_DEBUG_LIVE_OBSERVER=ON
#if defined(ALLOW_DEBUG_UTILS) && !defined(LIVE_OBSERVER_LOGGING) && !defined(DISABLE_LIVE_OBSERVER_LOGGING)
	#define LIVE_OBSERVER_LOGGING 1
#endif

// Identifies the build that produced a log, so a stale binary is not debugged by mistake. Bump on
// every change to the instrumentation. Shared by LiveObserver.cpp and LiveStreamer.cpp, which must
// therefore include this header - that is also what resolves LIVE_OBSERVER_LOGGING above, without
// which logging silently stays off in a DEFAULT build.
#define LIVE_OBSERVER_BUILD_TAG "2026-08-15-observer-pace-toggle"

void liveObserverLog(const char* fmt, ...);
void liveObserverInitLog(const char* lobbyId);

// Gates instrumentation outside these two files so it only fires for an actual live-observer
// session, and not on every game start (including the streamer's own local game). Expands to
// nothing when logging is off, so the arguments are not evaluated either - unlike a plain call to
// the (then empty) liveObserverLog. Callers must have included Common/Recorder.h for
// TheRecorder/RECORDERMODETYPE_LIVE_OBSERVER.
#if defined(LIVE_OBSERVER_LOGGING)
	#define LIVE_OBSERVER_LOG(...) \
		do { if (TheRecorder && TheRecorder->getMode() == RECORDERMODETYPE_LIVE_OBSERVER) (liveObserverLog)(__VA_ARGS__); } while (0)
#else
	#define LIVE_OBSERVER_LOG(...) do { } while (0)
#endif

#endif // GENERALS_ONLINE
