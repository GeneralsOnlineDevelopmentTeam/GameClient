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

#include "PreRTS.h"

#if defined(GENERALS_ONLINE)

#include "Common/LiveObserver.h"
#include "Common/Recorder.h"
#include "Common/GlobalData.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "Common/FramePacer.h"		// the pace controller drives the logic time scale; see updatePlaybackPace
#include "GameLogic/GameLogic.h"	// the buffering gate pauses the game; see updatePlaybackGate
#include "GameClient/ClientInstance.h"
#include "GameClient/InGameUI.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/websockets.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <fstream>		// cacert.pem presence check, see connectToRelay
#include <string>
#include <thread>
#include <windows.h>

// ============================================================================
// liveObserverLog
// ============================================================================
// LIVE_OBSERVER_BUILD_TAG and the LIVE_OBSERVER_LOGGING gate both live in LiveObserver.h.

void liveObserverLog(const char* fmt, ...) {
#if !defined(LIVE_OBSERVER_LOGGING)
	(void)fmt;
#else
	static FILE* logFile = NULL;
	if (!logFile) {
		// Per-instance name: streamer and observer are the same exe and can share an install
		// directory, so a bare relative filename means both processes truncate the same log.
		AsciiString path;
		path.format("live_observer_debug_Instance%.2u.log", rts::ClientInstance::getInstanceId());
		logFile = fopen(path.str(), "w");
		if (logFile)
			fprintf(logFile, "LIVE_OBSERVER_BUILD_TAG=%s\n", LIVE_OBSERVER_BUILD_TAG);
	}
	if (logFile) {
		// Wall-clock prefix, so several instances' logs (and the relay's) can be aligned.
		SYSTEMTIME st;
		GetLocalTime(&st);
		fprintf(logFile, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		vfprintf(logFile, fmt, args);
		va_end(args);
		fflush(logFile);
	}
#endif // LIVE_OBSERVER_LOGGING
}

void liveObserverInitLog(const char* lobbyId) {
	liveObserverLog("=== Live Observer Init ===\n");
	liveObserverLog("Lobby: %s\n", lobbyId ? lobbyId : "(empty)");
	liveObserverLog("Observer mode activated\n");
}

// ============================================================================
// LiveObserver
// ============================================================================
LiveObserver* TheLiveObserver = nullptr;

LiveObserver::LiveObserver()
	: m_connected(false)
	, m_shouldRun(false)
	, m_headerReceived(false)
	, m_streamEnded(false)
	, m_maxCompleteFrame(0)
	, m_safeReadOffset(0)
	, m_parseAbsOffset(0)
	, m_bodyStartOffset(0)
	, m_parseCorrupt(false)
	, m_parseGapPending(false)
	, m_liveFrameHint(0)
	, m_holdPlayback(FALSE)
	, m_nearLiveHeld(FALSE)
	, m_preRollComplete(FALSE)
	, m_autoPaused(FALSE)
	, m_userPaused(FALSE)
	, m_stalled(FALSE)
	, m_playbackStarted(FALSE)
	, m_lastSeenLiveEdge(0)
	, m_lastLiveEdgeChangeMs(timeGetTime())
	, m_desyncFrame(0)
	, m_lastGateLogMs(0)
	, m_lastGateLogFrame(0)
	, m_lastGateLogEdge(0)
	, m_underrunCount(0)
	, m_paceSampleCount(0)
	, m_sourceFps(0)
	, m_paceFps(LOGICFRAMES_PER_SECOND)
	, m_lastPaceApplyMs(0)
	, m_paceMatchingEnabled(TRUE)
	, m_pacerTouched(FALSE)
	, m_savedLogicScaleFps(0)
	, m_savedLogicScaleEnabled(FALSE)
	, m_delaySeconds(LIVE_DELAY_SECONDS_DEFAULT)
	, m_serverHeld(FALSE)
	, m_delayWaitActive(FALSE)
	, m_delayWaitDeadlineMs(0)
	, m_expectedDelaySeconds(-1)
	, m_spectatorChatMode(SPECTATOR_CHAT_AUTO)
	, m_liveFile(nullptr)
	, m_curlEasy(nullptr)
	, m_curlMulti(nullptr)
{
	// Every field above is session state and this constructor is the only place it is ever
	// initialised: a session begins when the object is created and ends when it is destroyed,
	// so there is no "reset the previous session" step to forget.
}

// ============================================================================
// Standalone relay HTTP fetch (live game browser)
// ============================================================================

namespace
{
	std::mutex s_fetchMutex;
	std::atomic<bool> s_fetchInFlight(false);
	std::atomic<bool> s_fetchReady(false);
	std::string s_fetchBody;
	bool s_fetchSuccess = false;
	long s_fetchStatus = 0;

	size_t liveRelayWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata)
	{
		std::string* out = static_cast<std::string*>(userdata);
		out->append(ptr, size * nmemb);
		return size * nmemb;
	}

	// Shared setup for every GO services call made from this file. Identical CA handling and
	// timeouts whether the caller is the browser's background fetch, the observer asking for a
	// watch ticket, or the streamer registering a stream - one place to get this right.
	//
	// Returns the header list, which the caller owns and must curl_slist_free_all().
	curl_slist* liveServicesConfigureCurl(CURL* easy, std::string* outBody, const std::string& authToken)
	{
		curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, liveRelayWriteCb);
		curl_easy_setopt(easy, CURLOPT_WRITEDATA, outBody);
		curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
		// Keep this short: these run behind a menu the user is looking at, or in the moment a
		// match starts, and a hung service must not leave either of them hanging.
		curl_easy_setopt(easy, CURLOPT_TIMEOUT, 10L);
		curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 5L);

		// Same CA handling as connectToRelay - this libcurl is OpenSSL-backed and has
		// no trust anchors of its own, so https:// fails without an explicit bundle.
		std::ifstream certFile("cacert.pem");
		if (certFile.good())
		{
			certFile.close();
			curl_easy_setopt(easy, CURLOPT_CAINFO, "cacert.pem");
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
		}
		else
		{
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
		}

		curl_slist* headers = curl_slist_append(nullptr, "Accept: application/json");
		if (!authToken.empty())
		{
			const std::string authHeader = "Authorization: Bearer " + authToken;
			headers = curl_slist_append(headers, authHeader.c_str());
		}
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		return headers;
	}

	void liveRelayFetchThread(std::string url, std::string authToken)
	{
		std::string body;
		bool success = false;
		long status = 0;

		CURL* easy = curl_easy_init();
		if (easy)
		{
			curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
			curl_slist* headers = liveServicesConfigureCurl(easy, &body, authToken);

			CURLcode res = curl_easy_perform(easy);
			curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
			success = (res == CURLE_OK);
			if (!success)
				liveObserverLog("liveRelayFetch: curl failed (result=%d) for %s\n", (int)res, url.c_str());
			curl_slist_free_all(headers);
			curl_easy_cleanup(easy);
		}

		{
			std::lock_guard<std::mutex> lock(s_fetchMutex);
			s_fetchBody = body;
			s_fetchSuccess = success;
			s_fetchStatus = status;
		}
		s_fetchReady.store(true);
		s_fetchInFlight.store(false);
	}
}

// The signed-in player's session token, or an empty string when not signed in.
static std::string liveServicesAuthToken()
{
	NGMP_OnlineServices_AuthInterface* pAuthInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface == nullptr || !pAuthInterface->IsLoggedIn())
		return std::string();

	return pAuthInterface->GetAuthToken();
}

AsciiString liveServicesEndpoint(const char* szEndpoint)
{
	// Static on the manager, so this resolves whether or not the player has signed in.
	return AsciiString(NGMP_OnlineServicesManager::GetAPIEndpoint(szEndpoint).c_str());
}

Bool liveServicesParseLivestreams(const AsciiString& body, std::vector<LiveGameEntry>& outGames)
{
	outGames.clear();

	try
	{
		// GO answers with { "livestreams": [ ... ] }, already filtered to what this player may
		// watch - lobbies in progress whose relay session is live.
		nlohmann::json response = nlohmann::json::parse(body.str());
		if (!response.is_object() || !response.contains("livestreams"))
			return FALSE;

		const nlohmann::json& games = response["livestreams"];
		if (!games.is_array())
			return FALSE;

		for (const auto& game : games)
		{
			if (!game.is_object())
				continue;

			LiveGameEntry entry;

			// lobby_id is a number in GO's JSON, and the relay keys its sessions by the same
			// value as decimal text, so it is formatted, not read as a string.
			if (game.contains("lobby_id") && game["lobby_id"].is_number_integer())
				entry.lobbyId.format("%lld", (long long)game["lobby_id"].get<long long>());
			if (entry.lobbyId.isEmpty())
				continue;

			// map_name is a display name, not a path, so it needs no leaf/extension stripping.
			// A game missing metadata is still watchable, so fall back rather than drop the row.
			const std::string mapName = game.value("map_name", std::string(""));
			entry.mapName = mapName.empty() ? "(unknown map)" : mapName.c_str();

			// The lobby's display name - used for the password popup title on passworded rows.
			const std::string lobbyName = game.value("name", std::string(""));
			entry.name = lobbyName.empty() ? entry.mapName : lobbyName.c_str();

			// players[] arrives already reduced to the humans in the lobby - no empty slots.
			std::string playerList;
			if (game.contains("players") && game["players"].is_array())
			{
				for (const auto& player : game["players"])
				{
					if (!player.is_string())
						continue;

					const std::string name = player.get<std::string>();
					if (name.empty())
						continue;

					if (!playerList.empty())
						playerList += ", ";
					playerList += name;
				}
			}
			entry.players = playerList.empty() ? "?" : playerList.c_str();

			// delay_seconds and age_seconds are nullable in GO's contract, so present-but-null has
			// to be treated as absent: value() would throw on it.
			entry.observerCount = game.value("observer_count", 0);
			entry.delaySeconds = (game.contains("delay_seconds") && game["delay_seconds"].is_number_integer())
				? game["delay_seconds"].get<Int>() : (Int)LIVE_DELAY_SECONDS_DEFAULT;
			entry.ageSeconds = (game.contains("age_seconds") && game["age_seconds"].is_number_integer())
				? game["age_seconds"].get<Int>() : 0;

			// Defaults keep an older GO's live-only rows usable: live, not passworded, none waiting.
			entry.state = (game.contains("state") && game["state"].is_number_integer())
				? game["state"].get<Int>() : 1;
			entry.passworded = game.value("passworded", false) ? TRUE : FALSE;
			entry.pendingObserverCount = game.value("pending_observer_count", 0);

			// watch_action is GO's per-viewer directive (0 observe / 1 wait / 2 join). Absent on
			// older GO, derive it from the state: live rows join, pre-game rows observe.
			entry.watchAction = (game.contains("watch_action") && game["watch_action"].is_number_integer())
				? game["watch_action"].get<Int>() : (entry.state == 1 ? 2 : 0);
			entry.delayRemainingSeconds = (game.contains("delay_remaining_seconds") &&
				game["delay_remaining_seconds"].is_number_integer())
				? game["delay_remaining_seconds"].get<Int>() : 0;

			// priority: GO latches the lobby when a user_priority = Player creates/joins.
			// Absent on older GO = not priority.
			entry.priority = game.value("priority", false) ? TRUE : FALSE;

			outGames.push_back(entry);
		}
	}
	catch (const nlohmann::json::exception&)
	{
		outGames.clear();
		return FALSE;
	}

	return TRUE;
}

Bool liveServicesRequest(const AsciiString& url, Bool bPost, const char* szPostBody,
	AsciiString& outBody, Int& outStatusCode)
{
	outBody = AsciiString::TheEmptyString;
	outStatusCode = 0;

	const std::string authToken = liveServicesAuthToken();
	if (authToken.empty())
	{
		liveObserverLog("liveServicesRequest: %s refused (not signed in)\n", url.str());
		return FALSE;
	}

	CURL* easy = curl_easy_init();
	if (easy == nullptr)
	{
		liveObserverLog("liveServicesRequest: %s failed (curl init)\n", url.str());
		return FALSE;
	}

	std::string body;
	curl_easy_setopt(easy, CURLOPT_URL, url.str());
	curl_slist* headers = liveServicesConfigureCurl(easy, &body, authToken);

	if (bPost)
	{
		// GO reads the body itself rather than through a model binder, so an empty POST still
		// needs a real (zero-length) body rather than no body at all.
		const char* szBody = (szPostBody != nullptr) ? szPostBody : "";
		headers = curl_slist_append(headers, "Content-Type: application/json");
		curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(easy, CURLOPT_POST, 1L);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDS, szBody);
		curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, (long)strlen(szBody));
	}

	const CURLcode res = curl_easy_perform(easy);
	long status = 0;
	curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(easy);

	outBody = body.c_str();
	outStatusCode = (Int)status;

	if (res != CURLE_OK)
	{
		liveObserverLog("liveServicesRequest: %s failed (result=%d)\n", url.str(), (int)res);
		return FALSE;
	}

	return TRUE;
}

Bool liveRelayBeginFetch(const AsciiString& url)
{
	bool expected = false;
	if (!s_fetchInFlight.compare_exchange_strong(expected, true))
		return FALSE;	// one already running

	// Read the token here, on the calling thread: the auth interface is not safe to reach from
	// the fetch thread, and it is a plain string by the time it crosses over.
	const std::string authToken = liveServicesAuthToken();
	if (authToken.empty())
	{
		// GO gates the livestream list behind a GameClient session, so there is nothing to ask
		// for when signed out. Release the in-flight flag rather than leaving the browser
		// believing a request is running.
		s_fetchInFlight.store(false);
		liveObserverLog("liveRelayFetch: skipped %s (not signed in)\n", url.str());
		return FALSE;
	}

	s_fetchReady.store(false);
	liveObserverLog("liveRelayFetch: GET %s\n", url.str());

	std::thread(liveRelayFetchThread, std::string(url.str()), authToken).detach();
	return TRUE;
}

Bool liveRelayPollFetch(AsciiString& outBody, Bool& outSuccess, Int& outStatusCode)
{
	if (!s_fetchReady.load())
		return FALSE;

	std::lock_guard<std::mutex> lock(s_fetchMutex);
	// Re-check under the lock so two callers in one frame cannot both consume the result.
	if (!s_fetchReady.load())
		return FALSE;

	outBody = s_fetchBody.c_str();
	outSuccess = s_fetchSuccess ? TRUE : FALSE;
	outStatusCode = (Int)s_fetchStatus;
	s_fetchReady.store(false);
	return TRUE;
}

Bool liveRelayFetchInFlight()
{
	return s_fetchInFlight.load() ? TRUE : FALSE;
}

// ============================================================================
// Replay-record scanner
// ============================================================================
//
// One replay record on disk is laid out as:
//   [UnsignedInt frame][GameMessage::Type type][Int playerIndex][UnsignedByte numTypes]
//   { [UnsignedByte argType][UnsignedByte numArgs] } x numTypes
//   [argument payload]
//
// This must agree byte-for-byte with RecorderClass::appendNextCommand(), which consumes these
// records during playback. If appendNextCommand() or readArgument() ever change what they read,
// change this with them.
//
// It fails closed: an unparseable record stalls the watermark rather than poisoning it.

enum ScanRecordResult CPP_11(: Int)
{
	SCANRECORD_OK,              ///< a complete record is present; outSize/outFrame are valid
	SCANRECORD_INCOMPLETE,      ///< the buffer holds a valid prefix - more bytes needed
	SCANRECORD_CORRUPT          ///< unparseable (e.g. unknown argument type)
};

// Size of one replay argument on disk. Must match RecorderClass::readArgument() exactly. Returns
// -1 for anything unrecognised, so callers fail closed instead of skipping zero bytes and
// desyncing the rest of the parse.
static Int replayArgumentSize(UnsignedByte argType)
{
	switch ((GameMessageArgumentDataType)argType) {
		case ARGUMENTDATATYPE_INTEGER:      return sizeof(Int);
		case ARGUMENTDATATYPE_REAL:         return sizeof(Real);
		case ARGUMENTDATATYPE_BOOLEAN:      return sizeof(Bool);
		case ARGUMENTDATATYPE_OBJECTID:     return sizeof(ObjectID);
		case ARGUMENTDATATYPE_DRAWABLEID:   return sizeof(DrawableID);
		case ARGUMENTDATATYPE_TEAMID:       return sizeof(UnsignedInt);
		case ARGUMENTDATATYPE_LOCATION:     return sizeof(Coord3D);
		case ARGUMENTDATATYPE_PIXEL:        return sizeof(ICoord2D);
		case ARGUMENTDATATYPE_PIXELREGION:  return sizeof(IRegion2D);
		case ARGUMENTDATATYPE_TIMESTAMP:    return sizeof(UnsignedInt);
		case ARGUMENTDATATYPE_WIDECHAR:     return sizeof(WideChar);
		default:                            return -1;
	}
}

/// Scan one replay record from buf[0..len). Never reads past len. outSize/outFrame may be null.
static ScanRecordResult scanReplayRecord(const unsigned char* buf, Int len, Int* outSize, UnsignedInt* outFrame)
{
	// numTypes and numArgs are single bytes, so a well-formed record cannot exceed
	// 9 + 255*2 + 255*255*sizeof(IRegion2D) bytes. Anything claiming more is misparsed data
	// rather than a record still arriving, so report it instead of stalling forever.
	const Int MAX_SANE_RECORD_SIZE = 2 * 1024 * 1024;

	const Int fixedSize = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int) + sizeof(UnsignedByte);
	if (len < fixedSize)
		return SCANRECORD_INCOMPLETE;

	UnsignedInt frame;
	memcpy(&frame, buf, sizeof(frame));

	Int pos = sizeof(UnsignedInt) + sizeof(GameMessage::Type) + sizeof(Int);
	UnsignedByte numTypes = buf[pos];
	pos += sizeof(UnsignedByte);

	// All (argType, numArgs) pairs are written consecutively, and only then the argument payload
	// for every type in order - see appendNextCommand(), which reads the full pair list before its
	// readArgument() loop. So accumulate the payload size and add it once, after the pair list;
	// skipping each type's payload inside this loop is correct only when numTypes == 1.
	Int payloadSize = 0;
	for (UnsignedByte i = 0; i < numTypes; ++i) {
		if (pos + 2 > len)
			return SCANRECORD_INCOMPLETE;

		UnsignedByte argType = buf[pos];
		UnsignedByte numArgs = buf[pos + 1];
		pos += 2;

		Int argSize = replayArgumentSize(argType);
		if (argSize < 0)
			return SCANRECORD_CORRUPT;

		payloadSize += argSize * (Int)numArgs;
		if (payloadSize > MAX_SANE_RECORD_SIZE)
			return SCANRECORD_CORRUPT;
	}

	pos += payloadSize;
	if (pos > len)
		return SCANRECORD_INCOMPLETE;

	if (outSize)
		*outSize = pos;
	if (outFrame)
		*outFrame = frame;
	return SCANRECORD_OK;
}

// ============================================================================
// Parse cursor - publishes the live-edge and safe-read watermarks
// ============================================================================

void LiveObserver::resetParseCursor(Int bodyStartOffset)
{
	m_parseTail.clear();
	m_parseAbsOffset = bodyStartOffset;
	m_bodyStartOffset = bodyStartOffset;
	m_parseCorrupt = false;
	m_parseGapPending = false;
	m_maxCompleteFrame.store(0);
	m_liveFrameHint.store(0);
	m_srcLogicFps.store(0);
	m_srcPingMs.store(0);
	m_safeReadOffset.store(bodyStartOffset);
}

void LiveObserver::advanceParseCursor(Int chunkOffset, const unsigned char* data, size_t dataLen)
{
	if (m_parseCorrupt || dataLen == 0)
		return;

	// The relay appends body data strictly in order, so chunks normally arrive contiguously. The
	// exception is the observer-join race: a live chunk can reach a freshly-registered observer
	// before catch-up has sent the earlier ones, leaving a hole. Parsing across a hole would feed
	// uninitialised bytes to the scanner, so stall the watermark instead; the cursor resumes by
	// itself once the missing bytes are backfilled and a contiguous chunk arrives.
	Int expected = m_parseAbsOffset + (Int)m_parseTail.size();
	if (chunkOffset != expected)
	{
		// Latch it: the frame heartbeat's "every record up to N was sent" guarantee is worthless
		// while we know some of those records have not landed here yet.
		m_parseGapPending = true;
		liveObserverLog("LiveObserver: parse cursor gap - chunkOffset=%d expected=%d, watermark stalled\n",
			chunkOffset, expected);
		return;
	}

	m_parseGapPending = false;
	m_parseTail.insert(m_parseTail.end(), data, data + dataLen);

	Int consumed = 0;
	UnsignedInt maxFrame = m_maxCompleteFrame.load();
	const Int tailSize = (Int)m_parseTail.size();

	while (consumed < tailSize)
	{
		Int recSize = 0;
		UnsignedInt recFrame = 0;
		ScanRecordResult r = scanReplayRecord(&m_parseTail[consumed], tailSize - consumed, &recSize, &recFrame);

		if (r == SCANRECORD_INCOMPLETE)
			break;

		if (r == SCANRECORD_CORRUPT)
		{
			// Fail closed: freezing the watermark stops playback advancing into data we cannot
			// trust, which is recoverable and diagnosable.
			m_parseCorrupt = true;
			liveObserverLog("LiveObserver: parse cursor CORRUPT at abs offset %d - watermark frozen at frame %u\n",
				m_parseAbsOffset + consumed, maxFrame);
			break;
		}

		consumed += recSize;
		if (recFrame > maxFrame)
			maxFrame = recFrame;
	}

	if (consumed > 0)
	{
		m_parseTail.erase(m_parseTail.begin(), m_parseTail.begin() + consumed);
		m_parseAbsOffset += consumed;
		// Publish the frame before the offset: a reader that sees the new safe offset must
		// never see a stale live edge for the records it is now allowed to read.
		m_maxCompleteFrame.store(maxFrame);
		m_safeReadOffset.store(m_parseAbsOffset);
	}
}

// ============================================================================
// Buffering gate
// ============================================================================
//
// The observer never plays closer to the live game than the broadcast delay. The decision is made
// here because every input to it - the delay, the live edge, whether the initial buffer has been
// built - belongs to this session; the Recorder only carries out what this decides.

void LiveObserver::updatePlaybackGate(UnsignedInt curFrame)
{
	// Never hold the game before it has started, and not for the first few ticks after either.
	// The map load and object creation run inside GameLogic::update(), which the pause stops from
	// being called at all - so pausing this early means the game never starts while TheGameClient
	// keeps updating above the halt. Likewise the scene is not composed until logic has run for a
	// few ticks, so a hold at frame 1 renders nothing: not the map, and not the buffering
	// countdown meant to explain the wait. Frames barely advance in this window, so nothing is
	// lost by waiting it out.
	if (TheGameLogic == nullptr || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame()
		|| TheGameLogic->isStartingNewGame()
		|| curFrame < (UnsignedInt)LIVE_PREROLL_WARMUP_FRAMES)
	{
		if (TheGameLogic != nullptr && m_autoPaused && TheGameLogic->isGamePaused())
		{
			TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);
			m_autoPaused = FALSE;
		}
		// The gate is not being evaluated, so it must not keep reporting a hold from the
		// last tick it was.
		m_holdPlayback = FALSE;
		m_nearLiveHeld = FALSE;
		return;
	}

	const UnsignedInt liveEdge = getLiveEdge();
	const UnsignedInt gap = (liveEdge > curFrame) ? (liveEdge - curFrame) : 0;
	const UnsignedInt delayFrames = getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;
	const Bool streamEnded = m_streamEnded.load();

	// The two bounds are deliberately asymmetric, because they answer different questions.
	//
	// engageBelow - when must we stop? Only at the broadcast delay, which is a hard promise (a
	// normal viewer may never see closer to live than that). With no client-side delay it is 0:
	// nothing forces a hold except running out of data altogether.
	//
	// releaseAbove - how much do we rebuild before resuming? The target lead plus a margin.
	// Resuming the instant one frame is available leaves no cushion, so the next hiccup stalls
	// again and the equilibrium lead grinds to zero. The margin is one heartbeat interval, so the
	// observer plays a full tick's worth before nearing the engage bound again.
	//
	// The gate only ever chooses between running and waiting; playback is always exactly 100%.
	const UnsignedInt targetLead = getTargetLeadFrames();
	const UnsignedInt engageBelow = delayFrames;
	const UnsignedInt releaseAbove = targetLead + LIVE_GATE_RELEASE_MARGIN_FRAMES;

	// Fast-forward auto-disable. Uses the release bound so the join catch-up stops exactly
	// where the gate will settle: any closer to the edge and a fast-forward would spoil the
	// live game.
	if (gap <= releaseAbove)
	{
		if (TheWritableGlobalData)
			TheWritableGlobalData->m_TiVOFastMode = FALSE;
	}

	// Pre-roll: hold playback until the initial buffer has been built once, then latch for the
	// rest of the session - after this the lead is maintained by the near-live gate below. This
	// is where the jitter buffer is paid for: once, at join, as a fixed offset. A finished stream
	// is the escape hatch for a game that ends before ever buffering the full target; without it
	// a short game would pre-roll-pause forever.
	//
	// Built to releaseAbove rather than targetLead, so a session starts with the same lead a hold
	// resumes at. Pre-rolling to the bare target meant every session began on less than half the
	// cushion it maintains for the rest of the match (8 frames against 18 at the default 250 ms
	// jitter buffer), which makes the first hiccup an underrun by construction - a pause a few
	// seconds in, then steady, exactly as reported 2026-08-15. The extra wait at join is one
	// heartbeat interval.
	if (!m_preRollComplete && (gap >= releaseAbove || streamEnded))
		m_preRollComplete = TRUE;

	// The gate is purely a function of the gap, deliberately not of whether a record happened to
	// be readable this tick: an "did we hit EOF" term would make the two callers of this function
	// disagree, because the poll runs before GameLogic::UPDATE() and cannot know.
	const Bool wasNearLiveHeld = m_nearLiveHeld;
	if (m_preRollComplete && !streamEnded)
	{
		if (gap <= engageBelow)
			m_nearLiveHeld = TRUE;
		else if (gap > releaseAbove)
			m_nearLiveHeld = FALSE;
	}
	else
	{
		m_nearLiveHeld = FALSE;
	}
	// Every FALSE->TRUE is one underrun: the lead we banked was not enough for what just
	// happened. The count is the honest measure of whether the buffer is sized right.
	if (!wasNearLiveHeld && m_nearLiveHeld)
		++m_underrunCount;

	const Bool preRollGate = !m_preRollComplete;
	m_holdPlayback = (preRollGate || m_nearLiveHeld) && !streamEnded;

	// Distinguish normal delay-holding from a genuine stall for the status bar's benefit.
	// At the boundary the hold toggles constantly, which is healthy; what the observer
	// actually wants flagged is the source having stopped producing data altogether.
	const UnsignedInt nowMs = timeGetTime();
	if (liveEdge != m_lastSeenLiveEdge)
	{
		m_lastSeenLiveEdge = liveEdge;
		m_lastLiveEdgeChangeMs = nowMs;
	}
	m_stalled = m_holdPlayback && !streamEnded && (nowMs - m_lastLiveEdgeChangeMs) > LIVE_STALL_THRESHOLD_MS;

	// Rate-matched playback. Runs after the gate has decided, so the pace reported alongside a
	// hold is the one that goes with it, and so a paused tick cannot be paced off a frozen clock.
	updatePlaybackPace(nowMs, gap, targetLead, streamEnded);

	// Gate trace, once a second. Playback and source rates are reported as frames per interval
	// rather than as counters: if both read ~LOGICFRAMES_PER_SECOND the session is healthy, a
	// source rate below it means the streamer's own simulation is running slow (nothing the
	// buffer can fix - see the notes on rate-matched playback), and a source rate at nominal
	// while the gap collapses means the transport is losing ground.
	if (nowMs - m_lastGateLogMs >= 1000)
	{
		const UnsignedInt elapsedMs = (m_lastGateLogMs != 0) ? (nowMs - m_lastGateLogMs) : 0;
		if (elapsedMs > 0)
		{
			const UnsignedInt playedFrames = (curFrame > m_lastGateLogFrame) ? (curFrame - m_lastGateLogFrame) : 0;
			const UnsignedInt sourceFrames = (liveEdge > m_lastGateLogEdge) ? (liveEdge - m_lastGateLogEdge) : 0;
			liveObserverLog("GATE: cur=%u (%u/s) edge=%u (%u/s) src=%u pace=%u rec=%u hb=%u gap=%u "
				"engage=%u release=%u hold=%d preroll=%d stall=%d underruns=%u safeOff=%d\n",
				curFrame, playedFrames * 1000 / elapsedMs,
				liveEdge, sourceFrames * 1000 / elapsedMs,
				m_sourceFps, m_paceFps,
				m_maxCompleteFrame.load(), m_liveFrameHint.load(),
				gap, engageBelow, releaseAbove,
				m_holdPlayback ? 1 : 0, m_preRollComplete ? 1 : 0, m_stalled ? 1 : 0,
				m_underrunCount, m_safeReadOffset.load());
		}
		m_lastGateLogMs = nowMs;
		m_lastGateLogFrame = curFrame;
		m_lastGateLogEdge = liveEdge;
	}

	// The user's intent and ours are independent inputs to one decision, so a manual pause
	// can never be silently undone by buffering, nor vice versa.
	const Bool shouldBePaused = m_userPaused || m_holdPlayback;
	if (shouldBePaused != TheGameLogic->isGamePaused())
	{
		TheGameLogic->setGamePaused(shouldBePaused, FALSE, FALSE);
		m_autoPaused = shouldBePaused && !m_userPaused;
	}
}

// ============================================================================
// Pace controller - rate-matched playback
// ============================================================================
//
// See the notes on LIVE_PACE_* in LiveObserver.h for why this exists at all. In short: the source
// does not produce frames at a fixed rate, so consuming them at one is a losing race, and the gate
// answering that race with a full pause is the most visible possible way to lose it.

void LiveObserver::updatePlaybackPace(UnsignedInt nowMs, UnsignedInt gap, UnsignedInt targetLead,
	Bool streamEnded)
{
	if (TheFramePacer == nullptr)
		return;

	// Record where the live edge is now. The slope of these samples is the source's own logic
	// rate: how many frames the streamer's simulation actually produced per second of wall clock.
	const UnsignedInt edge = getLiveEdge();
	if (m_paceSampleCount == LIVE_PACE_MAX_SAMPLES)
	{
		memmove(&m_paceSamples[0], &m_paceSamples[1], sizeof(PaceSample) * (LIVE_PACE_MAX_SAMPLES - 1));
		--m_paceSampleCount;
	}
	m_paceSamples[m_paceSampleCount].ms = nowMs;
	m_paceSamples[m_paceSampleCount].edge = edge;
	++m_paceSampleCount;

	// Drop everything older than the window, but never the last two - a slope needs two points,
	// and on a stalled stream no sample would otherwise stay young enough to qualify.
	Int oldest = 0;
	while (oldest < m_paceSampleCount - 2
		&& (nowMs - m_paceSamples[oldest].ms) > (UnsignedInt)LIVE_PACE_WINDOW_MS)
	{
		++oldest;
	}
	if (oldest > 0)
	{
		memmove(&m_paceSamples[0], &m_paceSamples[oldest], sizeof(PaceSample) * (m_paceSampleCount - oldest));
		m_paceSampleCount -= oldest;
	}

	// The slope is only meaningful once it spans most of the window; before that a joining
	// observer would be paced off two samples of noise.
	const UnsignedInt spanMs = (m_paceSampleCount >= 2)
		? (m_paceSamples[m_paceSampleCount - 1].ms - m_paceSamples[0].ms) : 0;
	if (spanMs >= (UnsignedInt)(LIVE_PACE_WINDOW_MS / 2))
	{
		const UnsignedInt spanFrames = (m_paceSamples[m_paceSampleCount - 1].edge > m_paceSamples[0].edge)
			? (m_paceSamples[m_paceSampleCount - 1].edge - m_paceSamples[0].edge) : 0;
		m_sourceFps = spanFrames * 1000 / spanMs;
	}

	// Nominal in every case the controller has no business slowing: before the buffer is built,
	// once the stream has ended (the tail is all local now, so play it at full speed), and while
	// fast-forward is running, which bypasses the logic time scale anyway.
	//
	// The sampling above runs unconditionally, including while matching is switched off: the
	// measured source rate is what the gate trace reports and what the viewer is being shown, so
	// it must stay live whether or not playback is currently following it.
	const Bool fastForwarding = (TheGlobalData != nullptr && TheGlobalData->m_TiVOFastMode);
	if (!m_paceMatchingEnabled || !m_preRollComplete || streamEnded || fastForwarding || m_sourceFps == 0)
	{
		applyPaceFps(LOGICFRAMES_PER_SECOND);
		return;
	}

	// Follow the source, plus a correction that repays or spends the difference between the lead
	// we hold and the lead we want. The correction is what keeps the buffer at its target instead
	// of wherever the last hiccup left it.
	const Int error = (Int)gap - (Int)targetLead;
	Int correction = error / LIVE_PACE_CORRECTION_SECONDS;

	// Bound the correction relative to the source, not absolutely. The correction is meant to
	// nudge the pace so the lead drifts back to target; at 60 fps a raw +8 is a 13% nudge, but at
	// a source rate of 3 it is +267% and playback stops matching the match in any useful sense -
	// it becomes a slow fast-forward (observed 2026-08-15: L: 3 against P: 11). Spending a
	// backlog deliberately is what the F8 toggle is for, so the controller stays conservative.
	Int maxCorrection = (Int)m_sourceFps / 2;
	if (maxCorrection < LIVE_PACE_MIN_CORRECTION_FPS)
		maxCorrection = LIVE_PACE_MIN_CORRECTION_FPS;
	if (correction > maxCorrection)
		correction = maxCorrection;
	else if (correction < -maxCorrection)
		correction = -maxCorrection;

	Int desired = (Int)m_sourceFps + correction;

	// Never above nominal: that would close the distance to the live game, which is the one thing
	// the broadcast delay promises will not happen. Fast-forward owns catching up, and it already
	// refuses inside the delay boundary.
	if (desired > LOGICFRAMES_PER_SECOND)
		desired = LOGICFRAMES_PER_SECOND;
	if (desired < LIVE_PACE_MIN_FPS)
		desired = LIVE_PACE_MIN_FPS;

	applyPaceFps(desired);
}

void LiveObserver::applyPaceFps(Int paceFps)
{
	if (TheFramePacer == nullptr)
		return;

	// Returning to nominal is always allowed through: it is the safe state, and making it wait on
	// the hysteresis below would leave playback slowed after the reason for slowing had gone.
	const Bool toNominal = (paceFps >= LOGICFRAMES_PER_SECOND);
	if (!toNominal)
	{
		const Int delta = (paceFps > (Int)m_paceFps) ? (paceFps - (Int)m_paceFps) : ((Int)m_paceFps - paceFps);
		if (delta < LIVE_PACE_MIN_STEP_FPS)
			return;
		const UnsignedInt nowMs = timeGetTime();
		if (m_lastPaceApplyMs != 0 && (nowMs - m_lastPaceApplyMs) < (UnsignedInt)LIVE_PACE_MIN_INTERVAL_MS)
			return;
		m_lastPaceApplyMs = nowMs;
	}

	if (toNominal && m_paceFps == (UnsignedInt)LOGICFRAMES_PER_SECOND)
		return;   // already there; do not touch the pacer every tick

	// Latch what the pacer looked like before this session touched it, so ending the session can
	// hand it back unchanged. Done here rather than at construction because the pacer may not be
	// in its final state until the game is actually running.
	if (!m_pacerTouched)
	{
		m_savedLogicScaleFps = TheFramePacer->getLogicTimeScaleFps();
		m_savedLogicScaleEnabled = TheFramePacer->isLogicTimeScaleEnabled();
		m_pacerTouched = TRUE;
	}

	// The set/enable/set dance is the idiom the replay game-speed hotkey uses (CommandXlat.cpp):
	// the value is written before and after toggling, so the scale can never re-enable carrying a
	// stale one. enableLogicTimeScale is not bookkeeping - canUpdateRegularGameLogic runs logic
	// every render frame when the scale is at or above the render cap, so the accumulator that
	// actually slows playback only engages below it.
	const Int maxRenderFps = TheFramePacer->getActualFramesPerSecondLimit();
	TheFramePacer->setLogicTimeScaleFps(paceFps);
	TheFramePacer->enableLogicTimeScale(paceFps < maxRenderFps);
	TheFramePacer->setLogicTimeScaleFps(paceFps);

	m_paceFps = (UnsignedInt)paceFps;
}

void LiveObserver::restorePlaybackPace()
{
	if (!m_pacerTouched || TheFramePacer == nullptr)
		return;

	TheFramePacer->setLogicTimeScaleFps(m_savedLogicScaleFps);
	TheFramePacer->enableLogicTimeScale(m_savedLogicScaleEnabled);
	TheFramePacer->setLogicTimeScaleFps(m_savedLogicScaleFps);
	m_pacerTouched = FALSE;
	m_paceFps = LOGICFRAMES_PER_SECOND;
}

void LiveObserver::noteDesync(UnsignedInt frame)
{
	if (m_desyncFrame != 0)
		return;

	m_desyncFrame = frame;

	// Logged with the gate's state and both edges: the questions about a divergence are whether
	// we had run out of data, and whether the heartbeat let playback run past where the records
	// actually reached (recordEdge far behind curFrame with heartbeat ahead of it).
	liveObserverLog("DESYNC: observer diverged from the stream at frame %u. curFrame=%u liveEdge=%u "
		"recordEdge=%u heartbeat=%u delayFrames=%u holdPlayback=%d stalled=%d preRoll=%d\n",
		frame, TheGameLogic ? TheGameLogic->getFrame() : 0, getLiveEdge(),
		getMaxCompleteFrame(), m_liveFrameHint.load(), getDelayFrames(),
		m_holdPlayback ? 1 : 0, m_stalled ? 1 : 0, m_preRollComplete ? 1 : 0);
}

UnsignedInt LiveObserver::getTargetLeadFrames() const
{
	const UnsignedInt delayFrames = getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;

	// Settings may not exist yet on very early calls (the countdown can be asked before the
	// online services are up). Falling back to the delay alone never claims a cushion we have
	// not established.
	if (NGMP_OnlineServicesManager::GetInstance() == nullptr)
		return delayFrames;

	const Int jitterMs = NGMP_OnlineServicesManager::Settings.LiveObserver_GetJitterBufferMs();
	const UnsignedInt jitterFrames =
		(UnsignedInt)((jitterMs * LOGICFRAMES_PER_SECOND + 999) / 1000);   // round up

	return jitterFrames > delayFrames ? jitterFrames : delayFrames;
}

Bool LiveObserver::isWithinBroadcastDelay(UnsignedInt curFrame) const
{
	const UnsignedInt liveEdge = getLiveEdge();
	const UnsignedInt gap = (liveEdge > curFrame) ? (liveEdge - curFrame) : 0;
	// The gate's release bound, so the join catch-up stops fast-forwarding exactly where the gate
	// will settle. Inside it, a fast-forward would spoil the live game.
	return gap <= getTargetLeadFrames() + LIVE_GATE_RELEASE_MARGIN_FRAMES;
}

Bool LiveObserver::isPlaybackReady() const
{
	if (!m_headerReceived.load())
		return false;
	if (m_streamEnded.load())
		return true;

	// Playback may only start once the file is safe to read: the header plus at least the
	// first body record, and enough complete records to cover the whole broadcast delay.
	// The delay boundary proves the buffer is built because records arrive in order - and
	// it also guarantees the first record is present, which the Recorder's seeding read
	// depends on. A zero delay still needs that first record, hence the offset check.
	if (m_safeReadOffset.load() <= m_bodyStartOffset)
		return false;
	return getMaxCompleteFrame() >= getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND;
}

Int LiveObserver::getBroadcastDelayRemainingSeconds() const
{
	if (!m_delayWaitActive.load())
		return 0;

	const UnsignedInt nowMs = timeGetTime();
	const UnsignedInt deadline = m_delayWaitDeadlineMs.load();
	if (deadline <= nowMs)
		return 0;

	// Round up, so the countdown only reads 0 when the hold is genuinely over.
	return (Int)((deadline - nowMs + 999) / 1000);
}

Int LiveObserver::getSecondsUntilPlaybackReady() const
{
	// The GO admission hold replaces the pre-roll wait: while the ticket itself is held
	// behind the broadcast delay there is no file yet, so the countdown must come from the
	// hold deadline, not from the buffer.
	if (isWaitingForBroadcastDelay())
		return getBroadcastDelayRemainingSeconds();

	if (isPlaybackReady())
		return 0;

	// Before the ticket/ROLE arrive there is no authoritative delay yet: use the expected
	// lobby delay (pre-seeded at connect), which is what the countdown should show while
	// GO is still holding. Once connected, the relay/GO values apply.
	const UnsignedInt delaySeconds = m_connected.load() ? getEffectiveDelaySeconds() : getExpectedDelaySeconds();
	const UnsignedInt delayFrames = delaySeconds * LOGICFRAMES_PER_SECOND;
	const UnsignedInt edge = getMaxCompleteFrame();
	const UnsignedInt remaining = (delayFrames > edge) ? (delayFrames - edge) : 0;
	// Round up so the countdown only reads 0 when playback can genuinely start.
	return (Int)((remaining + LOGICFRAMES_PER_SECOND - 1) / LOGICFRAMES_PER_SECOND);
}

UnsignedInt LiveObserver::getJoinTimeoutMs() const
{
	// While GO holds the ticket behind the broadcast delay the wait can be minutes long
	// (the host's delay, up to 600 s). The timeout must cover the remaining hold plus
	// headroom, or the join pump would abandon a perfectly healthy wait.
	if (m_delayWaitActive.load())
	{
		return getBroadcastDelayRemainingSeconds() * 1000 + 60000;
	}

	// A server-held stream never needs the client's pre-roll buffer (effective delay 0),
	// so the whole wait is connection + first record - headroom only.
	if (m_serverHeld.load())
	{
		return 60000;
	}

	// Before the ticket/ROLE arrive, time out on the expected lobby delay (pre-seeded at
	// connect) rather than the ROLE default: the pre-live phase can legitimately last the
	// whole delay once the join is queued at game start.
	const UnsignedInt delaySeconds = m_connected.load() ? getDelaySeconds() : getExpectedDelaySeconds();

	// Worst case is a freshly-started game, where the stream must produce a full delay's worth of
	// records before playback may begin; a game already past the delay is playable the moment its
	// catch-up arrives. The headroom covers the connection, ticket minting and the first record,
	// and also the lobby-observer flow, where the ticket retry waits out the stream going live.
	return delaySeconds * 1000 + 60000;
}

UnsignedInt LiveObserver::getJoinDeadlineMs() const
{
	// While GO holds the ticket behind the broadcast delay the wait is the hold itself, so the
	// deadline is the absolute hold end plus headroom, refreshed forward on every 423.
	//
	// The hold deadline is sticky: once any 423 has armed it, it governs the rest of the session,
	// even after the ticket is granted and m_delayWaitActive clears. The post-admission phase
	// (relay connect + first record) must not fall back to an elapsed budget measured from
	// connect(), because a hold longer than the headroom would leave that budget already expired
	// the moment the stream became watchable.
	const UnsignedInt holdDeadline = m_delayWaitDeadlineMs.load();
	if (holdDeadline != 0)
	{
		return holdDeadline + 60000;
	}

	// Never held: join start plus the ordinary timeout budget, measured once against the
	// absolute baseline set at connect (and re-based forward at ticket grant).
	return m_joinStartedAtMs.load() + getJoinTimeoutMs();
}

LiveObserver::~LiveObserver()
{
	// The pause is global game state, so unlike every other field here it does not disappear
	// with the object - a session left holding it hands the next one an already-halted game.
	if (m_autoPaused && TheGameLogic != nullptr && TheGameLogic->isGamePaused())
		TheGameLogic->setGamePaused(FALSE, FALSE, FALSE);

	// Same reasoning for the frame pacer: a slowed logic scale left behind would follow the
	// player into their next game and look exactly like an engine bug.
	restorePlaybackPace();

	close();
}

LiveObserver* createLiveObserver()
{
	return new LiveObserver();
}

// One-shot "a live-observer game just ended" latch; see LiveObserverConsumeReturnedFromGame
// in LiveObserver.h.
static bool g_bLiveObserverReturnedFromGame = false;

Bool LiveObserverConsumeReturnedFromGame(void)
{
	const Bool returned = g_bLiveObserverReturnedFromGame ? TRUE : FALSE;
	g_bLiveObserverReturnedFromGame = false;
	return returned;
}

void liveObserverEndSession(void)
{
	liveObserverLog("liveObserverEndSession: observer=%s\n", TheLiveObserver ? "destroying" : "(none)");

	if (TheLiveObserver)
	{
		TheLiveObserver->close();
		delete TheLiveObserver;
		TheLiveObserver = nullptr;
	}

	// Only latch when a game was actually running (a stream END or an in-game quit both end the
	// session before the game clears). An aborted join ends the session from the shell, with no
	// game, and must not re-route the next visit.
	if (TheGameLogic != nullptr && TheGameLogic->isInInteractiveGame())
		g_bLiveObserverReturnedFromGame = true;

	// Closes the playback file and parks the playback cursor, but deliberately does not reset()
	// the Recorder: the score screen runs immediately after and consults isMultiplayer() to pick
	// between the multiplayer and single-player layouts, and the single-player one overrides the
	// player names with "player". Keeping LIVE_OBSERVER mode and the header's game-info slots
	// makes that report the streamer's game truthfully. A no-op when there was no session.
	if (TheRecorder)
		TheRecorder->endLivePlayback();

	// Every way a session ends returns the player to the shell, and the shell map is the shell's
	// backdrop. Restored here rather than in stopPlayback(), so that the clearGameData() path
	// (the in-game exit button) does not leave a mapless shell. A session that is still starting
	// never reaches this function - the guard in liveObserverOnGameCleared() returns first.
	if (TheWritableGlobalData)
		TheWritableGlobalData->m_shellMapOn = TRUE;
}

void liveObserverOnGameCleared(void)
{
	if (TheLiveObserver == nullptr)
		return;

	// A session that has not started playing is still being set up, and the thing clearing game
	// data right now is that very setup: playbackFile() unloads the shell map before it reads the
	// header. Ending the session here would destroy the observer that just finished connecting.
	if (!TheLiveObserver->hasPlaybackStarted())
	{
		liveObserverLog("liveObserverOnGameCleared: session is still starting, keeping it\n");
		return;
	}

	liveObserverEndSession();
}

// ============================================================================
// Network setup
// ============================================================================

bool LiveObserver::fetchWatchTicket(AsciiString& outConnectUrl)
{
	// GO owns admission to a livestream: it checks the session, confirms the lobby really is
	// being streamed, and asks the relay for a single-use ticket on the player's behalf. What
	// comes back is a complete connect URL, so nothing here needs to know the relay's address.
	AsciiString url;
	url.format("%s/observe/%s", liveServicesEndpoint("Livestreams").str(), m_gameId.str());

	AsciiString body;
	Int statusCode = 0;

	// The POST body carries the lobby password when one was supplied; unpassworded streams
	// keep the empty body. Built once: nlohmann's dump() escapes it, so a quote in a
	// password cannot break the JSON.
	std::string postBody;
	if (!m_password.empty())
	{
		nlohmann::json pwPayload;
		pwPayload["password"] = m_password;
		postBody = pwPayload.dump();
	}

	// The observer can arrive just as the stream goes live, and GO answers 404 for that window
	// (the relay may not hold the header yet, or GO may not have processed its liveness report),
	// so retry on a short cadence for a bounded time instead of aborting. The loop aborts as soon
	// as the session is cancelled, so LEAVE does not hang behind it.
	//
	// 401 must NOT retry: the stream is password-protected and the supplied password was missing
	// or wrong. Latched and returned immediately, so the join pump can re-prompt.
	//
	// 423 is the broadcast-delay admission hold - GO will not mint the ticket until the stream
	// has been live for the host's delay. See the 423 branch below.
	const int64_t kTicketRetryWindowMs = 40000;
	const auto retryStart = std::chrono::steady_clock::now();
	auto retryDeadline = retryStart + std::chrono::milliseconds(kTicketRetryWindowMs);
	for (;;)
	{
		if (!m_shouldRun.load())
		{
			liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s cancelled\n",
				m_gameId.str());
			return false;
		}

		if (!liveServicesRequest(url, TRUE, postBody.c_str(), body, statusCode))
		{
			liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s failed (request not sent)\n",
				m_gameId.str());
			return false;
		}

		if (statusCode == 200)
		{
			// GO owns the broadcast delay: the ticket was only minted once the stream
			// outlived it, so the relay stream is already delayed and this client must not
			// hold playback itself. Absent field (older GO) keeps the client-side hold.
			try
			{
				nlohmann::json ticketResponse = nlohmann::json::parse(body.str());
				if (ticketResponse.is_object() && ticketResponse.contains("server_held")
					&& ticketResponse["server_held"].is_boolean())
				{
					m_serverHeld.store(ticketResponse["server_held"].get<bool>() ? TRUE : FALSE);
				}
			}
			catch (const nlohmann::json::exception&) { }
			m_delayWaitActive.store(FALSE);

			// Admission granted: re-base the join clock, because connect() ran before the hold
			// and the hold can outlive the elapsed budget it started - without this the join
			// pump's deadline would already be in the past the moment the hold ended.
			m_joinStartedAtMs.store(timeGetTime());
			break;
		}

		if (statusCode == 401)
		{
			// Wrong or missing password for a password-protected stream.
			m_passwordRejected.store(TRUE);
			liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s password rejected (status=%d)\n",
				m_gameId.str(), statusCode);
			return false;
		}

		if (statusCode == 423)
		{
			Int holdRemainingSeconds = 0;
			try
			{
				nlohmann::json holdResponse = nlohmann::json::parse(body.str());
				if (holdResponse.is_object() && holdResponse.contains("delay_remaining_seconds")
					&& holdResponse["delay_remaining_seconds"].is_number_integer())
				{
					holdRemainingSeconds = holdResponse["delay_remaining_seconds"].get<Int>();
				}
			}
			catch (const nlohmann::json::exception&) { }

			if (holdRemainingSeconds > 0)
			{
				m_delayWaitDeadlineMs.store(timeGetTime() + (UnsignedInt)holdRemainingSeconds * 1000);
				m_delayWaitActive.store(TRUE);
				retryDeadline = std::chrono::steady_clock::now()
					+ std::chrono::milliseconds((int64_t)holdRemainingSeconds * 1000 + 30000);

				// Sleep to the end of the hold plus a small margin, rather than polling. Capped
				// at 30s so a hold that ends early (stream gone, viewer granted priority
				// mid-wait) is still picked up within half a minute: each wake re-requests, gets
				// the fresh remaining hold, and re-arms.
				Int64 holdSleepMs = (Int64)holdRemainingSeconds * 1000 + 500;
				if (holdSleepMs > 30000)
				{
					holdSleepMs = 30000;
				}
				liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s held behind the "
					"broadcast delay (%ds remaining), retrying in %lldms\n",
					m_gameId.str(), holdRemainingSeconds, holdSleepMs);
				std::this_thread::sleep_for(std::chrono::milliseconds(holdSleepMs));
				continue;
			}
		}

		if (std::chrono::steady_clock::now() > retryDeadline)
		{
			// 404 is the ordinary "that stream is over" answer: the game was listed a moment
			// ago, but the relay has closed it since. Anything else is a real failure.
			liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s gave up (status=%d) %s\n",
				m_gameId.str(), statusCode, body.str());
			return false;
		}

		liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s not watchable yet (status=%d), retrying\n",
			m_gameId.str(), statusCode);
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}

	bool success = false;
	try
	{
		nlohmann::json response = nlohmann::json::parse(body.str());
		if (response.is_object() && response.contains("url") && response["url"].is_string())
		{
			const std::string ticketUrl = response["url"].get<std::string>();
			if (!ticketUrl.empty())
			{
				outConnectUrl = ticketUrl.c_str();
				success = true;
			}
		}
	}
	catch (const nlohmann::json::exception&)
	{
	}

	liveObserverLog("LiveObserver::fetchWatchTicket: lobby=%s %s (status=%d)\n",
		m_gameId.str(), success ? "succeeded" : "failed", statusCode);
	return success;
}

void LiveObserver::connect(const AsciiString& lobbyId, const std::string& password,
	Int expectedDelaySeconds)
{
	m_shouldRun.store(true);
	m_password = password;
	m_passwordRejected.store(FALSE);
	m_expectedDelaySeconds = expectedDelaySeconds;
	m_joinStartedAtMs = timeGetTime();

	// The lobby id is all this needs: admission runs through GO, which mints a single-use ticket
	// for this player and answers with the complete relay URL (see fetchWatchTicket).
	m_gameId = lobbyId.isEmpty() ? AsciiString("unknown") : lobbyId;
	if (lobbyId.isEmpty())
	{
		// No id means no session to ask GO about. Keep the filename unique to this instance
		// anyway: a shared "_live.rep" is the worst possible name to collide on.
		m_liveFilename.format("unknown_Instance%.2u_live.rep",
			rts::ClientInstance::getInstanceId());
		liveObserverLog("LiveObserver::connect: no lobby id supplied\n");
	}
	else
	{
		m_liveFilename.format("%s_live.rep", m_gameId.str());
	}

	liveObserverLog("LiveObserver::connect game=%s (file=%s)\n",
		m_gameId.str(), m_liveFilename.str());

	m_networkThread = std::thread(&LiveObserver::networkThreadFunc, this);
}

void LiveObserver::close()
{
	m_shouldRun.store(false);

	if (m_networkThread.joinable())
		m_networkThread.join();

	if (m_liveFile)
	{
		m_liveFile->close();
		m_liveFile = nullptr;
	}

	m_connected.store(false);
	m_headerReceived.store(false);
	m_streamEnded.store(false);
}

// ============================================================================
// Live file management
// ============================================================================

bool LiveObserver::openLiveFile()
{
	AsciiString filepath = RecorderClass::getReplayDir();
	filepath.concat(m_liveFilename);

	m_liveFilePath = filepath;

	// Delete before opening. These files are named by game id and never cleaned up, so a rejoin
	// or a session after a crash must not inherit the previous session's bytes. A failed delete
	// usually means another process still holds the file, since streamer and observer are the
	// same exe - two writers at absolute offsets in one file corrupt each other, so refuse
	// rather than proceed.
	if (remove(filepath.str()) == 0)
	{
		liveObserverLog("LiveObserver::openLiveFile removed leftover %s (previous session did not clean up)\n",
			filepath.str());
	}
	else if (errno != ENOENT)
	{
		liveObserverLog("LiveObserver::openLiveFile could NOT remove %s (errno=%d) - refusing to reuse it\n",
			filepath.str(), errno);
		return false;
	}

	m_liveFile = TheFileSystem->openFile(filepath.str(),
		File::WRITE | File::CREATE | File::TRUNCATE | File::BINARY);
	if (!m_liveFile)
	{
		liveObserverLog("LiveObserver::openLiveFile FAILED for %s\n", filepath.str());
		return false;
	}

	liveObserverLog("LiveObserver::openLiveFile opened %s\n", filepath.str());
	return true;
}

// ============================================================================
// Chat helpers
//
// Mirrored in LiveStreamer.cpp as wideToUtf8/utf8ToWide/appendU32LE - keep both copies in sync.
// ============================================================================

static std::string chatWideToUtf8(const UnicodeString& text)
{
	const wchar_t* src = text.str();
	const int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)       // nothing but the terminator, or a failure
		return std::string();
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, src, -1, &out[0], len, nullptr, nullptr);
	return out;
}

static UnicodeString chatUtf8ToWide(const std::string& utf8)
{
	const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
	if (len <= 0)
		return UnicodeString::TheEmptyString;
	std::wstring tmp(static_cast<size_t>(len), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &tmp[0], len);
	return UnicodeString(tmp.c_str());
}

static void chatAppendU32LE(std::vector<char>& out, unsigned int value)
{
	out.push_back((char)(value & 0xFF));
	out.push_back((char)((value >> 8) & 0xFF));
	out.push_back((char)((value >> 16) & 0xFF));
	out.push_back((char)((value >> 24) & 0xFF));
}

namespace
{
	/// The signed-in user's display name, for spectator chat sends. Cached: the auth
	/// interface outlives every live-observer session.
	UnicodeString observerDisplayName()
	{
		static UnicodeString s_cached;
		if (s_cached.isEmpty())
		{
			NGMP_OnlineServices_AuthInterface* auth =
				NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
			if (auth)
				s_cached.set(auth->GetDisplayNameW().c_str());
		}
		return s_cached;
	}
}

/// Fixed spectator-chat display color (light blue) - matches the streamer-side style.
static const unsigned int SPECTATOR_CHAT_COLOR = 0x72ADF2u;

void LiveObserver::displayChat(const ChatEntry& entry)
{
	RGBColor c;
	c.setFromInt((Int)entry.colorArgb);
	TheInGameUI->messageColor(true, &c, UnicodeString(L"%ls"), entry.text.str());
}

Bool LiveObserver::isSpectatorGateOpen(UnsignedInt curFrame) const
{
	// The 5-second spoiler rule: live spectator chat is shown only while the observer is within
	// ~5s of the broadcast-delay boundary, i.e. effectively watching live. Further behind
	// (pre-roll, stalls, long pauses) it would spoil the game it describes.
	const UnsignedInt liveEdge = m_maxCompleteFrame.load();
	if (liveEdge <= curFrame)
		return TRUE;    // at or past the live edge - as live as it gets
	const UnsignedInt gap = liveEdge - curFrame;
	return gap <= getEffectiveDelaySeconds() * LOGICFRAMES_PER_SECOND + 5 * LOGICFRAMES_PER_SECOND;
}

void LiveObserver::pollChatMessages(UnsignedInt curFrame)
{
	if (!TheInGameUI)
		return;

	std::deque<ChatEntry> batch;
	{
		std::lock_guard<std::mutex> lock(m_chatMutex);
		if (m_chatQueue.empty())
			return;
		batch.swap(m_chatQueue);
	}

	const Bool interactive = TheGameLogic && TheGameLogic->isInInteractiveGame();
	const Bool gateOpen = isSpectatorGateOpen(curFrame);
	std::deque<ChatEntry> holdback;
	for (auto& entry : batch)
	{
		if (entry.disaster)
		{
			// Stream-failure notice: the stream is gone, so there is nothing left to spoil.
			displayChat(entry);
		}
		else if (entry.spectator)
		{
			// Live meta-chat, shown per the F7 mode: auto = inside the spoiler window only,
			// forced ON = always, OFF = never. Outside the window in auto mode it is dropped,
			// never held - if you were not watching live, you missed it.
			Bool showSpectator = FALSE;
			if (m_spectatorChatMode == SPECTATOR_CHAT_FORCED_ON)
				showSpectator = interactive;
			else if (m_spectatorChatMode == SPECTATOR_CHAT_AUTO)
				showSpectator = interactive && gateOpen;
			if (showSpectator)
				displayChat(entry);
		}
		else if (interactive && entry.frame <= curFrame)
		{
			// Player chat is frame-gated: released exactly when the observed game reaches the
			// moment the streamer sent it, so it sits behind the same broadcast delay.
			displayChat(entry);
		}
		else
		{
			holdback.push_back(entry);
		}
	}
	if (!holdback.empty())
	{
		std::lock_guard<std::mutex> lock(m_chatMutex);
		// Reinsert at the FRONT: these are the oldest entries and must drain in order.
		m_chatQueue.insert(m_chatQueue.begin(), holdback.begin(), holdback.end());
	}
}

void LiveObserver::sendSpectatorChat(const UnicodeString& text)
{
	if (!m_connected.load() || !m_shouldRun.load())
	{
		liveObserverLog("LiveObserver::sendSpectatorChat DROPPED (not connected)\n");
		return;
	}

	// [nameLen u32 LE][UTF-8 name][textLen u32 LE][UTF-8 text].
	std::string utf8Name = chatWideToUtf8(observerDisplayName());
	std::string utf8Text = chatWideToUtf8(text);
	std::vector<char> payload;
	payload.reserve(8 + utf8Name.size() + utf8Text.size());
	chatAppendU32LE(payload, (unsigned int)utf8Name.size());
	payload.insert(payload.end(), utf8Name.begin(), utf8Name.end());
	chatAppendU32LE(payload, (unsigned int)utf8Text.size());
	payload.insert(payload.end(), utf8Text.begin(), utf8Text.end());

	std::lock_guard<std::mutex> lock(m_outboundChatMutex);
	if (m_outboundChatQueue.size() < 100)
		m_outboundChatQueue.push_back(payload);
}

// ============================================================================
// Frame handler
// ============================================================================

void LiveObserver::handleFrame(unsigned char type, const char* payload, size_t len)
{
	switch (type)
	{
	case 1: // LIVE_MSG_HEADER
	{
		if (!openLiveFile())
			return;

		if (len > 0)
			m_liveFile->write(payload, len);
		m_liveFile->flush();

		// Do NOT reopen here: the Recorder needs the file to itself to read the header during
		// playbackFile(). The PATCH/BODY handlers reopen lazily.
		m_liveFile->close();
		m_liveFile = nullptr;

		// Body records start immediately after the header, so that is where the parse cursor
		// begins. Reset here rather than only in the constructor, so a second live-observer
		// session in the same process cannot inherit a stale watermark.
		resetParseCursor((Int)len);

		m_headerReceived.store(true);
		liveObserverLog("LiveObserver: HEADER received (%zu bytes), ready for playback\n", len);
		break;
	}

	case 2: // LIVE_MSG_PATCH
	{
		if (len < 8)
			return;

		// Lazy-open for read/write; the file exists and must not be truncated.
		if (!m_liveFile)
		{
			m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
		}
		if (!m_liveFile)
			return;

		const unsigned char* p = (const unsigned char*)payload;
		Int offset = (Int)p[0] | ((Int)p[1] << 8) | ((Int)p[2] << 16) | ((Int)p[3] << 24);
		Int dataLen = (Int)p[4] | ((Int)p[5] << 8) | ((Int)p[6] << 16) | ((Int)p[7] << 24);

		if (dataLen <= 0 || (size_t)(8 + dataLen) > len)
			return;

		// Restore the append position afterwards: BODY writes seek absolutely, but the file
		// handle is shared with them.
		Int fileSize = (Int)m_liveFile->size();
		Int seekRes = m_liveFile->seek(offset, File::seekMode::START);
		if (seekRes == offset)
		{
			m_liveFile->write(payload + 8, dataLen);
			m_liveFile->seek(fileSize, File::seekMode::START);
		}
		break;
	}

	case 3: // LIVE_MSG_BODY
	{
		// BODY payload: [8B offset uint64 LE][data]
		if (len < 8)
		{
			liveObserverLog("LiveObserver: BODY frame too short (len=%d)\n", (int)len);
			return;
		}

		const unsigned char* p = (const unsigned char*)payload;
		Int offset = (Int)(p[0] | ((unsigned long long)p[1] << 8)
			| ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24)
			| ((unsigned long long)p[4] << 32) | ((unsigned long long)p[5] << 40)
			| ((unsigned long long)p[6] << 48) | ((unsigned long long)p[7] << 56));
		size_t dataLen = len - 8;
		if (dataLen == 0)
		{
			liveObserverLog("LiveObserver: BODY frame with zero dataLen at offset=%d\n", offset);
			return;
		}

		// Lazy-open for read/write; the file exists from the HEADER handler and must not be
		// truncated.
		if (!m_liveFile)
		{
			m_liveFile = TheFileSystem->openFile(m_liveFilePath.str(), File::READWRITE | File::BINARY);
		}
		if (!m_liveFile)
		{
			liveObserverLog("LiveObserver: BODY openFile FAILED for %s\n", m_liveFilePath.str());
			return;
		}

		m_liveFile->seek(offset, File::seekMode::START);
		m_liveFile->write(payload + 8, (Int)dataLen);
		m_liveFile->flush();

		// Scan the bytes just committed, so the game thread's live edge and safe-read limit are
		// up to date the moment the data is readable.
		advanceParseCursor(offset, (const unsigned char*)(payload + 8), dataLen);
		break;
	}

	case 5: // LIVE_MSG_ROLE - session config, sent by the relay ahead of the HEADER
	{
		// The broadcast delay must be applied before playback starts, because the pre-roll buffer
		// latches against it and there is no un-latching once a session is running - which is why
		// the relay sends this frame before the HEADER that triggers the game start.
		std::string json(payload, len);
		liveObserverLog("LiveObserver: ROLE received: %s\n", json.c_str());

		const char* delayStart = strstr(json.c_str(), "\"delay_seconds\":");
		if (delayStart)
		{
			delayStart += 16; // skip "delay_seconds":
			Int delaySeconds = (Int)strtol(delayStart, nullptr, 10);
			if (delaySeconds >= 0 && delaySeconds <= LIVE_DELAY_SECONDS_MAX)
			{
				m_delaySeconds.store((UnsignedInt)delaySeconds);
				liveObserverLog("LiveObserver: broadcast delay set to %d seconds\n", delaySeconds);
			}
			else
			{
				liveObserverLog("LiveObserver: ignoring out-of-range delay_seconds=%d, keeping %u\n",
					delaySeconds, m_delaySeconds.load());
			}
		}
		// No delay_seconds (older relay) simply leaves the built-in default in place.
		break;
	}

	case 9: // LIVE_MSG_TICK - the streamer's current logic frame
	{
		if (len < 4)
			return;

		const unsigned char* p = (const unsigned char*)payload;
		const UnsignedInt frame = (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8)
			| ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24);

		// The tick only proves "every record up to this frame has arrived" while the byte stream
		// behind it is whole. A gap or a corrupt record means records below this frame may be
		// missing, and acting on the tick would let playback run past them - which does not
		// stall, it silently executes those commands at the wrong frame later and diverges the
		// simulation for good. Fail closed and fall back to the record-derived edge; the cursor
		// repairs itself when the missing bytes are backfilled.
		//
		// Same thread as advanceParseCursor, so reading its state needs no synchronisation.
		if (m_parseCorrupt || m_parseGapPending)
			break;

		// Monotonic, mirroring the relay: a late or duplicated tick must not walk the edge
		// backwards under a game thread that has already simulated past it.
		if (frame > m_liveFrameHint.load())
			m_liveFrameHint.store(frame);
		break;
	}

	case 10: // LIVE_MSG_STATS - the streamer's logic frame rate and ping
	{
		if (len < 8)
			return;

		const unsigned char* p = (const unsigned char*)payload;
		const UnsignedInt logicFps = (UnsignedInt)p[0] | ((UnsignedInt)p[1] << 8)
			| ((UnsignedInt)p[2] << 16) | ((UnsignedInt)p[3] << 24);
		const UnsignedInt pingMs = (UnsignedInt)p[4] | ((UnsignedInt)p[5] << 8)
			| ((UnsignedInt)p[6] << 16) | ((UnsignedInt)p[7] << 24);

		// Display only - nothing simulates off these, so unlike the tick there is no ordering
		// requirement against the body. Sent on change, so the last value received stands until
		// the next one arrives; the streamer's heartbeat bounds how stale that can get.
		m_srcLogicFps.store(logicFps);
		m_srcPingMs.store(pingMs);
		break;
	}

	case 4: // LIVE_MSG_END
	{
		liveObserverLog("LiveObserver: END received\n");
		m_streamEnded.store(true);

		if (m_liveFile)
		{
			m_liveFile->flush();
			m_liveFile->close();
			m_liveFile = nullptr;
		}
		break;
	}

	case 6: // LIVE_MSG_ERROR - the relay killed the session (a disaster, not a game end).
	// Sent instead of a plain END when the stream was reaped or the relay is shutting down.
	// The player gets an in-game notice; a normal stream END stays silent.
	{
		AsciiString errMsg;
		if (len > 0)
			errMsg.set(payload, len);
		liveObserverLog("LiveObserver: ERROR from relay: %s\n", errMsg.str());

		// Payload is JSON {"reason":"...","msg":"..."} when the relay sends one; older
		// relays send a bare text. Parse the reason for the player-facing line.
		UnicodeString reasonText(L"stream ended unexpectedly");
		if (len > 0)
		{
			const std::string json(payload, len);
			static const char REASON_KEY[] = "\"reason\":\"";
			const char* reasonStart = strstr(json.c_str(), REASON_KEY);
			if (reasonStart)
			{
				reasonStart += strlen(REASON_KEY);
				const char* reasonEnd = strchr(reasonStart, '"');
				if (reasonEnd)
					reasonText = chatUtf8ToWide(std::string(reasonStart, reasonEnd - reasonStart));
			}
		}

		{
			std::lock_guard<std::mutex> lock(m_chatMutex);
			if (m_chatQueue.size() < 1000)
			{
				ChatEntry entry;
				entry.frame = 0;
				entry.colorArgb = 0xFF7A5Au;    // red: a failure, not a chat line
				entry.spectator = FALSE;
				entry.disaster = TRUE;
				entry.text.format(L"Stream lost - the relay ended the session (%ls)",
					reasonText.str());
				m_chatQueue.push_back(entry);
			}
		}
		m_streamEnded.store(true);
		break;
	}

	case 7: // LIVE_MSG_CHAT - player chat, frame-stamped by the streamer
	{
		// [frame u32 LE][textLen u32 LE][UTF-8 text][color u32 LE]
		if (len < 12)
			break;
		const unsigned char* p = (const unsigned char*)payload;
		unsigned int frame = (unsigned int)p[0]
			| ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
		unsigned int textLen = (unsigned int)p[4]
			| ((unsigned int)p[5] << 8) | ((unsigned int)p[6] << 16) | ((unsigned int)p[7] << 24);
		if ((uint64_t)len < 12ull + textLen)
			break;
		unsigned int colorArgb = (unsigned int)p[8 + textLen]
			| ((unsigned int)p[9 + textLen] << 8)
			| ((unsigned int)p[10 + textLen] << 16)
			| ((unsigned int)p[11 + textLen] << 24);

		ChatEntry entry;
		entry.frame = frame;
		entry.colorArgb = colorArgb;
		entry.spectator = FALSE;
		entry.text = chatUtf8ToWide(std::string(payload + 8, textLen));
		{
			std::lock_guard<std::mutex> lock(m_chatMutex);
			if (m_chatQueue.size() < 1000)
				m_chatQueue.push_back(entry);
		}
		break;
	}

	case 8: // LIVE_MSG_SPECTATOR_CHAT - live spectator meta-chat
	{
		// [nameLen u32 LE][UTF-8 name][textLen u32 LE][UTF-8 text]
		if (len < 8)
			break;
		const unsigned char* p = (const unsigned char*)payload;
		unsigned int nameLen = (unsigned int)p[0]
			| ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
		if ((uint64_t)len < 8ull + nameLen)
			break;
		const unsigned char* t = p + 4 + nameLen;
		unsigned int textLen = (unsigned int)t[0]
			| ((unsigned int)t[1] << 8) | ((unsigned int)t[2] << 16) | ((unsigned int)t[3] << 24);
		if ((uint64_t)len < 8ull + nameLen + textLen)
			break;

		ChatEntry entry;
		entry.frame = 0;
		entry.colorArgb = SPECTATOR_CHAT_COLOR;
		entry.spectator = TRUE;
		UnicodeString name = chatUtf8ToWide(std::string(payload + 4, nameLen));
		UnicodeString text = chatUtf8ToWide(std::string((const char*)(t + 4), textLen));
		entry.text.format(L"[%ls] %ls", name.str(), text.str());
		{
			std::lock_guard<std::mutex> lock(m_chatMutex);
			if (m_chatQueue.size() < 1000)
				m_chatQueue.push_back(entry);
		}
		break;
	}

	default:
		break;
	}
}

// ============================================================================
// WebSocket I/O
// ============================================================================

bool LiveObserver::wsSendBinary(const unsigned char* data, size_t len)
{
	if (!m_curlEasy)
		return false;

	size_t sent = 0;
	CURLcode rc = curl_ws_send(m_curlEasy, data, len, &sent, 0, CURLWS_BINARY);
	return (rc == CURLE_OK && sent == len);
}

LiveObserver::WsRecvResult LiveObserver::wsRecv(std::vector<char>& outBuffer)
{
	if (!m_curlEasy)
		return WS_RECV_NONE;

	outBuffer.clear();
	outBuffer.resize(65536);

	const struct curl_ws_frame* meta = nullptr;
	size_t nread = 0;
	CURLcode rc = curl_ws_recv(m_curlEasy, outBuffer.data(), outBuffer.size(), &nread, &meta);
	if (rc == CURLE_AGAIN)
	{
		outBuffer.clear();
		return WS_RECV_NONE;
	}
	if (rc != CURLE_OK)
	{
		// The connection itself is gone (relay crash, socket closed without an ERROR
		// frame). Wind the session down like a connection loss instead of spinning on a
		// dead socket until the watchdog catches up.
		liveObserverLog("LiveObserver::wsRecv error: %d - connection lost, winding down the session\n", (int)rc);
		m_connected.store(false);
		m_streamEnded.store(true);
		outBuffer.clear();
		return WS_RECV_NONE;
	}

	// Any frame - stream bytes or the relay's protocol-level keepalive pings - proves the relay
	// is alive; the watchdog below keys off this timestamp.
	m_lastFrameReceivedMs.store(timeGetTime());

	// Only binary payloads belong in the reassembly buffer: a PING/PONG/TEXT/CLOSE payload
	// appended into the byte stream misparses everything after it.
	if (meta != nullptr && (meta->flags & CURLWS_BINARY) == 0)
	{
		outBuffer.clear();
		return WS_RECV_SKIPPED;
	}

	outBuffer.resize(nread);
	return (nread > 0) ? WS_RECV_DATA : WS_RECV_SKIPPED;
}

bool LiveObserver::connectToRelay()
{
	if (m_curlEasy)
	{
		curl_easy_cleanup((CURL*)m_curlEasy);
		m_curlEasy = nullptr;
	}
	if (m_curlMulti)
	{
		curl_multi_cleanup((CURLM*)m_curlMulti);
		m_curlMulti = nullptr;
	}

	// No ticket, no connection. There is deliberately no fallback: the relay refuses any /watch
	// without a valid ?ticket=, so connecting anyway would turn "GO would not admit you" into a
	// connect that opens and is then rejected, which reads as a relay fault.
	AsciiString connectUrl;
	if (!fetchWatchTicket(connectUrl))
	{
		liveObserverLog("LiveObserver::connectToRelay game=%s aborted (no watch ticket)\n",
			m_gameId.str());
		return false;
	}

	CURL* easy = curl_easy_init();
	if (!easy)
	{
		liveObserverLog("LiveObserver::connectToRelay curl_easy_init failed\n");
		return false;
	}

	curl_easy_setopt(easy, CURLOPT_URL, connectUrl.str());
	curl_easy_setopt(easy, CURLOPT_CONNECT_ONLY, 2L);

	// wss:// needs a CA bundle. This libcurl is built against OpenSSL, which unlike Schannel does
	// not consult the Windows certificate store, so without trust anchors it rejects every
	// certificate as CURLE_PEER_FAILED_VERIFICATION (60). Same approach as HTTPRequest.cpp.
	{
		std::ifstream certFile("cacert.pem");
		if (certFile.good())
		{
			certFile.close();
			curl_easy_setopt(easy, CURLOPT_CAINFO, "cacert.pem");
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
		}
		else
		{
			liveObserverLog("LiveObserver: cacert.pem not found - TLS certificate verification DISABLED\n");
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
		}
	}

	CURLM* multi = curl_multi_init();
	if (!multi)
	{
		curl_easy_cleanup(easy);
		liveObserverLog("LiveObserver::connectToRelay curl_multi_init failed\n");
		return false;
	}

	curl_multi_add_handle(multi, easy);

	int stillRunning = 0;
	CURLMcode mc = curl_multi_perform(multi, &stillRunning);
	while (mc == CURLM_OK && stillRunning > 0)
	{
		mc = curl_multi_poll(multi, NULL, 0, 1000, NULL);
		if (mc == CURLM_OK)
			mc = curl_multi_perform(multi, &stillRunning);
	}

	if (mc != CURLM_OK)
	{
		liveObserverLog("LiveObserver::connectToRelay failed: %d\n", (int)mc);
		curl_multi_remove_handle(multi, easy);
		curl_multi_cleanup(multi);
		curl_easy_cleanup(easy);
		return false;
	}

	int infoRunning = 0;
	CURLMsg* infoMsg = curl_multi_info_read(multi, &infoRunning);
	if (!infoMsg || infoMsg->data.result != CURLE_OK)
	{
		int result = infoMsg ? (int)infoMsg->data.result : -1;
		liveObserverLog("LiveObserver::connectToRelay: handshake failed (result=%d)\n", result);
		curl_multi_remove_handle(multi, easy);
		curl_multi_cleanup(multi);
		curl_easy_cleanup(easy);
		return false;
	}

	m_curlEasy = easy;
	m_curlMulti = multi;
	m_connected.store(true);
	liveObserverLog("LiveObserver::connectToRelay connected (game=%s)\n", m_gameId.str());
	return true;
}

// ============================================================================
// Background network thread
// ============================================================================

void LiveObserver::networkThreadFunc()
{
	liveObserverLog("LiveObserver::networkThreadFunc started\n");

	if (!connectToRelay())
	{
		liveObserverLog("LiveObserver::networkThreadFunc connectToRelay failed\n");
		m_shouldRun.store(false);
		return;
	}

	m_connected.store(true);

	// Persistent buffer: multiple frames may arrive in one wsRecv call, or a frame may be split
	// across several.
	std::vector<char> buf;
	size_t totalBytesReceived = 0;
	size_t totalFramesProcessed = 0;

	// Set when a drain pass stopped on its own cap rather than on an empty buffer, so the next
	// pass polls with a zero timeout instead of sleeping on a socket whose data is already in
	// curl's hands.
	Bool moreBuffered = FALSE;

	while (m_shouldRun.load() && m_connected.load())
	{
		{
			// curl_multi_poll's out-param is numfds, not "transfers still running", and can be 0
			// on a timeout while curl_multi_perform() still has work. Perform unconditionally, or
			// arrived bytes sit unprocessed while curl_ws_recv() keeps returning CURLE_AGAIN.
			int numfds = 0;
			CURLMcode mpoll = curl_multi_poll(m_curlMulti, NULL, 0, moreBuffered ? 0 : 50, &numfds);
			if (mpoll != CURLM_OK)
			{
				liveObserverLog("LiveObserver: curl_multi_poll failed (%d), connection lost\n", (int)mpoll);
				m_connected.store(false);
				break;
			}
			int runningHandles = 0;
			curl_multi_perform((CURLM*)m_curlMulti, &runningHandles);
		}

		// Drain everything curl already holds, not one message per pass. curl_ws_recv yields a
		// single message per call, while curl_multi_poll waits on the *socket* - so once curl has
		// messages buffered the socket falls quiet, the poll burns its full timeout, and the
		// receive rate collapses to one message per 50 ms (20/s, measured). The relay sends one
		// BODY frame per streamer append, ~50/s on an active match, so a recv-once loop falls
		// behind by ~30 frames a second for the whole match: playback starves, the buffering gate
		// reads it as "caught up" and pauses, and the backlog is never recoverable. Seen as an
		// observer 96 s behind the relay at stream end (2026-08-15).
		{
			// Bounded so the outbound chat drain and the relay watchdog below still get their
			// turn on a permanently busy socket; the zero-timeout poll above means hitting the
			// cap costs a loop pass, not a stall.
			const int MAX_MESSAGES_PER_PASS = 512;
			int drained = 0;
			std::vector<char> tmp;
			for (;;)
			{
				if (drained >= MAX_MESSAGES_PER_PASS)
				{
					moreBuffered = TRUE;
					break;
				}
				const WsRecvResult rr = wsRecv(tmp);
				if (rr == WS_RECV_NONE)
				{
					moreBuffered = FALSE;
					break;
				}
				++drained;
				if (rr == WS_RECV_DATA && !tmp.empty())
				{
					totalBytesReceived += tmp.size();
					buf.insert(buf.end(), tmp.begin(), tmp.end());
				}
			}
		}

		// Process as many complete frames as possible from the buffer
		while (buf.size() >= 5)
		{
			// char is signed on MSVC, so a length byte >= 0x80 cast straight to unsigned int
			// sign-extends (0x87 -> 0xFFFFFF87) and corrupts roughly half of all lengths. Every
			// byte must zero-extend through unsigned char first.
			unsigned char msgType = (unsigned char)buf[0];
			unsigned int msgLen = (unsigned int)(unsigned char)buf[1]
				| ((unsigned int)(unsigned char)buf[2] << 8)
				| ((unsigned int)(unsigned char)buf[3] << 16)
				| ((unsigned int)(unsigned char)buf[4] << 24);

			if ((uint64_t)buf.size() < 5ull + msgLen)
				break; // partial frame - wait for more data

			const char* payload = (msgLen > 0) ? buf.data() + 5 : nullptr;
			handleFrame(msgType, payload, msgLen);
			++totalFramesProcessed;

			// Remove the processed frame from the buffer (the length was validated against
			// buf.size() in 64-bit above, so this cannot wrap)
			buf.erase(buf.begin(), buf.begin() + 5 + (size_t)msgLen);
		}

		// Send any queued spectator chat. Drained here because the curl handle is
		// network-thread-owned; chat is sparse, so one frame per loop pass is plenty.
		{
			std::vector<char> outbound;
			{
				std::lock_guard<std::mutex> lock(m_outboundChatMutex);
				if (!m_outboundChatQueue.empty())
				{
					outbound = m_outboundChatQueue.front();
					m_outboundChatQueue.pop_front();
				}
			}
			if (!outbound.empty())
			{
				// The relay expects the binary envelope [1B type][4B length][payload], the same
				// framing the streamer's sendBinaryFrame applies; a bare payload is read as
				// type/length and silently dropped.
				std::vector<char> framed;
				framed.reserve(5 + outbound.size());
				framed.push_back((char)8);   // LIVE_MSG_SPECTATOR_CHAT (see LiveStreamer.h)
				chatAppendU32LE(framed, (unsigned int)outbound.size());
				framed.insert(framed.end(), outbound.begin(), outbound.end());
				if (!wsSendBinary((const unsigned char*)framed.data(), framed.size()))
					liveObserverLog("LiveObserver: FAILED to send spectator chat (%zu bytes)\n", framed.size());
			}
		}

		// Relay liveness watchdog: the relay's websocket library pings us every ~20 s, so a long
		// silence can only mean the relay - or the connection to it - is gone. End the session
		// the same way a stream END does, instead of freezing on the last frame forever. Gated on
		// the header, because before it (the join, the broadcast-delay hold) the relay may
		// legitimately be silent for longer than the threshold.
		if (m_headerReceived.load()
			&& (timeGetTime() - m_lastFrameReceivedMs.load()) > (UnsignedInt)LIVE_RELAY_WATCHDOG_MS)
		{
			liveObserverLog("LiveObserver: no data from relay for %ums - connection lost, winding down the session\n",
				(unsigned)(timeGetTime() - m_lastFrameReceivedMs.load()));
			m_streamEnded.store(true);
			m_connected.store(false);
			break;
		}
	}

	// Cleanup
	if (m_liveFile)
	{
		m_liveFile->close();
		m_liveFile = nullptr;
	}
	if (m_curlMulti)
	{
		if (m_curlEasy)
			curl_multi_remove_handle((CURLM*)m_curlMulti, (CURL*)m_curlEasy);
		curl_multi_cleanup((CURLM*)m_curlMulti);
		m_curlMulti = nullptr;
	}
	if (m_curlEasy)
	{
		curl_easy_cleanup((CURL*)m_curlEasy);
		m_curlEasy = nullptr;
	}
	m_connected.store(false);
	liveObserverLog("LiveObserver::networkThreadFunc ended - totalBytes=%zu totalFrames=%zu\n", totalBytesReceived, totalFramesProcessed);
}

#endif // GENERALS_ONLINE
