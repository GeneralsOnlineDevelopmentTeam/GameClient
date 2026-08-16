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

// FILE: LiveStreamer.h ///////////////////////////////////////////////////////////////////////////
// Uploads a live match's replay bytes to the relay server, for live observers to play back.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/AsciiString.h"
#include "Common/GameCommon.h"
#include "Common/ReplayStreamSink.h"
#include "Common/UnicodeString.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <queue>
#include <deque>
#include <string>

/**
 * Binary message types sent over WebSocket between streamer/observer and relay.
 */
enum LiveMsgType : unsigned char {
	LIVE_MSG_REGISTER = 0,
	LIVE_MSG_HEADER    = 1,
	LIVE_MSG_PATCH     = 2,
	LIVE_MSG_BODY      = 3,
	LIVE_MSG_END       = 4,
	LIVE_MSG_ROLE      = 5,
	LIVE_MSG_ERROR     = 6,
	LIVE_MSG_CHAT      = 7,  // player chat: [frame u32][textLen u32][UTF-8 text][color u32]
	LIVE_MSG_SPECTATOR_CHAT = 8,  // spectator chat: [nameLen u32][UTF-8 name][textLen u32][UTF-8 text]
	LIVE_MSG_TICK      = 9,  // frame heartbeat: [frame u32]
	LIVE_MSG_STATS     = 10, // match telemetry: [logicFps u32][pingMs u32] - see publishStats()
};

class LiveStreamer;

/**
 * Everything the relay needs to open a session. The lobby fills it in and leaves it pending; the
 * Recorder sends it at match start, which keeps Recorder.cpp free of any GeneralsOnline includes.
 * Every player registers, since any of them can supply replay bytes, but only the host describes
 * the game - otherwise the description would be whichever REGISTER happened to arrive first.
 */
struct LiveStreamRegistration
{
	/// GO's LobbyID as plain decimal, spelled exactly as GO's /Lobbies JSON prints it. Doubles as
	/// the relay's session key and the id an observer watches by (/watch/<lobbyId>).
	AsciiString lobbyId;
	/// Local player, for the relay's logs only. Never used to identify the session.
	AsciiString playerName;
	/// TRUE when the local player owns this lobby. Gates the two host-only fields below.
	Bool isHost;
	/// Whether this client is willing to upload replay bytes. This machine's bandwidth, so unlike
	/// the fields below it is not the host's to decide.
	Bool canStream;

	/// HOST ONLY. Complete JSON object literal describing the lobby, in GO's own key spelling,
	/// already escaped by its builder. Empty on non-hosts, which send the lobby id alone.
	std::string lobbyJson;
	/// HOST ONLY. Broadcast delay every observer is held behind. Negative means "not mine to say".
	Int delaySeconds;

	LiveStreamRegistration() : isHost(FALSE), canStream(FALSE), delaySeconds(-1) {}

	Bool isValid() const { return !lobbyId.isEmpty(); }
};

/// Hand a completed registration to the Recorder. Safe to call repeatedly - the last one wins,
/// so a lobby that changes between the player arriving and the match starting is not a problem.
void liveStreamSetPendingRegistration(const LiveStreamRegistration& registration);

/// Drop anything pending. Called when a lobby is left without starting a match, so a later,
/// unrelated recording cannot pick up a stale lobby's registration.
void liveStreamClearPendingRegistration();

/// Open the pending session: create TheLiveStreamer, connect it to the relay and send REGISTER.
/// Returns the streamer for the caller to hook in as a replay sink, or nullptr when nothing is
/// pending or live streaming is switched off - in which case the game simply records as usual.
LiveStreamer* liveStreamStartPendingSession();

/**
 * Forwards raw replay bytes to the relay over a WebSocket, in a simple binary envelope. Has no
 * knowledge of the replay file format - it receives header/body/patch bytes from the Recorder.
 */
class LiveStreamer : public IReplayStreamSink
{
public:
	LiveStreamer();
	virtual ~LiveStreamer();

	/// IReplayStreamSink - called by Recorder during recording
	virtual void onHeaderBytes(const void* data, Int size) override;
	virtual void onHeaderComplete() override;
	virtual void onHeaderPatch(Int offset, const void* data, Int size) override;
	virtual void onBodyBytes(const void* data, Int size) override;
	virtual void onBodyFlush() override;
	virtual void onRecordingEnded() override;

	/// Start the network thread, which registers the stream with GO and connects to whatever
	/// relay URL GO returns. Non-blocking.
	void init();

	/// Shut down the background thread and close the connection.
	void close();

	/// Register a session with the relay server. See LiveStreamRegistration.
	void registerForGame(const LiveStreamRegistration& registration);

	/// The relay has confirmed the session with a role of "streamer", "backup" or "none". A backup
	/// stops uploading but keeps recording locally, so it can take over later.
	void onRoleAssigned(const AsciiString& role, const AsciiString& lobbyId, uint64_t bodyOffset);

	/// Promoted from backup to active streamer. Backfills the relay's missing bytes from the local
	/// recording starting at bodyOffset, then resumes live - seamless because a demoted backup
	/// never stopped recording.
	void onTakeover(uint64_t bodyOffset);

	/// m_isBackup gates data flow, not just the UI: while backup the sink drops HEADER/PATCH/BODY
	/// (END is still sent) so a demoted streamer stops using its uplink.
	Bool isStreaming() const { return m_isStreaming.load(); }
	Bool isBackup() const { return m_isBackup.load(); }
	AsciiString getLobbyId() const { return m_lobbyId; }

	/// Forward a displayed global chat line to the relay (MSG_CHAT).
	virtual void onChat(UnsignedInt frame, const UnicodeString& text, UnsignedInt colorArgb) override;

	/// Publish our current logic frame (MSG_TICK), so an observer can follow the live edge through
	/// quiet play, when the body carries only one CRC record per REPLAY_CRC_INTERVAL frames.
	/// Sent immediately after onBodyFlush() for the same frame and frames leave in queue order, so
	/// a tick for N proves every record up to N has been sent - the observer may simulate to N.
	virtual void onTick(UnsignedInt frame) override;

	/// Drain spectator chat received from the relay into the HUD message log. Called once per
	/// logic frame while recording a live-streamed game.
	void pumpSpectatorChat();

	/// Publish this client's logic frame rate and ping (MSG_STATS), so an observer can show the
	/// same numbers a player in the match sees. Sampled from onTick, which already runs on a fixed
	/// frame cadence, and sent only when a value actually moves - a ping walking 64 -> 92 is two
	/// messages, not sixty. See LIVE_STATS_* for the quantisation that makes that true.
	void publishStats(UnsignedInt frame);

	struct QueuedFrame
	{
		unsigned char type;
		std::vector<char> data;
	};

private:
	void networkThreadFunc();

	/// Ask GO to register this livestream and mint our single-use stream token, returning the
	/// relay URL to connect to. Blocking, so network thread only.
	bool requestStreamUrl(AsciiString& outUrl);

	bool connectToRelay();

	/// Tri-state send outcome: Sent = frame handed to the socket, WouldBlock = socket buffer
	/// full (CURLE_AGAIN - nothing was sent, retry the same frame later), Error = connection
	/// is gone. WouldBlock is a pause, never a failure: the relay is merely reading slowly.
	enum class WsSendResult { Sent, WouldBlock, Error };
	WsSendResult wsSendBinary(const unsigned char* data, size_t len);
	bool wsRecv(std::vector<char>& outBuffer);
	WsSendResult sendBinaryFrame(LiveMsgType type, const void* payload, size_t payloadLen);
	WsSendResult sendBinaryFrame(const QueuedFrame& frame);
	void queueFrame(LiveMsgType type, const void* data, size_t len);

	// ---- MSG_STATS: send-on-change telemetry ---------------------------------------------
	//
	// The readout is a counter on the observer's HUD, so it needs each value when it changes and
	// nothing in between. Two rules make "on change" mean something:
	//
	// - Quantise before comparing. A raw ping wobbling by a millisecond is a change on every
	//   sample, and the deduplication would buy nothing at all.
	// - Bound the rate from both sides. The floor stops a genuinely noisy value flooding the
	//   relay; the heartbeat ceiling means a joiner is not left with a blank readout on a value
	//   that happens to be stable, and a stuck reading is visibly stuck rather than silently
	//   stale.
	enum { LIVE_STATS_PING_QUANTUM_MS = 5 };
	enum { LIVE_STATS_MIN_INTERVAL_MS = 500 };
	enum { LIVE_STATS_HEARTBEAT_MS = 5000 };
	enum { LIVE_STATS_PING_MAX_MS = 2000 };

	Int m_lastSentLogicFps;		// -1 = nothing sent yet
	Int m_lastSentPingMs;
	UnsignedInt m_lastStatsSentMs;

	// Previous sample for the achieved logic rate. The reported rate must be frames actually
	// advanced per wall-clock second, not TheNetwork->getFrameRate() - that is the rate the mesh
	// negotiated, and a loaded host sits far below it (60 negotiated while stepping 11, observed
	// 2026-08-15 with eight instances on one machine). Reporting the negotiated rate would tell a
	// viewer the match is healthy while they watch it crawl, and would disagree with the rate the
	// observer derives for itself from the live edge.
	UnsignedInt m_statsLastFrame;
	UnsignedInt m_statsLastFrameMs;

	// UI-informational flags; m_isBackup additionally gates data flow (see onRoleAssigned)
	std::atomic<Bool> m_isStreaming;
	std::atomic<Bool> m_isBackup;
	std::atomic<Bool> m_connected;
	std::atomic<Bool> m_shouldRun;

	// Timestamp of the last websocket frame of any kind, including the relay's ~20 s keepalive
	// pings. Zero means nothing received yet, so the watchdog cannot fire before the first ROLE.
	std::atomic<UnsignedInt> m_lastFrameReceivedMs{ 0 };

	// Silence this long while connected means the relay, or the path to it, is gone; without a
	// timeout the streamer uploads into a dead socket forever. 120 s = ~6 missed pings.
	enum { LIVE_STREAM_WATCHDOG_MS = 120000 };

	// Why the network thread ended (shutdown / relay-silent / send-failed / relay-error), printed
	// in the thread-end summary so a dead stream is always attributable to a side.
	AsciiString m_endReason;

	// Bytes and frames actually put on the wire, for the thread-end summary.
	size_t m_sentBytes;
	size_t m_sentFrames;

	AsciiString m_lobbyId;
	/// Host-only fields kept from the registration, because the stream is registered with GO
	/// from the network thread and the registration struct is gone by then.
	Bool m_isHost;
	Int m_delaySeconds;
	AsciiString m_playerName;

	void* m_curlEasy;
	void* m_curlMulti;

	std::thread m_networkThread;
	mutable std::mutex m_sendMutex;

	// Written by the network thread, drained by pumpSpectatorChat on the game thread. This client
	// is a source, so spectator chat is received and never sent.
	struct SpectatorChatEntry
	{
		UnicodeString displayName;
		UnicodeString text;
	};
	std::deque<SpectatorChatEntry> m_spectatorChatQueue;
	mutable std::mutex m_spectatorChatMutex;

	// deque, not queue: on CURLE_AGAIN unsent frames go back at the FRONT, since order is data
	// and a misordered stream is corrupt.
	std::deque<QueuedFrame> m_outgoingQueue;
	/// Bytes queued, and whether the budget has been blown. Frames are queued before the relay
	/// connection exists, so a registration GO refuses would otherwise grow the queue all match
	/// with nothing draining it.
	size_t m_queuedBytes;
	bool m_queueOverflowed;

	// Header accumulation - buffered until onHeaderComplete()
	std::vector<char> m_headerBuffer;

	// One buffer, both roles. While streaming it flushes every BODY_FLUSH_THRESHOLD bytes. While
	// backup, onBodyFlush is a no-op, so the same buffer accumulates the body from the demotion
	// point onward - the backfill source a later takeover needs. m_bodySentOffset freezes at
	// buffer[0]'s absolute offset while backup so takeover offsets stay correct. Guarded by
	// m_sendMutex: game thread writes, network thread takes over.
	std::vector<char> m_bodyBuffer;
	static const size_t BODY_FLUSH_THRESHOLD = 4096;
	uint64_t m_bodySentOffset;   // absolute file offset for next BODY chunk
	/// Ceiling on the backup accumulation. On overflow the oldest bytes are dropped and
	/// m_bodySentOffset advances, so a takeover older than the retained window skips forward.
	static const size_t BODY_BUFFER_MAX = 8 * 1024 * 1024;
};

extern LiveStreamer* TheLiveStreamer;
LiveStreamer* createLiveStreamer();

void liveStreamLog(const char* fmt, ...);

/// Escape a string for embedding in a JSON string literal. UTF-8 bytes pass through unchanged,
/// being already valid there.
std::string liveStreamJsonEscape(const char* str);
