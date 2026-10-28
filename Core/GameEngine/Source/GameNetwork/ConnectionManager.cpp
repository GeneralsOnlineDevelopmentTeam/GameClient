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


#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Compression.h"
#include "WWLib/strtok_r.h"
#include "Common/AudioEventRTS.h"
#include "Common/CRCDebug.h"
#include "Common/Debug.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "Common/GameAudio.h"
#include "Common/LocalFileSystem.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/RandomValue.h"
#include "Common/Recorder.h"

#include "GameClient/Diplomacy.h"
#include "GameClient/GameText.h"
#include "GameClient/MessageBox.h"
#include "GameNetwork/ConnectionManager.h"
#include "GameNetwork/LANAPICallbacks.h"
#include "GameNetwork/NAT.h"
#include "GameNetwork/NetCommandWrapperList.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptActions.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/VictoryConditions.h"
#include "GameClient/DisconnectMenu.h"
#include "GameClient/InGameUI.h"
#include "WWLib/TARGA.h"

#include "../NextGenTransport.h"
#include "../NetworkMesh.h"
#include "../ngmp_interfaces.h"

#include <algorithm>

#if defined(GENERALS_ONLINE)
Int MIN_RUNAHEAD = 4;
#endif

static Bool hasValidTransferFileExtension(const AsciiString& filePath)
{
	static const char* const validExtensions[] = {
		"map",
		"ini",
		"str",
		"wak",
		"tga",
		"txt"
	};

	const char* fileExt = strrchr(filePath.str(), '.');

	if (fileExt == nullptr || fileExt[1] == '\0')
	{
		return false;
	}

	fileExt++;

	for (Int i = 0; i < ARRAY_SIZE(validExtensions); ++i)
	{
		if (stricmp(fileExt, validExtensions[i]) == 0)
		{
			return true;
		}
	}

	return false;
}

enum TransferFileType
{
	TransferFileType_Invalid = -1,
	TransferFileType_Map,
	TransferFileType_Ini,
	TransferFileType_Str,
	TransferFileType_Txt,
	TransferFileType_Tga,
	TransferFileType_Wak,
	TransferFileType_Count
};

struct TransferFileRule
{
	const char* ext;
	UnsignedInt maxSize;
};

static const TransferFileRule transferFileRules[TransferFileType_Count] =
{
	{ ".map", 5 * 1024 * 1024 },
	{ ".ini", 2 * 1024 * 1024 },
	{ ".str", 512 * 1024 },
	{ ".txt", 1 * 1024 * 1024 },
	{ ".tga", 2 * 1024 * 1024 },
	{ ".wak", 128 * 1024 },
};

static TransferFileType getTransferFileType(const char* extension)
{
	for (Int i = 0; i < TransferFileType_Count; ++i)
	{
		if (stricmp(extension, transferFileRules[i].ext) == 0)
		{
			return static_cast<TransferFileType>(i);
		}
	}
	return TransferFileType_Invalid;
}

static Bool hasValidTransferFileContent(const AsciiString& filePath, const UnsignedByte* data, UnsignedInt dataSize)
{
	const char* fileExt = strrchr(filePath.str(), '.');
	if (fileExt == nullptr)
	{
		DEBUG_LOG(("File '%s' has no extension for content validation.", filePath.str()));
		return false;
	}

	const TransferFileType fileType = getTransferFileType(fileExt);
	if (fileType == TransferFileType_Invalid)
	{
		DEBUG_LOG(("File '%s' has unrecognized extension '%s' for content validation.", filePath.str(), fileExt));
		return false;
	}

	// Check size limit
	const TransferFileRule& rule = transferFileRules[fileType];
	if (dataSize > rule.maxSize)
	{
		DEBUG_LOG(("File '%s' exceeds maximum size (%u bytes, limit %u bytes).", filePath.str(), dataSize, rule.maxSize));
		return false;
	}

	// Extension-specific content validation
	switch (fileType)
	{
	case TransferFileType_Map:
		break;

	case TransferFileType_Ini:
	{
		for (UnsignedInt i = 0; i < dataSize; ++i)
		{
			if (data[i] == 0)
			{
				DEBUG_LOG(("INI file '%s' contains null bytes (likely binary).", filePath.str()));
				return false;
			}
		}
		break;
	}

	case TransferFileType_Tga:
	{
		if (dataSize < sizeof(TGAHeader) + sizeof(TGA2Footer))
		{
			DEBUG_LOG(("TGA file '%s' is too small to be valid.", filePath.str()));
			return false;
		}
		TGA2Footer footer;
		memcpy(&footer, data + dataSize - sizeof(footer), sizeof(footer));
		const Bool isTGA2 = memcmp(footer.Signature, TGA2_SIGNATURE, sizeof(footer.Signature)) == 0
			&& footer.RsvdChar == '.'
			&& footer.BZST == '\0';
		if (!isTGA2)
		{
			DEBUG_LOG(("TGA file '%s' is missing TRUEVISION-XFILE footer signature.", filePath.str()));
			return false;
		}
		break;
	}

	default:
	{
		break;
	}
	}

	return true;
}

/**
 * Le destructor.
 */
ConnectionManager::~ConnectionManager()
{
#if defined(GENERALS_ONLINE)
	clearSignatureState(TRUE);
#endif

	deleteInstance(m_localUser);
	m_localUser = nullptr;

	// Network will delete transports; we just forget them
	delete m_transport;
	m_transport = nullptr;

	Int i = 0;
	for (; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	// This is done here since TheDisconnectMenu should only be there if we are in a network game.
	delete TheDisconnectMenu;
	TheDisconnectMenu = nullptr;

	delete m_disconnectManager;
	m_disconnectManager = nullptr;

	deleteInstance(m_pendingCommands);
	m_pendingCommands = nullptr;

	deleteInstance(m_relayedCommands);
	m_relayedCommands = nullptr;

	deleteInstance(m_netCommandWrapperList);
	m_netCommandWrapperList = nullptr;

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Le constructor
 */
ConnectionManager::ConnectionManager()
{
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_frameData[i] = nullptr;
		m_lastSeenCommandID[i] = 0;
		m_haveLastSeenCommandID[i] = FALSE;
	}
	m_transport = nullptr;
	m_disconnectManager = nullptr;
	m_pendingCommands = nullptr;
	m_relayedCommands = nullptr;
	m_localAddr = 0;
	m_localPort = 0;
	m_netCommandWrapperList = nullptr;
	m_localUser = nullptr;
	m_localUser = newInstance(User);
#if defined(GENERALS_ONLINE)
	clearSignatureState(FALSE);
#endif
}

/**
 * Initialize the connection manager and any subsystems.
 */
void ConnectionManager::init()
{
#if defined(GENERALS_ONLINE)
	m_networkSettings = NetworkSettings::LocalDefaults();
	NGMP_OnlineServicesManager *services = NGMP_OnlineServicesManager::GetInstance();
	if (TheLAN == nullptr && TheNGMPGame != nullptr && services != nullptr) {
		const ServiceConfig &overrides = services->GetServiceConfig();
		m_networkSettings.send = overrides.send;
		m_networkSettings.input_delay = overrides.input_delay;
		m_networkSettings.disconnect = overrides.disconnect;
	}
	const NetworkSettings::InputDelay &runAhead = m_networkSettings.input_delay;
	TheWritableGlobalData->m_networkFPSHistoryLength = (std::max)(1, runAhead.fps_history_samples);
	TheWritableGlobalData->m_networkLatencyHistoryLength = (std::max)(30, (runAhead.formula.latency_window_ms > 0 ? runAhead.formula.latency_window_ms : runAhead.decision_interval_ms) * GENERALS_ONLINE_HIGH_FPS_LIMIT / 1000);
	TheWritableGlobalData->m_networkRunAheadSlack = runAhead.slack_percent;
	TheWritableGlobalData->m_networkRunAheadMetricsTime = runAhead.decision_interval_ms;
	MIN_RUNAHEAD = runAhead.min_frames;
	FRAME_GROUPING_CAP = (int)(m_networkSettings.send.frames_per_packet * (float)(1000.f / GENERALS_ONLINE_HIGH_FPS_LIMIT));
	TheWritableGlobalData->m_networkDisconnectTime = m_networkSettings.disconnect.freeze_before_screen_ms;
	TheWritableGlobalData->m_networkPlayerTimeoutTime = m_networkSettings.disconnect.remove_after_ms;
#endif

//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	UnsignedInt i = 0;
	for (; i < MAX_SLOTS; ++i) {
		m_connections[i] = nullptr;
	}

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	m_packetRouterSlot = 0; /// @todo The LAN/WOL interface should be telling us who the packet router is based on machine specs passed around through game options.
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
		m_lastSeenCommandID[i] = 0;
		m_haveLastSeenCommandID[i] = FALSE;
	}

	for (i = 0; i < MAX_SLOTS; ++i) {
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

//	m_averageFps = 30;			// since 30 fps is the desired rate, we'll start off at that.
//	m_averageLatency = (Real)0.2; // 200ms seems like a good starting point.

	for (i = 0; i < MAX_SLOTS; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < MAX_SLOTS; ++i) {
		m_latencyAverages[i] = 0.0; // using zero since all floating point standards should be able to specify 0.0 accurately.
	}
	m_smallestPacketArrivalCushion = -1;
#if !RETAIL_COMPATIBLE_NETWORKING
	m_currentSenderSlot = -1;
	m_outFrameAskedSlot = -1;
	m_outFrameAskedFrame = 0;
	m_outFrameAskedTime = 0;
#endif
#if defined(GENERALS_ONLINE)
	m_stallActive = FALSE;
	m_stallStartTime = 0;
	m_stallMissingMask = 0;
	m_lastKnownRunAhead = 0;
	m_runAheadLowerCount = 0;
	m_runAheadLowerPeak = 0;
	m_runAheadStillInitial = TRUE;
	m_pendingRunAhead = 0;
	m_pendingExecFrame = 0;
	m_loggedSenderUnavailable = FALSE;
	m_securityDrops = 0;
	m_directSendActive = FALSE;
	m_frameAckedMasks.clear();
	m_formerRouterMask = 0;
	m_directSendCheckedAt = 0;
	clearSignatureState(TRUE);
	m_shortGrouping = FALSE;
	m_frameGroupingMs = 0;
#endif

	m_frameMetrics.init();

	TheDisconnectMenu = NEW DisconnectMenu;
	TheDisconnectMenu->init();

	m_disconnectManager = NEW DisconnectManager;
	m_disconnectManager->init();

	TheDisconnectMenu->attachDisconnectManager(m_disconnectManager);
	TheDisconnectMenu->hideScreen();

	m_netCommandWrapperList = newInstance(NetCommandWrapperList);
	m_netCommandWrapperList->init();
#if defined(GENERALS_ONLINE)
	resetRecentCommandIds();
#endif

	s_fileCommandMap.clear();
	s_fileRecipientMaskMap.clear();
	for (i = 0; i < MAX_SLOTS; ++i) {
		s_fileProgressMap[i].clear();
	}
}

/**
 * Reset the connection manager and any subsystems.
 */
void ConnectionManager::reset()
{
//	if (m_transport == nullptr) {
//		m_transport = new Transport;
//	}
//	m_transport->reset();

	delete m_transport;
	m_transport = nullptr;

	UnsignedInt i = 0;
	for (; i < (UnsignedInt)MAX_SLOTS; ++i) {
		deleteInstance(m_connections[i]);
		m_connections[i] = nullptr;
	}

	for (i=0; i<(UnsignedInt)MAX_SLOTS; ++i)
	{
		deleteInstance(m_frameData[i]);
		m_frameData[i] = nullptr;
	}

	if (m_pendingCommands == nullptr) {
		m_pendingCommands = newInstance(NetCommandList);
		m_pendingCommands->init();
	}
	m_pendingCommands->reset();

	if (m_relayedCommands == nullptr) {
		m_relayedCommands = newInstance(NetCommandList);
		m_relayedCommands->init();
	}
	m_relayedCommands->reset();

	if (m_netCommandWrapperList == nullptr) {
		m_netCommandWrapperList = newInstance(NetCommandWrapperList);
		m_netCommandWrapperList->init();
	}
	m_netCommandWrapperList->reset();
#if defined(GENERALS_ONLINE)
	resetRecentCommandIds();
	clearSignatureState(TRUE);
#endif

	m_localSlot = -1;
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif
	m_packetRouterSlot = -1;

	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_fpsAverages[i] = -1;
	}
	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_latencyAverages[i] = 0.0;
	}

	for (i = 0; i < (UnsignedInt)MAX_SLOTS; ++i) {
		m_packetRouterFallback[i] = -1;
	}

	m_frameMetrics.reset();
}

UnsignedInt ConnectionManager::getPingFrame()
{
	return (m_disconnectManager)?m_disconnectManager->getPingFrame():0;
}

Int ConnectionManager::getPingsSent()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsSent():0;
}

Int ConnectionManager::getPingsReceived()
{
	return (m_disconnectManager)?m_disconnectManager->getPingsReceived():0;
}

Bool ConnectionManager::isPlayerConnected( Int playerID )
{
	DEBUG_ASSERTCRASH( playerID < MAX_SLOTS, ("ConnectionManager::isPlayerConnected - %d is an invalid player number", playerID) );
	return ( playerID == m_localSlot || (m_connections[playerID] && !m_connections[playerID]->isQuitting()) );
}

void ConnectionManager::attachTransport(Transport *transport) {
	delete m_transport;
	m_transport = transport;
}

/**
 * zero out the command counts for the given frames.  Presently this is used for
 * the start of a game since there won't be any commands for the first few frames due to runahead.
 */
void ConnectionManager::zeroFrames(UnsignedInt startingFrame, UnsignedInt numFrames) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
//			DEBUG_LOG(("Calling zeroFrames on player %d, starting frame %d, numFrames %d", i, startingFrame, numFrames));
			m_frameData[i]->zeroFrames(startingFrame, numFrames);
		}
	}
}

/**
 * Destroy any game messages that are left over due to the run ahead.
 */
void ConnectionManager::destroyGameMessages() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		// Need to destroy these game messages because when the game ends, there are
		// still some game messages left over because of the run ahead aspect of
		// network play.
		if (m_frameData[i] != nullptr) {
			m_frameData[i]->destroyGameMessages();
		}
	}
}

#if defined(GENERALS_ONLINE)
// TRUE if (sender, id) was seen in the last 256 ids, remembers it otherwise. Synchronized commands are deduplicated by the frame data.
Bool ConnectionManager::isDuplicateCommand(NetCommandMsg *msg) {
	if (IsCommandSynchronized(msg->getNetCommandType())) {
		return FALSE;
	}

	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return FALSE;
	}

	RecentCommandIds &recent = m_recentIds[playerID];
	const UnsignedShort id = msg->getID();

	if (!recent.valid) {
		recent.valid = TRUE;
		recent.highest = id;
		for (Int i = 0; i < RECENT_ID_COUNT; ++i) {
			recent.seen[i] = FALSE;
		}
		recent.seen[id % RECENT_ID_COUNT] = TRUE;
		return FALSE;
	}

	const Int diff = (Int)(Short)(id - recent.highest);
	if (diff > 0) {
		// the window moves forward, the slots it moves over belong to ids not seen yet
		for (Int step = 1; step <= diff && step <= RECENT_ID_COUNT; ++step) {
			recent.seen[(recent.highest + step) % RECENT_ID_COUNT] = FALSE;
		}
		recent.highest = id;
		recent.seen[id % RECENT_ID_COUNT] = TRUE;
		return FALSE;
	}

	if (-diff >= RECENT_ID_COUNT) {
		return FALSE;
	}

	if (recent.seen[id % RECENT_ID_COUNT]) {
		return TRUE;
	}
	recent.seen[id % RECENT_ID_COUNT] = TRUE;
	return FALSE;
}

enum { ACK_PRUNE_FRAMES = 256, DIRECT_RECHECK_MS = 250 };

Bool ConnectionManager::isPeerActive(Int slot) const {
	return m_connections[slot] != nullptr && !m_connections[slot]->isQuitting();
}

// Frame infos sent directly are acked by every peer, the latency of a frame is the slowest peer's round trip.
void ConnectionManager::onDirectFrameAck(UnsignedInt frame, Int peerSlot) {
	if (peerSlot < 0 || peerSlot >= MAX_SLOTS || !m_frameMetrics.isPending(frame)) {
		return;
	}

	UnsignedInt &acked = m_frameAckedMasks[frame];
	acked |= (1u << peerSlot);

	UnsignedInt expected = 0;
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		if (slot != m_localSlot && isPeerActive(slot)) {
			expected |= (1u << slot);
		}
	}

	if ((acked & expected) == expected) {
		m_frameMetrics.processLatencyResponse(frame);
		m_frameAckedMasks.erase(frame);
	}

	// frames that never completed, a peer's ack was lost for good
	while (!m_frameAckedMasks.empty() && m_frameAckedMasks.begin()->first + ACK_PRUNE_FRAMES < frame) {
		m_frameAckedMasks.erase(m_frameAckedMasks.begin());
	}
}

Int ConnectionManager::getRunAheadFloor() const {
	const NetworkSettings::InputDelay &runAhead = m_networkSettings.input_delay;
	if (runAhead.min_frames > 0) {
		return runAhead.min_frames;
	}
	// send interval + receive + execute + margin frame
	return (m_shortGrouping ? 1 : m_networkSettings.send.frames_per_packet) + 3;
}

time_t ConnectionManager::getEffectiveDecisionIntervalMs(Int runAhead) const {
	time_t interval = TheGlobalData->m_networkRunAheadMetricsTime; // the floor
	const time_t twiceRunAhead = (time_t)(2 * runAhead * 1000 / GENERALS_ONLINE_HIGH_FPS_LIMIT);
	if (twiceRunAhead > interval) {
		interval = twiceRunAhead;
	}
	return interval;
}

// leaving the one frame sends needs 2 frames more than entering them
void ConnectionManager::updateShortGrouping() {
	if (TheNetwork == nullptr) {
		return;
	}

	const Int runAhead = (Int)TheNetwork->getRunAhead();
	const NetworkSettings::InputDelay &config = m_networkSettings.input_delay;
	const Int floorOneFrame = (config.min_frames > 0) ? config.min_frames : 4;
	if (!m_shortGrouping && runAhead <= floorOneFrame + 2) {
		m_shortGrouping = TRUE;
	} else if (m_shortGrouping && runAhead > floorOneFrame + 4) {
		m_shortGrouping = FALSE;
	}

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_connections[i]->setShortGrouping(m_shortGrouping);
		}
	}
}

Int ConnectionManager::slotForUserID(Int64 userID) const {
	return (TheNGMPGame != nullptr) ? TheNGMPGame->getSlotOfUser(userID) : -1;
}

// Counts a dropped command; TRUE for the drops that are logged (the first 20, then every 100th).
Bool ConnectionManager::noteSecurityDrop() {
	++m_securityDrops;
	return m_securityDrops <= 20 || (m_securityDrops % 100) == 0;
}

// How a command may reach us with another player's name on it: 0 only straight from its author, 1 authenticated by the
// author's signature (frames, chat), also from a former router, 2 only from the current packet router.
static Int relayClass(NetCommandType type) {
	switch (type) {
	case NETCOMMANDTYPE_FRAMEINFO:
	case NETCOMMANDTYPE_SIGNEDFRAMEINFO:
	case NETCOMMANDTYPE_GAMECOMMAND:
	case NETCOMMANDTYPE_PLAYERLEAVE:
	case NETCOMMANDTYPE_RUNAHEAD:
	case NETCOMMANDTYPE_DESTROYPLAYER:
	case NETCOMMANDTYPE_CHAT:
	case NETCOMMANDTYPE_SIGNATURES:
		return 1;
	case NETCOMMANDTYPE_WRAPPER:
	case NETCOMMANDTYPE_RUNAHEADMETRICS:
	case NETCOMMANDTYPE_PROGRESS:
	case NETCOMMANDTYPE_ACKBOTH:
	case NETCOMMANDTYPE_ACKSTAGE1:
	case NETCOMMANDTYPE_ACKSTAGE2:
		return 2;
	default:
		return 0; // votes, disconnect frames, keep alives, load complete, file transfer, ... are never passed on
	}
}

// send.direct is on and every peer has a direct connection; commands then bypass the packet router.
Bool ConnectionManager::isDirectSendActive() {
	if (TheNGMPGame == nullptr || !m_networkSettings.send.direct) {
		return FALSE;
	}

	const time_t now = timeGetTime();
	if (m_directSendCheckedAt != 0 && (now - m_directSendCheckedAt) < DIRECT_RECHECK_MS) {
		return m_directSendActive;
	}
	m_directSendCheckedAt = now;

	Bool meshUp = TRUE;
	NetworkMesh *mesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	for (Int slot = 0; slot < MAX_SLOTS && meshUp; ++slot) {
		if (slot == m_localSlot || !isPeerActive(slot)) {
			continue;
		}
#if !RETAIL_COMPATIBLE_NETWORKING
		// a player that is out needs no commands
		if (m_disconnectManager != nullptr && m_disconnectManager->isPlayerOutOfGame(slot, this)) {
			continue;
		}
#endif
		const NGMPGameSlot *gameSlot = (const NGMPGameSlot *)TheNGMPGame->getConstSlot(slot);
		PlayerConnection *connection = (mesh != nullptr && gameSlot != nullptr) ? mesh->GetConnectionForUser(gameSlot->m_userID) : nullptr;
		if (connection == nullptr || connection->GetState() != EConnectionState::CONNECTED_DIRECT) {
			meshUp = FALSE;
		}
	}

	if (meshUp != m_directSendActive) {
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[DIRECT] commands are now sent %s", meshUp ? "directly to every peer" : "through the packet router, a peer connection is not up");
	}
	m_directSendActive = meshUp;
	return m_directSendActive;
}

// Checks that a command names its sender, or that a router, a former router or a peer holding a removed player's frames passes it on.
Bool ConnectionManager::isCommandFromSender(NetCommandMsg *msg, Int senderSlot) {
	const Int claimedSlot = (Int)msg->getPlayerID();
	if (claimedSlot == senderSlot && claimedSlot >= 0 && claimedSlot < MAX_SLOTS) {
		return TRUE;
	}
	const Int cls = relayClass(msg->getNetCommandType());
	const Bool nameOk = claimedSlot >= 0 && claimedSlot < MAX_SLOTS && claimedSlot != (Int)m_localSlot;
	Bool maySendForOthers = FALSE;
	if (nameOk && cls == 1) {
		maySendForOthers = (senderSlot == m_packetRouterSlot) || (senderSlot >= 0 && (m_formerRouterMask & (1u << senderSlot)) != 0);
	} else if (nameOk && cls == 2) {
		maySendForOthers = (senderSlot == m_packetRouterSlot);
	}
#if !RETAIL_COMPATIBLE_NETWORKING
	// a player that is out sends nothing more, so its last signed frames may come from any peer that has them
	if (nameOk && cls == 1 && !maySendForOthers && m_disconnectManager != nullptr && m_disconnectManager->isPlayerOutOfGame(claimedSlot, this)) {
		maySendForOthers = TRUE;
	}
#endif
	// forwarded frames are checked in isPassedOnFrameAuthentic
	if (maySendForOthers) {
		return TRUE;
	}
	// late frames of a removed player: acked, then ignored
	if (nameOk && cls == 1 && m_connections[claimedSlot] == nullptr
		&& m_signerUserID[claimedSlot] >= 0 && IsCommandSynchronized(msg->getNetCommandType())) {
		return TRUE;
	}

	if (noteSecurityDrop()) {
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped %s claiming slot %d from slot %d (%d dropped so far)",
			GetNetCommandTypeAsString(msg->getNetCommandType()), claimedSlot, (Int)senderSlot, m_securityDrops);
	}
	return FALSE;
}

void ConnectionManager::resetRecentCommandIds() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		m_recentIds[i].valid = FALSE;
	}
}

static Int64 userIDOfSlot(Int slot) {
	return (TheNGMPGame != nullptr) ? TheNGMPGame->getUserOfSlot(slot) : -1;
}

static MatchKeys *activeMatchKeys() {
	NetworkMesh *mesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	if (mesh == nullptr || !mesh->GetMatchKeys().IsActive()) {
		return nullptr;
	}
	return &mesh->GetMatchKeys();
}

static void appendBytes(std::vector<UnsignedByte> &out, UnsignedInt64 value, Int count) {
	for (Int i = 0; i < count; ++i) {
		out.push_back((UnsignedByte)(value >> (8 * i)));
	}
}

static void appendSignedHeader(std::vector<UnsignedByte> &out, Char kind, UnsignedInt64 matchID, Int64 userID, Int slot) {
	out.push_back('G');
	out.push_back('O');
	out.push_back('S');
	out.push_back((UnsignedByte)kind);
	out.push_back(1); // version
	appendBytes(out, matchID, 8);
	appendBytes(out, (UnsignedInt64)userID, 8);
	out.push_back((UnsignedByte)slot);
}

// The command as the full packet format writes it, without the relay mask that differs per hop.
static void appendCommandBytes(std::vector<UnsignedByte> &out, NetCommandMsg *msg) {
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(0);
	const size_t start = out.size();
	const size_t size = msg->getSizeForNetPacket();
	appendBytes(out, 0, 4);
	out.resize(start + 4 + size);
	const size_t written = msg->copyBytesForNetPacket(&out[start + 4], *ref);
	out.resize(start + 4 + written);
	for (Int i = 0; i < 4; ++i) {
		out[start + i] = (UnsignedByte)(written >> (8 * i));
	}
	deleteInstance(ref);
}

static Bool commandSignOrder(const NetCommandMsg *a, const NetCommandMsg *b) {
	if (a->getNetCommandType() != b->getNetCommandType()) {
		return a->getNetCommandType() < b->getNetCommandType();
	}
	return a->getID() < b->getID();
}

void ConnectionManager::clearSignatureState(Bool releaseChats) {
	for (Int slot = 0; slot < MAX_SLOTS; ++slot) {
		m_frameSignatures[slot].assign(FRAME_DATA_LENGTH, FrameSignature());
		m_nextRelayedWrapper[slot] = 0;
		m_signerUserID[slot] = -1;
		for (Int wrapper = 0; wrapper < RELAYED_WRAPPER_COUNT; ++wrapper) {
			m_relayedWrappers[slot][wrapper].used = FALSE;
		}
	}
	for (Int held = 0; held < HELD_CHAT_COUNT; ++held) {
		if (releaseChats && m_heldChats[held].msg != nullptr) {
			m_heldChats[held].msg->detach();
		}
		m_heldChats[held].msg = nullptr;
		m_chatSignatures[held].used = FALSE;
	}
	m_currentSenderSlot = -1;
	m_lastFrameInfoSent = 0;
}

ConnectionManager::FrameSignature *ConnectionManager::findFrameSignature(Int slot, UnsignedInt frame) {
	if (slot < 0 || slot >= MAX_SLOTS) {
		return nullptr;
	}
	FrameSignature &entry = m_frameSignatures[slot][frame % FRAME_DATA_LENGTH];
	return (entry.used && entry.frame == frame) ? &entry : nullptr;
}

ConnectionManager::FrameSignature &ConnectionManager::frameSignatureFor(Int slot, UnsignedInt frame) {
	FrameSignature &entry = m_frameSignatures[slot][frame % FRAME_DATA_LENGTH];
	if (!entry.used || entry.frame != frame) {
		entry = FrameSignature();
		entry.used = TRUE;
		entry.frame = frame;
		entry.relayedBy = -1;
	}
	return entry;
}

// The user id of a slot, kept once seen so the last frames of a player that left can still be verified.
Int64 ConnectionManager::signerUserID(Int slot) {
	if (slot < 0 || slot >= MAX_SLOTS) {
		return -1;
	}
	const Int64 userID = userIDOfSlot(slot);
	if (userID >= 0) {
		m_signerUserID[slot] = userID;
	}
	return m_signerUserID[slot];
}

void ConnectionManager::noteFramePassedOn(Int author, UnsignedInt frame, Int relayedBy, Bool changesFrame) {
	if (author < 0 || author >= MAX_SLOTS || author == (Int)m_localSlot) {
		return;
	}
	if (TheGameLogic != nullptr && frame < TheGameLogic->getFrame()) {
		return;
	}
	FrameSignature &entry = frameSignatureFor(author, frame);
	entry.relayed = TRUE;
	entry.relayedBy = relayedBy;
	if (changesFrame) {
		entry.verified = FALSE;
	}
}

void ConnectionManager::noteWrapperPassedOn(NetCommandMsg *wrapper, Int relayedBy) {
	const Int author = (Int)wrapper->getPlayerID();
	if (author < 0 || author >= MAX_SLOTS) {
		return;
	}
	const UnsignedShort wrappedID = ((NetWrapperCommandMsg *)wrapper)->getWrappedCommandID();
	for (Int i = 0; i < RELAYED_WRAPPER_COUNT; ++i) {
		if (m_relayedWrappers[author][i].used && m_relayedWrappers[author][i].wrappedCommandID == wrappedID) {
			m_relayedWrappers[author][i].relayedBy = relayedBy;
			return;
		}
	}
	RelayedWrapper &entry = m_relayedWrappers[author][m_nextRelayedWrapper[author]];
	m_nextRelayedWrapper[author] = (m_nextRelayedWrapper[author] + 1) % RELAYED_WRAPPER_COUNT;
	entry.used = TRUE;
	entry.wrappedCommandID = wrappedID;
	entry.relayedBy = relayedBy;
}

Int ConnectionManager::wrapperPassedOnBy(NetCommandMsg *msg) {
	const Int author = (Int)msg->getPlayerID();
	if (author < 0 || author >= MAX_SLOTS) {
		return -2;
	}
	for (Int i = 0; i < RELAYED_WRAPPER_COUNT; ++i) {
		if (m_relayedWrappers[author][i].used && m_relayedWrappers[author][i].wrappedCommandID == msg->getID()) {
			m_relayedWrappers[author][i].used = FALSE;
			return m_relayedWrappers[author][i].relayedBy;
		}
	}
	return author;
}

/// Signed bytes: header (match, user id, slot), frame, command count, then the frame's commands in full packet form (relay 0) sorted by type and id.
/// The frame info id is not signed, a peer that passes it on renumbers it.
Bool ConnectionManager::buildSignedFrame(Int slot, UnsignedInt frame, std::vector<UnsignedByte> &out) {
	MatchKeys *keys = activeMatchKeys();
	const Int64 userID = signerUserID(slot);
	if (keys == nullptr || userID < 0 || m_frameData[slot] == nullptr) {
		return FALSE;
	}

	std::vector<NetCommandMsg *> commands;
	NetCommandList *list = m_frameData[slot]->getFrameCommandList(frame);
	for (NetCommandRef *ref = (list != nullptr) ? list->getFirstMessage() : nullptr; ref != nullptr; ref = ref->getNext()) {
		commands.push_back(ref->getCommand());
	}
	std::sort(commands.begin(), commands.end(), commandSignOrder);

	out.clear();
	appendSignedHeader(out, 'F', keys->GetMatchID(), userID, slot);
	appendBytes(out, frame, 4);
	appendBytes(out, m_frameData[slot]->getFrameCommandCount(frame), 2);
	for (size_t i = 0; i < commands.size(); ++i) {
		appendCommandBytes(out, commands[i]);
	}
	return TRUE;
}

// What the author signs for its chat, the one command it sends through the packet router outside a frame.
Bool ConnectionManager::buildSignedCommand(Int slot, NetCommandMsg *msg, std::vector<UnsignedByte> &out) {
	MatchKeys *keys = activeMatchKeys();
	const Int64 userID = signerUserID(slot);
	if (keys == nullptr || userID < 0) {
		return FALSE;
	}
	out.clear();
	appendSignedHeader(out, 'C', keys->GetMatchID(), userID, slot);
	appendCommandBytes(out, msg);
	return TRUE;
}

/// Signs a local frame that has commands; the signature rides in the frame info. Empty frames are not signed.
void ConnectionManager::signLocalFrame(NetFrameCommandMsg *frameInfo) {
	MatchKeys *keys = activeMatchKeys();
	if (keys == nullptr || frameInfo->getCommandCount() == 0) {
		return;
	}

	std::vector<UnsignedByte> data;
	UnsignedByte signature[SIGNATURE_BYTES];
	if (!buildSignedFrame(m_localSlot, frameInfo->getExecutionFrame(), data) || !keys->Sign(&data[0], data.size(), signature)) {
		return;
	}
	frameInfo->setSignature(signature);
}

void ConnectionManager::signLocalChat(NetChatCommandMsg *chat) {
	MatchKeys *keys = activeMatchKeys();
	if (keys == nullptr || m_packetRouterSlot >= MAX_SLOTS || m_packetRouterSlot == m_localSlot || m_connections[m_packetRouterSlot] == nullptr
		|| isDirectSendActive()) {
		return;
	}

	std::vector<UnsignedByte> data;
	UnsignedByte signature[SIGNATURE_BYTES];
	if (!buildSignedCommand(m_localSlot, chat, data) || !keys->Sign(&data[0], data.size(), signature)) {
		return;
	}

	NetSignaturesCommandMsg *msg = newInstance(NetSignaturesCommandMsg);
	msg->addEntry(chat->getExecutionFrame(), chat->getID(), signature);
	msg->setPlayerID(m_localSlot);
	msg->setID(GenerateNextCommandID());
	sendLocalCommand(msg, (UnsignedByte)(0xff & ~(1 << m_localSlot)));
	msg->detach();
}

// What the leaver signs: the match, its user id and slot. A leave carries no frame, so a copy a peer passes on keeps its proof.
Bool ConnectionManager::buildSignedLeave(Int slot, std::vector<UnsignedByte> &out) {
	MatchKeys *keys = activeMatchKeys();
	const Int64 userID = signerUserID(slot);
	if (keys == nullptr || userID < 0) {
		return FALSE;
	}
	out.clear();
	appendSignedHeader(out, 'L', keys->GetMatchID(), userID, slot);
	return TRUE;
}

// The leaving player signs its own leave; without a key yet the notice goes out unsigned.
Bool ConnectionManager::signLeaveNotice(NetDisconnectPlayerCommandMsg *msg) {
	MatchKeys *keys = activeMatchKeys();
	if (keys == nullptr || (Int)msg->getDisconnectSlot() != (Int)m_localSlot) {
		return FALSE;
	}
	std::vector<UnsignedByte> data;
	UnsignedByte signature[SIGNATURE_BYTES];
	if (!buildSignedLeave(m_localSlot, data) || !keys->Sign(&data[0], data.size(), signature)) {
		return FALSE;
	}
	msg->setSignature(signature);
	return TRUE;
}

// TRUE if the notice carries the signature of the player it says left.
Bool ConnectionManager::verifyLeaveNotice(NetDisconnectPlayerCommandMsg *msg) {
	const Int slot = (Int)msg->getDisconnectSlot();
	MatchKeys *keys = activeMatchKeys();
	if (!msg->hasSignature() || slot < 0 || slot >= MAX_SLOTS || keys == nullptr) {
		return FALSE;
	}
	std::vector<UnsignedByte> data;
	return buildSignedLeave(slot, data) && keys->Verify(signerUserID(slot), &data[0], data.size(), msg->getSignature());
}

void ConnectionManager::attachStoredSignature(Int author, NetFrameCommandMsg *frameInfo) {
	FrameSignature *entry = findFrameSignature(author, frameInfo->getExecutionFrame());
	if (entry != nullptr && entry->haveSignature) {
		frameInfo->setSignature(entry->signature);
	}
}

// the author's signature replaces a passed on one, never the reverse
void ConnectionManager::storeFrameSignature(Int author, UnsignedInt frame, const UnsignedByte *signature, Bool fromAuthor) {
	if (author < 0 || author >= MAX_SLOTS || author == (Int)m_localSlot) {
		return;
	}
	if (TheGameLogic != nullptr && frame + FRAMES_TO_KEEP < TheGameLogic->getFrame()) {
		return;
	}
	FrameSignature &entry = frameSignatureFor(author, frame);
	if (entry.haveSignature && !fromAuthor) {
		return;
	}
	if (!entry.haveSignature || memcmp(entry.signature, signature, SIGNATURE_BYTES) != 0) {
		memcpy(entry.signature, signature, SIGNATURE_BYTES);
		entry.haveSignature = TRUE;
		entry.verified = FALSE;
	}
}

// Asks the author for the frame again when we reach it, and the peer that passed it on, which holds it in any case.
void ConnectionManager::requestPassedOnFrameAgain(Int slot, UnsignedInt frame, Int passedOnBy) {
	UnsignedByte targets = 0;
	if (isPlayerConnected(slot)) {
		targets |= (UnsignedByte)(1 << slot);
	}
	if (passedOnBy >= 0 && passedOnBy < MAX_SLOTS && passedOnBy != (Int)m_localSlot && isPlayerConnected(passedOnBy)) {
		targets |= (UnsignedByte)(1 << passedOnBy);
	}
	if (targets == 0) {
		requestFrameDataResend(slot, frame);
		return;
	}

	NetFrameResendRequestCommandMsg *msg = newInstance(NetFrameResendRequestCommandMsg);
	msg->setPlayerID(m_localSlot);
	msg->setFrameToResend(frame);
	msg->setID(GenerateNextCommandID());
	sendLocalCommandDirect(msg, targets);
	msg->detach();
}

/// A frame that a peer other than its author passed on runs only with the author's valid signature, else it is cleared and asked for again.
Bool ConnectionManager::isPassedOnFrameAuthentic(Int slot, UnsignedInt frame) {
	FrameSignature *entry = findFrameSignature(slot, frame);
	if (entry == nullptr || !entry->relayed || entry->verified || !TheNetwork->matchKeysRequired()) {
		return TRUE;
	}
	// empty frames are not signed
	if (m_frameData[slot]->getFrameCommandCount(frame) == 0) {
		return TRUE;
	}

	const char *problem = nullptr;
	if (!entry->haveSignature) {
		const time_t now = timeGetTime();
		if (entry->waitingSince == 0) {
			entry->waitingSince = now;
		}
		if ((now - entry->waitingSince) < SIGNATURE_WAIT_MS) {
			return FALSE;
		}
		problem = "unsigned";
	} else {
		MatchKeys *keys = activeMatchKeys();
		std::vector<UnsignedByte> data;
		if (keys != nullptr && buildSignedFrame(slot, frame, data) && keys->Verify(signerUserID(slot), &data[0], data.size(), entry->signature)) {
			entry->verified = TRUE;
			return TRUE;
		}
		problem = "forged";
	}

	if (noteSecurityDrop()) {
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped %s commands for player %d relayed by %d (frame %u, %d dropped so far), asking for them again",
			problem, slot, entry->relayedBy, frame, m_securityDrops);
	}
	const Int passedOnBy = entry->relayedBy;
	m_frameData[slot]->resetFrame(frame, FALSE);
	*entry = FrameSignature();
	entry->used = TRUE;
	entry->frame = frame;
	entry->relayedBy = -1;
	requestPassedOnFrameAgain(slot, frame, passedOnBy);
	return FALSE;
}

void ConnectionManager::processSignatures(NetSignaturesCommandMsg *msg) {
	const Int author = (Int)msg->getPlayerID();
	if (author < 0 || author >= MAX_SLOTS || author == (Int)m_localSlot) {
		return;
	}

	const time_t now = timeGetTime();
	for (Int e = 0; e < msg->getEntryCount(); ++e) {
		const NetSignaturesCommandMsg::Entry &signedEntry = msg->getEntry(e);
		expireHeldChats(now);
		Bool released = FALSE;
		for (Int held = 0; held < HELD_CHAT_COUNT && !released; ++held) {
			NetChatCommandMsg *chat = m_heldChats[held].msg;
			if (chat != nullptr && (Int)chat->getPlayerID() == author && chat->getID() == signedEntry.commandId) {
				releaseHeldChat(held, signedEntry.signature);
				released = TRUE;
			}
		}
		if (released) {
			continue;
		}

		// the chat is not here yet, keep the signature for it, the oldest one gives way
		Int index = 0;
		for (Int i = 0; i < HELD_CHAT_COUNT; ++i) {
			if (!m_chatSignatures[i].used) {
				index = i;
				break;
			}
			if (m_chatSignatures[i].since < m_chatSignatures[index].since) {
				index = i;
			}
		}
		ChatSignature &stored = m_chatSignatures[index];
		stored.used = TRUE;
		stored.author = (UnsignedInt)author;
		stored.commandID = signedEntry.commandId;
		stored.since = now;
		memcpy(stored.signature, signedEntry.signature, SIGNATURE_BYTES);
	}
}

// Chat the packet router relays is held until its author's signature checks out.
Bool ConnectionManager::holdPassedOnChat(NetChatCommandMsg *msg) {
	const Int author = (Int)msg->getPlayerID();
	const Int relayedBy = m_currentSenderSlot;
	if (relayedBy < 0 || author == relayedBy || author == (Int)m_localSlot || !TheNetwork->matchKeysRequired()) {
		return FALSE;
	}

	const time_t now = timeGetTime();
	expireHeldChats(now);

	Int index = 0;
	for (Int i = 0; i < HELD_CHAT_COUNT; ++i) {
		if (m_heldChats[i].msg == nullptr) {
			index = i;
			break;
		}
		if (m_heldChats[i].since < m_heldChats[index].since) {
			index = i;
		}
	}
	if (m_heldChats[index].msg != nullptr) {
		++m_securityDrops;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped unsigned chat for player %d relayed by %d, too many chats wait for a signature",
			(Int)m_heldChats[index].msg->getPlayerID(), m_heldChats[index].relayedBy);
		m_heldChats[index].msg->detach();
		m_heldChats[index].msg = nullptr;
	}
	msg->attach();
	m_heldChats[index].msg = msg;
	m_heldChats[index].relayedBy = relayedBy;
	m_heldChats[index].since = now;

	for (Int i = 0; i < HELD_CHAT_COUNT; ++i) {
		if (m_chatSignatures[i].used && m_chatSignatures[i].author == (UnsignedInt)author && m_chatSignatures[i].commandID == msg->getID()) {
			m_chatSignatures[i].used = FALSE;
			releaseHeldChat(index, m_chatSignatures[i].signature);
			break;
		}
	}
	return TRUE;
}

void ConnectionManager::releaseHeldChat(Int index, const UnsignedByte *signature) {
	NetChatCommandMsg *chat = m_heldChats[index].msg;
	const Int relayedBy = m_heldChats[index].relayedBy;
	m_heldChats[index].msg = nullptr;

	const Int author = (Int)chat->getPlayerID();
	MatchKeys *keys = activeMatchKeys();
	std::vector<UnsignedByte> data;
	if (keys != nullptr && buildSignedCommand(author, chat, data) && keys->Verify(signerUserID(author), &data[0], data.size(), signature)) {
		processChat(chat);
	} else {
		++m_securityDrops;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped forged chat for player %d relayed by %d", author, relayedBy);
	}
	chat->detach();
}

void ConnectionManager::expireHeldChats(time_t now) {
	for (Int i = 0; i < HELD_CHAT_COUNT; ++i) {
		if (m_heldChats[i].msg != nullptr && (now - m_heldChats[i].since) > SIGNATURE_WAIT_MS * 3) {
			++m_securityDrops;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped unsigned chat for player %d relayed by %d",
				(Int)m_heldChats[i].msg->getPlayerID(), m_heldChats[i].relayedBy);
			m_heldChats[i].msg->detach();
			m_heldChats[i].msg = nullptr;
		}
		if (m_chatSignatures[i].used && (now - m_chatSignatures[i].since) > SIGNATURE_WAIT_MS * 3) {
			m_chatSignatures[i].used = FALSE;
		}
	}
}
#endif

/**
 * ConnectionManager::doRelay()
 * Queries the transport for commands that need to be relayed to another client.
 * Get those commands and relay them to the appropriate Connection(s). We make the
 * assumption that a command will only be relayed once.
 */
void ConnectionManager::doRelay() {
	NetPacket *packet = nullptr;

	for (size_t i = 0; i < ARRAY_SIZE(m_transport->m_inBuffer); ++i) {
		if (m_transport->m_inBuffer[i].length > 0) {
			// This transport buffer has yet to be processed.

			// make a NetPacket out of this data so it can be broken up into individual commands.
			packet = newInstance(NetPacket)(&(m_transport->m_inBuffer[i]));

			//DEBUG_LOG(("ConnectionManager::doRelay() - got a packet with %d commands", packet->getNumCommands()));
			//LOGBUFFER( packet->getData(), packet->getLength() );

			// Get the command list from the packet.
			NetCommandList *cmdList = packet->getCommandList();

#if defined(GENERALS_ONLINE)
			// -2 when the transport cannot say who sent the packet
			Int senderSlot = -2;
			if (m_transport->m_inBuffer[i].senderKnown) {
				senderSlot = slotForUserID(m_transport->m_inBuffer[i].senderUserID);
				if (senderSlot < 0) {
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] dropped a packet of %d commands from user %lld, who is not in this game", (Int)packet->getNumCommands(), (long long)m_transport->m_inBuffer[i].senderUserID);
				}
			} else if (!m_loggedSenderUnavailable) {
				m_loggedSenderUnavailable = TRUE;
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[SECURITY] the transport does not say who sent a packet (anticheat plugin path), commands are not checked against their sender");
			}
#endif

#if !RETAIL_COMPATIBLE_NETWORKING
#if defined(GENERALS_ONLINE)
			m_currentSenderSlot = (senderSlot >= 0) ? senderSlot : -1;
#else
			m_currentSenderSlot = getSlotOfAddress(m_transport->m_inBuffer[i].addr, m_transport->m_inBuffer[i].port);
#endif
#endif

			// Iterate through the commands in this packet and send them to the proper connections.
			for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
				//DEBUG_LOG(("ConnectionManager::doRelay() - Looking at a command of type %s",
					//GetNetCommandTypeAsString(cmd->getCommand()->getNetCommandType())));

#if defined(GENERALS_ONLINE)
				if (senderSlot == -1 || (senderSlot >= 0 && !isCommandFromSender(cmd->getCommand(), senderSlot))) {
					continue; // not acked and not processed
				}
				if (senderSlot >= 0 && cmd->getCommand()->getNetCommandType() == NETCOMMANDTYPE_WRAPPER && (Int)cmd->getCommand()->getPlayerID() != senderSlot) {
					noteWrapperPassedOn(cmd->getCommand(), senderSlot);
				}
#endif

				if (CommandRequiresAck(cmd->getCommand())) {
					ackCommand(cmd, m_localSlot);
#if defined(GENERALS_ONLINE)
					if (isDuplicateCommand(cmd->getCommand())) {
						continue; // a redundant copy, acked again in case the first ack was lost
					}
#endif
				}
				if (!processNetCommand(cmd)) {
					sendRemoteCommand(cmd);
				}
			}
#if defined(GENERALS_ONLINE)
			m_currentSenderSlot = -1;
#endif

			// Delete this packet since we won't be needing it anymore.
			deleteInstance(packet);
			packet = nullptr;

			deleteInstance(cmdList);
			cmdList = nullptr;

			// signal that this has been processed.
			m_transport->m_inBuffer[i].length = 0;
		} else {
			break;
		}
	}

#if !RETAIL_COMPATIBLE_NETWORKING
	m_currentSenderSlot = -1;
#endif

	NetCommandList *cmdList = m_netCommandWrapperList->getReadyCommands();
	for (NetCommandRef* cmd = cmdList->getFirstMessage(); cmd; cmd = cmd->getNext()) {
#if defined(GENERALS_ONLINE)
		// a command put together from pieces some other peer passed on counts as passed on by that peer
		m_currentSenderSlot = wrapperPassedOnBy(cmd->getCommand());
		if (m_currentSenderSlot < 0 || !isCommandFromSender(cmd->getCommand(), m_currentSenderSlot)) {
			m_currentSenderSlot = -1;
			continue; // the command inside the pieces is held to the same rule as one sent plain
		}
#endif
		if (CommandRequiresAck(cmd->getCommand())) {
			ackCommand(cmd, m_localSlot);
#if defined(GENERALS_ONLINE)
			if (isDuplicateCommand(cmd->getCommand())) {
				continue;
			}
#endif
		}
		if (!processNetCommand(cmd)) {
			sendRemoteCommand(cmd);
		}
	}
#if defined(GENERALS_ONLINE)
	m_currentSenderSlot = -1;
#endif

	// Delete this packet since we won't be needing it anymore.
	deleteInstance(packet);
	packet = nullptr;

	deleteInstance(cmdList);
	cmdList = nullptr;
}

/**
 * This is where the non-synchronized network commands should be processed.
 * Return TRUE if the command should not be relayed. Return FALSE if it should be relayed.
 */
Bool ConnectionManager::processNetCommand(NetCommandRef *ref) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandType cmdType = msg->getNetCommandType();

	// Track the command ids of each player, the ids far away from them are free for passed on frame infos.
	if (DoesCommandRequireACommandID(cmdType) && msg->getPlayerID() < MAX_SLOTS && msg->getPlayerID() != m_localSlot) {
		const UnsignedInt from = msg->getPlayerID();
		const UnsignedShort distance = (UnsignedShort)(msg->getID() - m_lastSeenCommandID[from]);
		if (m_haveLastSeenCommandID[from] == FALSE || distance < 0x4000 || distance > 0xC000) {
			m_lastSeenCommandID[from] = msg->getID();
			m_haveLastSeenCommandID[from] = TRUE;
		}
	}

	// Handle ACK commands first (before connection validation)
	if ((cmdType == NETCOMMANDTYPE_ACKSTAGE1) ||
			(cmdType == NETCOMMANDTYPE_ACKSTAGE2) ||
			(cmdType == NETCOMMANDTYPE_ACKBOTH)) {
		processAck(msg);
		return FALSE;
	}

	// Early validation checks
	if (msg->getPlayerID() >= MAX_SLOTS) {
		return TRUE;
	}

	if ((m_connections[msg->getPlayerID()] == nullptr) && (msg->getPlayerID() != m_localSlot)) {
		// if this is from a player that is no longer in the game, then ignore them.
		return TRUE;
	}

	// Handle WRAPPER commands (before second connection validation)
	if (cmdType == NETCOMMANDTYPE_WRAPPER) {
		processWrapper(ref); // need to send the NetCommandRef since we have to construct the relay for the wrapped command.
		return FALSE;
	}

	if (msg->getPlayerID() != m_localSlot) {
		if (m_connections[msg->getPlayerID()] == nullptr) {
			return TRUE;
		}
	}

	// Don't allow an out of date command to be sent through.
	// Its unnecessary traffic and it could cause problems.
	//
	// This was a fix for a command count bug where a command would be
	// executed, then a command for that old frame would be added to the
	// FrameData for that frame + 256, and would screw up the command count.
	if (IsCommandSynchronized(cmdType)) {
		if (ref->getCommand()->getExecutionFrame() < TheGameLogic->getFrame()) {
			return TRUE;
		}
	}

	// Handle disconnect commands as a range
	if ((cmdType > NETCOMMANDTYPE_DISCONNECTSTART) && (cmdType < NETCOMMANDTYPE_DISCONNECTEND)) {
		m_disconnectManager->processDisconnectCommand(ref, this);
		return TRUE;
	}

	// Process command by type
	switch (cmdType) {

		case NETCOMMANDTYPE_FRAMEINFO: {
			processFrameInfo((NetFrameCommandMsg *)msg);
			// need to set the relay so we don't send it to ourselves.
			UnsignedByte relay = ref->getRelay();
			relay = relay & (0xff ^ (1 << m_localSlot));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_PROGRESS: {
			//DEBUG_LOG(("ConnectionManager::processNetCommand - got a progress net command from player %d", msg->getPlayerID()));
			processProgress((NetProgressCommandMsg *) msg);
			// need to set the relay so we don't send it to ourselves.
			UnsignedByte relay = ref->getRelay();
			relay = relay & (0xff ^ (1 << m_localSlot));
			ref->setRelay(relay);
			return FALSE;
		}

		case NETCOMMANDTYPE_TIMEOUTSTART:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a TimeOut GameStart net command from player %d", msg->getPlayerID()));
			processTimeOutGameStart(msg);
			return FALSE;

		case NETCOMMANDTYPE_RUNAHEADMETRICS:
			processRunAheadMetrics((NetRunAheadMetricsCommandMsg *)msg);
			return TRUE;

		case NETCOMMANDTYPE_KEEPALIVE:
			return TRUE;

		case NETCOMMANDTYPE_DISCONNECTCHAT:
			processDisconnectChat((NetDisconnectChatCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_LOADCOMPLETE:
			DEBUG_LOG(("ConnectionManager::processNetCommand - got a Load Complete net command from player %d", msg->getPlayerID()));
			processLoadComplete(msg);
			return FALSE;

		case NETCOMMANDTYPE_CHAT:
#if defined(GENERALS_ONLINE)
			if (holdPassedOnChat((NetChatCommandMsg *)msg)) {
				return FALSE; // shown once the author's signature checks out
			}
#endif
			processChat((NetChatCommandMsg *)msg);
			return FALSE;

#if defined(GENERALS_ONLINE)
		case NETCOMMANDTYPE_SIGNATURES:
			processSignatures((NetSignaturesCommandMsg *)msg);
			return FALSE;
#endif

		case NETCOMMANDTYPE_FILE:
			processFile((NetFileCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FILEANNOUNCE:
			processFileAnnounce((NetFileAnnounceCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FILEPROGRESS:
			processFileProgress((NetFileProgressCommandMsg *)msg);
			return FALSE;

		case NETCOMMANDTYPE_FRAMERESENDREQUEST:
			processFrameResendRequest((NetFrameResendRequestCommandMsg *)msg);
			return TRUE;

		default:
			return FALSE;
	}
}

void ConnectionManager::processFrameResendRequest(NetFrameResendRequestCommandMsg *msg) {
	// first make sure this is a valid slot
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}

	// make sure this player is still in our game.
	if ((m_connections[playerID] == nullptr) || (m_connections[playerID]->isQuitting() == TRUE)) {
		return;
	}

	sendFrameDataToPlayer(playerID, msg->getFrameToResend());
}

/**
 * We have received a wrapper for a command too big to fit in a packet.
 */
void ConnectionManager::processWrapper(NetCommandRef *ref)
{
	NetWrapperCommandMsg *wrapperMsg = (NetWrapperCommandMsg *)(ref->getCommand());
	UnsignedShort commandID = wrapperMsg->getWrappedCommandID();
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - wrapped commandID is %d, commandID is %d",
		commandID, wrapperMsg->getID()));
	Int origProgress = 0;
	FileCommandMap::iterator fcIt = s_fileCommandMap.find(commandID);
	if (fcIt != s_fileCommandMap.end())
	{
		origProgress = s_fileProgressMap[m_localSlot][commandID];
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - origProgress[%d] == %d for command %d",
		m_localSlot, origProgress, commandID));

	m_netCommandWrapperList->processWrapper(ref);

	if (fcIt != s_fileCommandMap.end())
	{
		Int newProgress = m_netCommandWrapperList->getPercentComplete(commandID);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - newProgress[%d] == %d for command %d",
			m_localSlot, newProgress, commandID));
		if (newProgress > origProgress && newProgress < 100)
		{
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processWrapper() - sending a NetFileProgressCommandMsg"));
			s_fileProgressMap[m_localSlot][commandID] = newProgress;

			Int progressMask = 0xff ^ (1 << m_localSlot);
			NetFileProgressCommandMsg *msg = newInstance(NetFileProgressCommandMsg);
			msg->setPlayerID(m_localSlot);
			msg->setID(0);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()))
			{
				msg->setID(GenerateNextCommandID());
			}
			msg->setFileID(commandID);
			msg->setProgress(newProgress);
			sendLocalCommand(msg, progressMask);
			processFileProgress(msg);
			msg->detach();
		}
	}
}

/**
 * A client has sent us their run ahead metrics, lets store them away for future calculations.
 */
void ConnectionManager::processRunAheadMetrics(NetRunAheadMetricsCommandMsg *msg)
{
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (isPlayerConnected(playerID)) {
		m_latencyAverages[playerID] = msg->getAverageLatency();
		m_fpsAverages[playerID] = msg->getAverageFps();
		//DEBUG_LOG(("ConnectionManager::processRunAheadMetrics - player %d, fps = %d, latency = %f", player, msg->getAverageFps(), msg->getAverageLatency()));

		// NGMP_CHANGE: Modern machines render games at much higher framerates, 100 is no longer only achievable when a game is in the background...
#if defined(GENERALS_ONLINE)
		if (m_fpsAverages[playerID] > 1000) {
			m_fpsAverages[playerID] = 1000;
		}
#else
		if (m_fpsAverages[player] > 100) {
			// limit the reported frame rate average to 100.  This is done because if a
			// user alt-tab's out of the game their frame rate climbs to in the neighborhood of
			// 300, that was deemed "ugly" by the powers that be.
			m_fpsAverages[playerID] = 100;
		}
#endif
	}
}

void ConnectionManager::processDisconnectChat(NetDisconnectChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
	} else if (isPlayerConnected(playerID)) {
		name = m_connections[playerID]->getUser()->GetName();
	}
	unitext.format(L"[%ls] %ls", name.str(), msg->getText().str());
//	DEBUG_LOG(("ConnectionManager::processDisconnectChat - got message from player %d, message is %ls", playerID, unitext.str()));
	TheDisconnectMenu->showChat(unitext); // <-- need to implement this
}

void ConnectionManager::processChat(NetChatCommandMsg *msg)
{
	UnicodeString unitext;
	UnicodeString name;
	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	//DEBUG_LOG(("processChat(): playerID = %d", playerID));
	if (playerID == m_localSlot) {
		name = m_localUser->GetName();
		//DEBUG_LOG(("connection is null, using %ls", name.str()));
	} else if ((m_connections[playerID] != nullptr) && (m_connections[playerID]->isQuitting() == FALSE)) {
		name = m_connections[playerID]->getUser()->GetName();
		//DEBUG_LOG(("connection is non-null, using %ls", name.str()));
	}
	unitext.format(L"[%ls] %ls", name.str(), msg->getText().str());
//	DEBUG_LOG(("ConnectionManager::processChat - got message from player %d (mask %8.8X), message is %ls", playerID, msg->getPlayerMask(), unitext.str()));

	AsciiString playerName;
	playerName.format("player%d", msg->getPlayerID());
	const Player *player = ThePlayerList->findPlayerWithNameKey( TheNameKeyGenerator->nameToKey( playerName ) );
	if (!player)
	{
		TheInGameUI->message(L"%ls", unitext.str());
		return;
	}

	Bool fromObserver = !player->isPlayerActive();
	Bool amIObserver = !ThePlayerList->getLocalPlayer()->isPlayerActive();
	Bool canSeeChat = (amIObserver || !fromObserver) && !TheGameInfo->getConstSlot(playerID)->isMuted();

	if ( ((1<<m_localSlot) & msg->getPlayerMask() ) && canSeeChat  )
	{
		RGBColor rgb;
		rgb.setFromInt(player->getPlayerColor());
#if defined(GENERALS_ONLINE)
		TheInGameUI->messageColor(true, &rgb, UnicodeString(L"%ls"), unitext.str());
#else
		TheInGameUI->messageColor(&rgb, L"%ls", unitext.str());
#endif

		// feedback for received chat messages in-game
		AudioEventRTS audioEvent("GUICommunicatorIncoming");
		TheAudio->addAudioEvent(&audioEvent);
	}
}

void ConnectionManager::processFile(NetFileCommandMsg *msg)
{
#ifdef DEBUG_LOGGING
	UnicodeString log;
	UnicodeString uFilename;
	uFilename.translate(msg->getPortableFilename());
	log.format(L"Saw file transfer: '%s' of %d bytes from %d", uFilename.str(), msg->getFileLength(), msg->getPlayerID());
	DEBUG_LOG(("%ls", log.str()));
#endif

	AsciiString realFileName = msg->getRealFilename();
	if (realFileName.isEmpty())
	{
		// TheSuperHackers @security slurmlord 18/06/2025 As the file name/path from the NetFileCommandMsg failed to normalize,
		// in other words is bogus and points outside of the approved target directory, avoid an arbitrary file overwrite vulnerability
		// by simply returning and let the transfer time out.
		DEBUG_LOG(("Got a file name transferred that failed to normalize: '%s'!", msg->getPortableFilename().str()));
		return;
	}

	// TheSuperHackers @security bobtista 06/11/2025 Validate file extension to prevent arbitrary file types
	if (!hasValidTransferFileExtension(realFileName))
	{
		DEBUG_LOG(("File '%s' has invalid extension for transfer operations.", realFileName.str()));
		return;
	}

	if (TheFileSystem->doesFileExist(realFileName.str()))
	{
		DEBUG_LOG(("File exists already!"));
		//return;
	}

	UnsignedByte *buf = msg->getFileData();
	Int len = msg->getFileLength();

	// uncompress Targas
#ifdef COMPRESS_TARGAS
	Bool deleteBuf = FALSE;
	if (msg->getPortableFilename().endsWith(".tga") && CompressionManager::isDataCompressed(buf, len))
	{
		Int uncompLen = CompressionManager::getUncompressedSize(buf, len);
		UnsignedByte *uncompBuffer = NEW UnsignedByte[uncompLen];
		Int actualLen = CompressionManager::decompressData(buf, len, uncompBuffer, uncompLen);
		if (actualLen == uncompLen)
		{
			DEBUG_LOG(("Uncompressed Targa after map transfer"));
			deleteBuf = TRUE;
			buf = uncompBuffer;
			len = uncompLen;
		}
		else
		{
			DEBUG_LOG(("Failed to uncompress Targa after map transfer"));
			delete[] uncompBuffer; // failed to decompress, so just use the source
		}
	}
#endif // COMPRESS_TARGAS

	// TheSuperHackers @security bobtista 12/02/2026 Validate file content in memory before writing to disk
	if (!hasValidTransferFileContent(realFileName, buf, len))
	{
		DEBUG_LOG(("File '%s' failed content validation. Transfer aborted.", realFileName.str()));
#ifdef COMPRESS_TARGAS
		if (deleteBuf)
		{
			delete[] buf;
			buf = nullptr;
		}
#endif // COMPRESS_TARGAS
		return;
	}

	File *fp = TheFileSystem->openFile(realFileName.str(), File::CREATE | File::BINARY | File::WRITE);
	if (fp)
	{
		fp->write(buf, len);
		fp->close();
		fp = nullptr;
		DEBUG_LOG(("Wrote %d bytes to file %s!", len, realFileName.str()));

	}
	else
	{
		DEBUG_LOG(("Cannot open file!"));
	}

	DEBUG_LOG(("ConnectionManager::processFile() - sending a NetFileProgressCommandMsg"));

	Int commandID = msg->getID();
	Int newProgress = 100;

	s_fileProgressMap[m_localSlot][commandID] = newProgress;

	Int progressMask = 0xff ^ (1 << m_localSlot);
	NetFileProgressCommandMsg *progressMsg = newInstance(NetFileProgressCommandMsg);
	progressMsg->setPlayerID(m_localSlot);
	progressMsg->setID(0);
	if (DoesCommandRequireACommandID(progressMsg->getNetCommandType()))
	{
		progressMsg->setID(GenerateNextCommandID());
	}
	progressMsg->setFileID(commandID);
	progressMsg->setProgress(newProgress);
	sendLocalCommand(progressMsg, progressMask);
	processFileProgress(progressMsg);
	progressMsg->detach();

#ifdef COMPRESS_TARGAS
	if (deleteBuf)
	{
		delete[] buf;
		buf = nullptr;
	}
#endif // COMPRESS_TARGAS
}

void ConnectionManager::processFileAnnounce(NetFileAnnounceCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileAnnounce() - expecting '%s' (%s) in command %d", msg->getPortableFilename().str(), msg->getRealFilename().str(), msg->getFileID()));
	s_fileCommandMap[msg->getFileID()] = msg->getRealFilename();
	s_fileRecipientMaskMap[msg->getFileID()] = msg->getPlayerMask();
	for (Int i=0; i<MAX_SLOTS; ++i)
	{
		if ( (1<<i) & msg->getPlayerMask() )
		{
			s_fileProgressMap[i][msg->getFileID()] = 0;
		}
		else
		{
			s_fileProgressMap[i][msg->getFileID()] = 100; // they don't need to get it, so they're already done.
		}
	}
}

void ConnectionManager::processFileProgress(NetFileProgressCommandMsg *msg)
{
	DEBUG_LOG(("ConnectionManager::processFileProgress() - command %d is at %d%%",
		msg->getFileID(), msg->getProgress()));

	const UnsignedInt playerID = msg->getPlayerID();
	if (playerID >= MAX_SLOTS) {
		return;
	}
	const UnsignedShort fileID = msg->getFileID();
	const Int oldProgress = s_fileProgressMap[playerID][fileID];
	s_fileProgressMap[playerID][fileID] = max(oldProgress, msg->getProgress());
}

void ConnectionManager::processProgress( NetProgressCommandMsg *msg )
{
	TheGameLogic->processProgress(msg->getPlayerID(), msg->getPercentage());
}

void ConnectionManager::processLoadComplete( NetCommandMsg *msg )
{
	TheGameLogic->processProgressComplete(msg->getPlayerID());
}

void ConnectionManager::processTimeOutGameStart( NetCommandMsg *msg )
{
	TheGameLogic->timeOutGameStart();
}

/**
 * Another client has sent us the command count for a new frame.
 */
void ConnectionManager::processFrameInfo(NetFrameCommandMsg *msg) {
	//stupid frame info, why don't you process yourself?

	const UnsignedInt playerID = msg->getPlayerID();

	if (playerID < MAX_SLOTS) {
		if (m_frameData[playerID] != nullptr) {
#if defined(GENERALS_ONLINE)
			if (msg->hasSignature()) {
				storeFrameSignature((Int)playerID, msg->getExecutionFrame(), msg->getSignature(), m_currentSenderSlot == (Int)playerID);
			}
			if (m_currentSenderSlot >= 0 && (Int)playerID != m_currentSenderSlot) {
				noteFramePassedOn((Int)playerID, msg->getExecutionFrame(), m_currentSenderSlot, m_frameData[playerID]->getFrameCommandCount(msg->getExecutionFrame()) != msg->getCommandCount());
			}
#endif
//			DEBUG_LOG(("ConnectionManager::processFrameInfo - player %d, frame %d, command count %d, received on frame %d", playerID, msg->getExecutionFrame(), msg->getCommandCount(), TheGameLogic->getFrame()));
#if defined(GENERALS_ONLINE)
			// a redundant copy of a frame that was already seen says nothing about how early frames arrive
			NetCommandList *frameCommands = m_frameData[playerID]->getFrameCommandList(msg->getExecutionFrame());
			const Bool isCopy = (frameCommands != nullptr) && (frameCommands->findMessage(msg) != nullptr);
#endif
			m_frameData[playerID]->setFrameCommandCount(msg->getExecutionFrame(), msg->getCommandCount());
#if defined(GENERALS_ONLINE)
			if (!isCopy && (playerID != (UnsignedInt)m_localSlot) && (TheGameLogic != nullptr)) {
				m_cushionStats[playerID].note((Int)msg->getExecutionFrame() - (Int)TheGameLogic->getFrame(), timeGetTime());
			}
#endif
		}
	}
}

/**
 * We just got a stage 1 ack from someone.  So we should remove it from the connection that sent it so
 * it doesn't keep resending it.
 */
void ConnectionManager::processAckStage1(NetCommandMsg *msg) {
#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	const UnsignedInt playerID = msg->getPlayerID();
	NetCommandRef *ref = nullptr;

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processAck - processing ack for command %d from player %d", ((NetAckStage1CommandMsg *)msg)->getCommandID(), playerID));
	}
#endif

	if (playerID < MAX_SLOTS) {
		if (m_connections[playerID] != nullptr) {
			ref = m_connections[playerID]->processAck(msg);
		}
	} else {
		DEBUG_CRASH(("ConnectionManager::processAck - %d is an invalid player number", playerID));
	}

	if (ref != nullptr) {
		if (ref->getCommand()->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO || ref->getCommand()->getNetCommandType() == NETCOMMANDTYPE_SIGNEDFRAMEINFO) {
			const UnsignedInt ackedFrame = ref->getCommand()->getExecutionFrame();
#if defined(GENERALS_ONLINE)
			if (isDirectSendActive()) {
				onDirectFrameAck(ackedFrame, (Int)playerID);
			} else
#endif
			{
				m_frameMetrics.processLatencyResponse(ackedFrame);
			}
		}

		deleteInstance(ref);
		ref = nullptr;
	}
}

/**
 * We just got a stage 2 ack from someone.  So remove it from the pending commands list so it doesn't
 * get sent in the case of a new packet router.
 */
void ConnectionManager::processAckStage2(NetCommandMsg *msg) {
	UnsignedShort commandID = 0;
	UnsignedByte playerID = 0;
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) {
		commandID = ((NetAckStage2CommandMsg *)msg)->getCommandID();
		playerID = ((NetAckStage2CommandMsg *)msg)->getOriginalPlayerID();
	} else if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		commandID = ((NetAckBothCommandMsg *)msg)->getCommandID();
		playerID = ((NetAckBothCommandMsg *)msg)->getOriginalPlayerID();
	} else {
		return;
	}

	NetCommandRef *ref = m_pendingCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - removing command %d from the pending commands list.", commandID));
		DEBUG_ASSERTCRASH((m_localSlot == playerID), ("Found a command in the pending commands list that wasn't originated by the local player"));
		m_pendingCommands->removeMessage(ref);
		deleteInstance(ref);
		ref = nullptr;
	} else {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - Couldn't find command %d from player %d in the pending commands list.", commandID, playerID));
	}

	ref = m_relayedCommands->findMessage(commandID, playerID);
	if (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - found command ID %d from player %d in the relayed commands list.", commandID, playerID));
		UnsignedByte prevRelay = ref->getRelay();
		UnsignedByte relay = prevRelay & ~(1 << msg->getPlayerID());
		//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay was %d and is now %d", relay, prevRelay));
		if (relay == 0) {
			//DEBUG_LOG(("ConnectionManager::processAckStage2 - relay is 0, removing command from the relayed commands list."));
			m_relayedCommands->removeMessage(ref);
			NetAckStage2CommandMsg *ackmsg = newInstance(NetAckStage2CommandMsg)(ref->getCommand());
			sendLocalCommand(ackmsg, 1 << ackmsg->getOriginalPlayerID());
			deleteInstance(ref);
			ref = nullptr;

			ackmsg->detach();
			ackmsg = nullptr;
		} else {
			ref->setRelay(relay);
		}
	}
}

/**
 * We just got a "both" ack from someone.  So process it as both a stage 1 and stage 2 ack.
 */
void ConnectionManager::processAck(NetCommandMsg *msg) {
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage1(msg);
	}
	if ((msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE2) || (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH)) {
		processAckStage2(msg);
	}
}

/**
 * A player has just left our game. Delete their connection and frame data manager.
 * return codes are:
 * PLAYERLEAVECODE_UNKNOWN - player didn't have a valid slot number.
 * PLAYERLEAVECODE_CLIENT - someone in the game that wasn't us or the packet router.
 * PLAYERLEAVECODE_LOCAL - We are leaving the game, we could also be the packet router.
 * PLAYERLEAVECODE_PACKETROUTER - The packet router left the game.
 *
 * If we are leaving and are also the packet router, it will return the PLAYERLEAVECODE_LOCAL return code.
 */
PlayerLeaveCode ConnectionManager::processPlayerLeave(NetPlayerLeaveCommandMsg *msg) {
	UnsignedByte playerID = msg->getLeavingPlayerID();
	if ((playerID != m_localSlot) && (m_connections[playerID] != nullptr)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave() - setQuitting() on player %d on frame %d", playerID, TheGameLogic->getFrame()));
		m_connections[playerID]->setQuitting();
	}
	DEBUG_ASSERTCRASH(m_frameData[playerID]->getIsQuitting() == FALSE, ("Player %d is already quitting", playerID));
	if ((playerID != m_localSlot) && (m_frameData[playerID] != nullptr) && (m_frameData[playerID]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::processPlayerLeave - setQuitFrame on player %d for frame %d", playerID, TheGameLogic->getFrame()+1));
		m_frameData[playerID]->setQuitFrame(TheGameLogic->getFrame() + FRAMES_TO_KEEP + 1);
	}

	if (playerID == m_localSlot)
	{
		// we're leaving, so mark our connections and frame datas to go away.
		for (Int i=0; i<MAX_SLOTS; ++i)
		{
			if (m_connections[i])
			{
				m_connections[i]->clearCommandsExceptFrom(m_localSlot);
				m_connections[i]->setQuitting();
			}
		}
	}

	PlayerLeaveCode code = disconnectPlayer(playerID);
	DEBUG_LOG(("ConnectionManager::processPlayerLeave() - just disconnected player %d with ret code %d", playerID, code));
	if (code == PLAYERLEAVECODE_PACKETROUTER)
		resendPendingCommands();

	PopulateInGameDiplomacyPopup();
	return code;
}

UnsignedInt ConnectionManager::getPacketRouterFallbackSlot(Int packetRouterNumber) {
	if ((packetRouterNumber >= 0) && (packetRouterNumber < MAX_SLOTS)) {
		return m_packetRouterFallback[packetRouterNumber];
	}
	return MAX_SLOTS;
}

UnsignedInt ConnectionManager::getPacketRouterSlot() {
	return m_packetRouterSlot;
}

Bool ConnectionManager::areAllQueuesEmpty() {
	Bool retval = TRUE;
	for (Int i = 0; (i < MAX_SLOTS) && retval; ++i) {
		if (m_connections[i] != nullptr) {
			if (m_connections[i]->isQueueEmpty() == FALSE) {
				//DEBUG_LOG(("ConnectionManager::areAllQueuesEmpty() - m_connections[%d] is not empty", i));
				//m_connections[i]->debugPrintCommands();
				retval = FALSE;
			}
		}
	}

	return retval;
}

Bool ConnectionManager::canILeave() {
	return areAllQueuesEmpty();
}

/**
 * The local player is leaving. Tell the local player as well as the other players
 * to remove this player at the specified frame.
 */
void ConnectionManager::handleLocalPlayerLeaving(UnsignedInt frame) {
	NetPlayerLeaveCommandMsg *msg = newInstance(NetPlayerLeaveCommandMsg);

	msg->setLeavingPlayerID(m_localSlot);
	msg->setExecutionFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	DEBUG_LOG(("ConnectionManager::handleLocalPlayerLeaving - Local player leaving on frame %d", frame));
	DEBUG_ASSERTCRASH(m_packetRouterSlot >= 0, ("ConnectionManager::handleLocalPlayerLeaving, packet router is %d, illegal value.", m_packetRouterSlot));

	sendLocalCommand(msg);

	msg->detach();
}

/**
 * We just got a message that needs to be ack'd, so ack it!
 */
void ConnectionManager::ackCommand(NetCommandRef *ref, UnsignedInt localSlot) {
	NetCommandMsg *msg = ref->getCommand();
	NetCommandMsg *ackmsg;
	UnsignedShort commandID;
	UnsignedByte originalPlayerID;
	UnsignedByte sendRelay = 0;

	// Make send relay a bitmask for the connections that the relay will actually be sent to. This is
	// necessary to determine whether or not we have to wait to send a stage 2 ack.
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			sendRelay = sendRelay | (1 << i);
		}
	}

#if defined(RTS_DEBUG)
	Bool doDebug = (msg->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) ? TRUE : FALSE;
#endif

	sendRelay = sendRelay & ref->getRelay();
	if (sendRelay == 0) {
		NetAckBothCommandMsg *bothmsg = newInstance(NetAckBothCommandMsg)(ref->getCommand());
		ackmsg = bothmsg;
		commandID = bothmsg->getCommandID();
		originalPlayerID = bothmsg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack both for command %d from player %d", bothmsg->getCommandID(), bothmsg->getOriginalPlayerID()));
		}
#endif
	} else {
		NetAckStage1CommandMsg *stage1msg = newInstance(NetAckStage1CommandMsg)(ref->getCommand());
		ackmsg = stage1msg;
		commandID = stage1msg->getCommandID();
		originalPlayerID = stage1msg->getOriginalPlayerID();
#if defined(RTS_DEBUG)
		if (doDebug) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::ackCommand - doing ack stage 1 for command %d from player %d", stage1msg->getCommandID(), stage1msg->getOriginalPlayerID()));
		}
#endif
	}

	ackmsg->setPlayerID(localSlot); // Tell the player who this ack is coming from.

#if !RETAIL_COMPATIBLE_NETWORKING
	// a peer other than the router that passed on another player's command waits for the ack itself
	if ((m_currentSenderSlot >= 0) && (m_currentSenderSlot != (Int)msg->getPlayerID()) && (m_currentSenderSlot != (Int)m_packetRouterSlot) && (m_connections[m_currentSenderSlot] != nullptr)) {
		m_connections[m_currentSenderSlot]->sendNetCommandMsg(ackmsg, 1 << m_currentSenderSlot);
		ackmsg->detach();
		return;
	}
#endif

	// A command that came straight from its author and needs no relaying is acked straight back to it, one the router
	// passed on is acked to the router. The arrival decides, not our own direct send mode: peers can be in different modes.
	Bool ackDirectly = CommandRequiresDirectSend(msg) && CommandRequiresAck(msg);
#if defined(GENERALS_ONLINE) && !RETAIL_COMPATIBLE_NETWORKING
	if (!ackDirectly && sendRelay == 0 && msg->getPlayerID() < MAX_SLOTS && m_connections[msg->getPlayerID()] != nullptr) {
		// an unknown sender (anticheat transport) falls back to our own mode
		ackDirectly = (m_currentSenderSlot >= 0) ? (m_currentSenderSlot == (Int)msg->getPlayerID()) : isDirectSendActive();
	}
#endif
	if (ackDirectly) {
		// Send this ack directly back to the sending player, don't go through the packet router.
		if (msg->getPlayerID() < MAX_SLOTS) {
			if (m_connections[msg->getPlayerID()] != nullptr) {
				m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, 1 << msg->getPlayerID());
			}
		}
	} else {
		// The local connection may be the packet router, in that case, the connection would be null.  So do something about it!
		if ((m_packetRouterSlot >= 0) && (m_packetRouterSlot < MAX_SLOTS)) {
			if (m_connections[m_packetRouterSlot] != nullptr) {
//				DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d to packet router.", commandID, m_packetRouterSlot));
				m_connections[m_packetRouterSlot]->sendNetCommandMsg(ackmsg, 1 << m_packetRouterSlot);
			} else if (m_localSlot == m_packetRouterSlot) {
				// we are the packet router, send the ack to the player that sent the command.
				if (msg->getPlayerID() < MAX_SLOTS) {
					if (m_connections[msg->getPlayerID()] != nullptr) {
//						DEBUG_LOG(("ConnectionManager::ackCommand - acking command %d from player %d directly to player.", commandID, msg->getPlayerID()));
						m_connections[msg->getPlayerID()]->sendNetCommandMsg(ackmsg, 1 << msg->getPlayerID());
					} else {
	//					DEBUG_CRASH(("Connection to player is null"));
					}
				} else {
					DEBUG_CRASH(("Command sent by an invalid player ID."));
				}
			} else {
				DEBUG_CRASH(("Connection to packet router is null"));
			}
		} else {
			DEBUG_CRASH(("I don't know who the packet router is."));
		}
	}

	ackmsg->detach();
}

/**
 * This is where we relay a command from one client to others (including ourselves).
 * This should only be done by the current packet router.
 */
void ConnectionManager::sendRemoteCommand(NetCommandRef *msg) {
	UnsignedByte actualRelay = 0;
	if (msg->getCommand() == nullptr) {
		return;
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - sending net command %d of type %s from player %d, relay is 0x%x",
		msg->getCommand()->getID(), GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), msg->getRelay()));

	const UnsignedByte relay = msg->getRelay();
	const UnsignedInt playerID = msg->getCommand()->getPlayerID();
	FrameDataManager *frameDataMgr = playerID < MAX_SLOTS ? m_frameData[playerID] : nullptr;
	Bool isNew = TRUE;
	if ((relay & (1 << m_localSlot)) && (frameDataMgr != nullptr)) {
		if (IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getCommand()->getNetCommandType()), msg->getCommand()->getPlayerID(), msg->getCommand()->getExecutionFrame()));
			isNew = frameDataMgr->addNetCommandMsg(msg->getCommand());
#if defined(GENERALS_ONLINE)
			if (isNew && m_currentSenderSlot >= 0 && (Int)playerID != m_currentSenderSlot) {
				noteFramePassedOn((Int)playerID, msg->getCommand()->getExecutionFrame(), m_currentSenderSlot, TRUE);
			}
#endif
		}
	}

#if defined(GENERALS_ONLINE)
	if (!isNew) {
		return; // a redundant copy, it was already relayed and counted
	}
#endif

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (1 << i)) && ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE))) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendRemoteCommand - relaying command %d to player %d", msg->getCommand()->getID(), i));
			m_connections[i]->sendNetCommandMsg(msg->getCommand(), 1 << i);
			actualRelay = actualRelay | (1 << i);
		}
	}

	if ((actualRelay != 0) && (CommandRequiresAck(msg->getCommand()) == TRUE)) {
		NetCommandRef *ref = m_relayedCommands->addMessage(msg->getCommand());
		if (ref != nullptr) {
			ref->setRelay(actualRelay);
			//DEBUG_LOG(("ConnectionManager::sendRemoteCommand - command %d added to relayed commands with relay %d", msg->getCommand()->getID(), ref->getRelay()));
		}
	}

	// Do some metrics to find the minimum packet arrival cushion.
	if (isNew && IsCommandSynchronized(msg->getCommand()->getNetCommandType())) {
//		DEBUG_LOG(("ConnectionManager::sendRemoteCommand - about to call allCommandsReady"));
		if (allCommandsReady(msg->getCommand()->getExecutionFrame(), TRUE)) {
			UnsignedInt cushion = msg->getCommand()->getExecutionFrame() - TheGameLogic->getFrame();
			if ((cushion < m_smallestPacketArrivalCushion) || (m_smallestPacketArrivalCushion == -1)) {
				m_smallestPacketArrivalCushion = cushion;
			}
			m_frameMetrics.addCushion(cushion);
//			DEBUG_LOG(("Adding %d to cushion for frame %d", cushion, msg->getCommand()->getExecutionFrame()));
		}
	}
}

/**
 * ConnectionManager::update
 * Update the connections. Tell them to do the receive and send.  Also relay
 * commands to their final destinations as necessary.
 */
void ConnectionManager::update(Bool isInGame) {
//
// 1. do this
// 2. do that
// 3. do something else
// 4. blow something up
// 5. bust some cap
//

	if ((m_localAddr == 0) || (m_localPort == 0)) {
		// we don't have a local address or port yet, this is bad.
		DEBUG_ASSERTCRASH((m_localAddr != 0) && (m_localPort != 0), ("ConnectionManager doesn't have a local address."));
		return;
	}

	m_transport->doRecv();

	if (isInGame) {
		m_disconnectManager->update(this);
	}

	// take the packets from the transport, break them up into commands, and give them to the appropriate connections.
	doRelay();

	// send any necessary keep-alive packets.
	doKeepAlive();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			/*
			if (m_connections[i]->isQueueEmpty() == FALSE) {
//				DEBUG_LOG(("ConnectionManager::update - calling doSend on connection %d", i));
			}
			*/

			m_connections[i]->doSend();

			if (m_connections[i]->isQuitting() && m_connections[i]->isQueueEmpty())
			{
				DEBUG_LOG(("ConnectionManager::update - deleting connection for slot %d", i));
				deleteInstance(m_connections[i]);
				m_connections[i] = nullptr;
			}
		}

		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == TRUE)) {
			if (m_frameData[i]->getQuitFrame() == TheGameLogic->getFrame()) {
				DEBUG_LOG(("ConnectionManager::update - deleting frame data for slot %d on quitting frame %d", i, m_frameData[i]->getQuitFrame()));
				deleteInstance(m_frameData[i]);
				m_frameData[i] = nullptr;
			}
		}
	}

#if defined(GENERALS_ONLINE)
	updateShortGrouping();
#endif
	m_transport->doSend();
}

void ConnectionManager::updateRunAhead(Int oldRunAhead, Int frameRate, Bool didSelfSlug, Int nextExecutionFrame) {
	static time_t lasttimesent = 0;
	time_t curTime = timeGetTime();

#if defined(GENERALS_ONLINE)
	m_lastKnownRunAhead = oldRunAhead;
#endif

#if defined(GENERALS_ONLINE)
	const time_t decisionInterval = getEffectiveDecisionIntervalMs(oldRunAhead);
#else
	const time_t decisionInterval = TheGlobalData->m_networkRunAheadMetricsTime;
#endif
	if ((lasttimesent == 0) || ((curTime - lasttimesent) > decisionInterval)) {
#if defined(GENERALS_ONLINE)
		if (m_networkSettings.input_delay.formula.latency_window_ms <= 0) {
			// the in-band latency is averaged over the samples since the previous decision
			const UnsignedInt samples = (UnsignedInt)(decisionInterval * GENERALS_ONLINE_HIGH_FPS_LIMIT / 1000);
			TheWritableGlobalData->m_networkLatencyHistoryLength = (samples > 30) ? samples : 30;
		}
#endif
		if (m_localSlot == m_packetRouterSlot) {
			// We are the packet router, time to compute a new run ahead for this game.
			m_latencyAverages[m_localSlot] = m_frameMetrics.getAverageLatency();

			// since we are now using the display frame rate rather than the logic frame rate to get our average FPS,
			// it doesn't make sense to send the desired logic frame rate if we "slugged" ourself.
//			if (didSelfSlug) {
//				m_fpsAverages[m_localSlot] = frameRate;
//			} else {
			m_fpsAverages[m_localSlot] = m_frameMetrics.getAverageFPS();
			//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, actual fps = %d, latency = %f, didSelfSlug = true", m_fpsAverages[m_localSlot], m_frameMetrics.getAverageFPS(), m_latencyAverages[m_localSlot]));
			}
			else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - local player run ahead metrics, fps = %d, latency = %f, didSelfSlug = false", m_fpsAverages[m_localSlot], m_latencyAverages[m_localSlot]));
			}
			Int minFps;
			Int minFpsPlayer;
			getMinimumFps(minFps, minFpsPlayer);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - max latency = %f, min fps = %d, min fps player = %d old FPS = %d", getMaximumLatency(), minFps, minFpsPlayer, frameRate));
			if ((minFps >= ((frameRate * 9) / 10)) && (minFps < frameRate)) {
				// if the minimum fps is within 10% of the desired framerate, then keep the current minimum fps.
				minFps = frameRate;
			}

			// TheSuperHackers @info this clamps the logic time scale fps in network games
			minFps = clamp<Int>(MIN_LOGIC_FRAMES, minFps, TheNetwork->getFrameRate());
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::updateRunAhead - minFps after adjustment is %d", minFps));

			// TheSuperHackers @bugfix Mauller 21/08/2025 calculate the runahead so it always follows the latency
			// The runahead should always be rounded up to the next integer value to prevent variations in latency from causing stutter
			// The network slack pushes the runahead up to the next value when the latency is within the slack percentage of the current runahead

			const Real runAheadSlackScale = 1.0f + (Real)TheGlobalData->m_networkRunAheadSlack / 100.0f;

			Int newRunAhead = ceilf(getMaximumLatency() * runAheadSlackScale * (Real)minFps);

			// TheSuperHackers @info if the runahead goes below 3 logic frames it can start to introduce stutter
			// We also limit the upper range of the runahead to prevent it getting out of hand
			Int minRunAheadForClamp = MIN_RUNAHEAD;
#if defined(GENERALS_ONLINE)
			minRunAheadForClamp = getRunAheadFloor();
#endif

#if defined(GENERALS_ONLINE)
			// a lost packet is repaired by the copy in the next one, a send interval later: budget for it
			Int repairFrames = 0;
			if (m_networkSettings.input_delay.formula.repair_budget) {
				time_t repairInterval = 0;
				for (Int repairSlot = 0; repairSlot < MAX_SLOTS; ++repairSlot) {
					if (m_connections[repairSlot] != nullptr && m_connections[repairSlot]->getRedundancyDepth() > 0 && m_connections[repairSlot]->getFrameGrouping() > repairInterval) {
						repairInterval = m_connections[repairSlot]->getFrameGrouping();
					}
				}
				repairFrames = (Int)((repairInterval * GENERALS_ONLINE_HIGH_FPS_LIMIT + 999) / 1000);
			}
			newRunAhead += repairFrames;
#endif
			newRunAhead = clamp<Int>(minRunAheadForClamp, newRunAhead, MAX_FRAMES_AHEAD / 2);

#if defined(GENERALS_ONLINE)
			const Int decreaseAfter = m_networkSettings.input_delay.formula.decrease_after_decisions;
			// a command that was issued but has not taken effect yet is the run ahead to decide from, not the one still running
			const Int baseRunAhead = (m_pendingExecFrame != 0 && oldRunAhead != m_pendingRunAhead && TheGameLogic->getFrame() <= m_pendingExecFrame + 2) ? m_pendingRunAhead : oldRunAhead;
			newRunAhead = applyDecreaseHold(baseRunAhead, newRunAhead, decreaseAfter);
#endif

			NetRunAheadCommandMsg* msg = newInstance(NetRunAheadCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}

			// needs to be set to the greater of getExecutionFrame and TheGameLogic->getFrame() + oldRunAhead
			// This prevents the case of...
			// run ahead starts at 30
			// run ahead changes to 10 at frame 31 (the command was created on frame 1)
			// run ahead changes to 10 at frame 56 (the command was created on frame 46)
			// notice that 56 is within the previous run ahead of 30 which has triggered
			// the frame command count being set for frames 1 through 60 since run ahead
			// didn't change for the first time till frame 31.  This creates an extra command
			// for frame 56 that isn't accounted for in the frame command count that is sent
			// out in the NetFrameCommandMsg.  sheesh.
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg->setExecutionFrame(nextExecutionFrame);
			}
			else {
				msg->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

#if defined(GENERALS_ONLINE) // provide instant responsiveness if there are no remote human players
			if (TheNGMPGame != nullptr)
			{
				NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
				if (pMesh != nullptr)
				{
					if (pMesh->GetAllConnections().size() == 0)
					{
						newRunAhead = 0;
					}
				}
			}
#endif

			msg->setRunAhead(newRunAhead);
			msg->setFrameRate(minFps);
#if defined(GENERALS_ONLINE)
			m_pendingRunAhead = newRunAhead;
			m_pendingExecFrame = msg->getExecutionFrame();
#endif

			//DEBUG_LOG(("ConnectionManager::updateRunAhead - new run ahead = %d, new frame rate = %d, execution frame %d", newRunAhead, minFps, msg->getExecutionFrame()));
			sendLocalCommand(msg, 0xff ^ (1 << minFpsPlayer)); // Send the packet to everyone but the lowest FPS player.

			NetRunAheadCommandMsg* msg2 = newInstance(NetRunAheadCommandMsg);
			msg2->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg2->getNetCommandType())) {
				/*
				 * Ok there needs to be a big friggin comment about this change...
				 * What happens is that the two run ahead commands get sent to different players
				 * using different command ID's.  So player 1 has the run ahead command as command x
				 * and player 2 has the command as command x+1.  This is all good except when it comes
				 * to players being disconnected.  With the new disconnect scheme player 1 could potentially
				 * send his run ahead command to player 2 (or the other way around) to let player 2 catch
				 * up to him.  So if player 2 has his run ahead command as x+1 and now he gets player 1's
				 * command list with the run ahead command listed as command x, he won't see them as being
				 * the same command and will now think he has two different run ahead commands for that frame
				 * and thus his command list will have an extra command and he will never be able to recover.
				 * So to fix this we have to use the same command ID for both run ahead commands.  That way
				 * when the commands are copied places for the disconnect screen they will be seen as the
				 * same command, and all will be good.
				 */
				 //				msg2->setID(GenerateNextCommandID());
				msg2->setID(msg->getID());
			}
			if (nextExecutionFrame > (TheGameLogic->getFrame() + oldRunAhead)) {
				msg2->setExecutionFrame(nextExecutionFrame);
			}
			else {
				msg2->setExecutionFrame(TheGameLogic->getFrame() + oldRunAhead);
			}

			// Let the player with the slowest FPS run a little faster than the other computers...
			// just in case they are able to.  Then we might be able to run the game faster which would be good.
			Int newMinFps = (minFps * 11) / 10;
			if (newMinFps == minFps) {
				newMinFps = minFps + 1;
			}


			if (newMinFps > TheNetwork->getFrameRate()) {
				newMinFps = TheNetwork->getFrameRate(); // Cap FPS to network frame rate.
			}
			msg2->setRunAhead(newRunAhead);
			msg2->setFrameRate(newMinFps);

			sendLocalCommand(msg2, 1 << minFpsPlayer);

			msg->detach();
			msg2->detach();
		}
		else if (m_packetRouterSlot != -1) {
			// We are not the packet router, send our metrics info to the packet router.
			NetRunAheadMetricsCommandMsg* msg = newInstance(NetRunAheadMetricsCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(GenerateNextCommandID());
			}
			msg->setAverageLatency(m_frameMetrics.getAverageLatency());

			// see above for explanation.
//			if (didSelfSlug) {
//				msg->setAverageFps(frameRate);
//			} else {
			msg->setAverageFps(m_frameMetrics.getAverageFPS());
			//			}
			if (didSelfSlug) {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, actual fps = %d, didSelfSlug = true", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS(), m_frameMetrics.getAverageFPS()));
			}
			else {
				//DEBUG_LOG(("ConnectionManager::updateRunAhead - average latency = %f, average fps = %d, didSelfSlug = false", m_frameMetrics.getAverageLatency(), m_frameMetrics.getAverageFPS()));
			}

			if (m_packetRouterSlot != -1)
			{
				Connection* connection = m_connections[m_packetRouterSlot];
				if (connection != nullptr)
				{
					connection->sendNetCommandMsg(msg, 1 << m_packetRouterSlot);
				}
			}
			msg->detach();
		}
		lasttimesent = curTime;
	}
}

#if defined(GENERALS_ONLINE)
// The run ahead goes up at once and falls only after decreaseAfter asks for less in a row, then to the highest of them.
Int ConnectionManager::applyDecreaseHold(Int oldRunAhead, Int askedRunAhead, Int decreaseAfter) {
	if (m_runAheadStillInitial) {
		m_runAheadLowerCount = 0;
		return askedRunAhead;
	}
	if (decreaseAfter > 0 && askedRunAhead < oldRunAhead) {
		m_runAheadLowerPeak = (m_runAheadLowerCount == 0) ? askedRunAhead : max(m_runAheadLowerPeak, askedRunAhead);
		++m_runAheadLowerCount;
		if (m_runAheadLowerCount >= decreaseAfter) {
			m_runAheadLowerCount = 0;
			return m_runAheadLowerPeak;
		}
		return oldRunAhead;
	}
	m_runAheadLowerCount = 0;
	return askedRunAhead;
}

void ConnectionManager::noteRunAheadApplied(Int oldRunAhead, Int newRunAhead) {
	if (oldRunAhead == newRunAhead) {
		return;
	}
	m_runAheadStillInitial = FALSE;
}
#endif

// The average of the two highest in-band round trips, our own slot carries the one to our slowest peer.
Real ConnectionManager::getMaximumLatency()
{
	Real lat1 = 0.0f;
	Real lat2 = 0.0f;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (!isPlayerConnected(i))
		{
			continue;
		}
		if (m_latencyAverages[i] > lat1)
		{
			lat2 = lat1;
			lat1 = m_latencyAverages[i];
		}
		else if (m_latencyAverages[i] > lat2)
		{
			lat2 = m_latencyAverages[i];
		}
	}
	return (lat1 + lat2) / 2.0f;
}

void ConnectionManager::getMinimumFps(Int &minFps, Int &minFpsPlayer) {
	minFps = -1;
	minFpsPlayer = -1;
//	DEBUG_LOG_RAW(("ConnectionManager::getMinimumFps -"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_connections[i] != nullptr) || (i == m_localSlot)) {
//			DEBUG_LOG_RAW((" %d: %d,", i, m_fpsAverages[i]));
			if (m_fpsAverages[i] != -1) {
				if ((minFps == -1) || (m_fpsAverages[i] < minFps)) {
					minFps = m_fpsAverages[i];
					minFpsPlayer = i;
				}
			}
		}
	}
//	DEBUG_LOG_RAW(("\n"));
}

UnsignedInt ConnectionManager::getMinimumCushion() {
	return m_frameMetrics.getMinimumCushion();
}

/**
 * The commands for the given frame are all ready, time to send out our command count for that frame.
 */
void ConnectionManager::processFrameTick(UnsignedInt frame) {
	if ((m_frameData[m_localSlot] == nullptr) || (m_frameData[m_localSlot]->getIsQuitting() == TRUE)) {
		// if the local frame data stuff is null, we must be leaving the game.
		return;
	}
	UnsignedShort commandCount = m_frameData[m_localSlot]->getCommandCount(frame);
	NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
	msg->setExecutionFrame(frame);
	msg->setCommandCount(commandCount);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setPlayerID(m_localSlot);

	m_frameMetrics.doPerFrameMetrics(frame);

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::processFrameTick - sending frame info for frame %d, ID %d, command count %d", frame, msg->getID(), commandCount));

#if defined(GENERALS_ONLINE)
	signLocalFrame(msg);
#endif

	sendLocalCommand(msg, 0xff & ~(1 << m_localSlot));
#if defined(GENERALS_ONLINE)
	m_lastFrameInfoSent = frame;
#endif

	msg->detach();
}

/**
 * Set the local address.
 */
void ConnectionManager::setLocalAddress(UnsignedInt ip, UnsignedInt port) {
	DEBUG_LOG(("ConnectionManager::setLocalAddress() - local address is %X:%d", ip, port));
	m_localAddr = ip;
	m_localPort = port;
}

/**
 * Initialize the transport object
 */
void ConnectionManager::initTransport() {
	DEBUG_ASSERTCRASH((m_transport == nullptr), ("m_transport already exists when trying to init it."));
	DEBUG_LOG(("ConnectionManager::initTransport - Initializing Transport"));

	delete m_transport;

#if defined(GENERALS_ONLINE)
	// support lan + our new transport
	if (TheLAN == nullptr)
	{
		m_transport = new NextGenTransport;
	}
	else
	{
		m_transport = new UDPTransport;
	}
#else
	m_transport = new UDPTransport;
#endif
	m_transport->reset();
	m_transport->init(m_localAddr, m_localPort);
}

/**
 * This is where the commands from the local client are sent to the other clients in
 * the game.  This is also where the local commands are put into the frame data for
 * future execution.
 */
void ConnectionManager::sendLocalGameMessage(GameMessage *msg, UnsignedInt frame) {
	UnsignedShort currentID = 0;
	if (DoesCommandRequireACommandID(NETCOMMANDTYPE_GAMECOMMAND)) {
		currentID = GenerateNextCommandID();
	}

	NetCommandMsg *netmsg = newInstance(NetGameCommandMsg)(msg);
	netmsg->setExecutionFrame(frame);
	netmsg->setPlayerID(m_localSlot);
	netmsg->setID(currentID);

	sendLocalCommand(netmsg);

	netmsg->detach();
}

/**
 * This is a NetCommandMsg that originated on the local computer. Send this to everyone specified
 * in the relay field.  Commands sent in this way go through the packet router.
 */
void ConnectionManager::sendLocalCommand(NetCommandMsg *msg, UnsignedByte relay /* = 0xff by default*/) {
	if (CommandRequiresDirectSend(msg) || (m_packetRouterSlot < 0) || (m_packetRouterSlot >= MAX_SLOTS) || (m_connections[m_packetRouterSlot] == nullptr)) {
		sendLocalCommandDirect(msg, relay);
		return;
	}
	msg->attach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - sending net command %d of type %s", msg->getID(),
		GetNetCommandTypeAsString(msg->getNetCommandType())));

	if (relay & (1 << m_localSlot)) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommand - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
		m_frameData[m_localSlot]->addNetCommandMsg(msg);
	}

	// Send the packet to everyone else
#if defined(GENERALS_ONLINE)
	const Bool sendToAll = isDirectSendActive() || (m_localSlot == m_packetRouterSlot);
#else
	const Bool sendToAll = (m_localSlot == m_packetRouterSlot);
#endif
	if (sendToAll) {
		// I am the packet router or sending directly, I need to send this packet to everyone individually.
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			// Send it to all open connections.
			if (((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) && (relay & (1 << i))) {
				// Set the relay mask to only go to this player so he knows not to relay it to anyone else.
				UnsignedByte temprelay = 1 << i;
				m_connections[i]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.
			}
		}
	} else {
		// Send the command to everyone else via the packet router.
		UnsignedByte temprelay = relay & ~(1 << m_localSlot);	// Tell the packet router to relay the message to everyone but myself.
														// Hopefully the packet router is smart enough to not send it
														// to slots that are not in the game.

		m_connections[m_packetRouterSlot]->sendNetCommandMsg(msg, temprelay); // This will create a new copy of netmsg for this connection.

		if (CommandRequiresAck(msg)) {
			NetCommandRef *ref = m_pendingCommands->addMessage(msg);
			//DEBUG_LOG(("ConnectionManager::sendLocalCommand - added command %d to pending commands list.", msg->getID()));
			if (ref != nullptr) {
				ref->setRelay(temprelay);
			}
		}
	}

	msg->detach(); // detach from the command msg.
}

/**
 * This is a NetCommandMsg that originated on the local computer.  Send this to everyone specified
 * in the relay field.  Commands sent in this way do not go through the packet router.
 */
void ConnectionManager::sendLocalCommandDirect(NetCommandMsg *msg, UnsignedByte relay) {
	msg->attach();

	if (((relay & (1 << m_localSlot)) != 0) && (m_frameData[m_localSlot] != nullptr)) {
		if (IsCommandSynchronized(msg->getNetCommandType()) == TRUE) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - adding net command of type %s to player %d for frame %d", GetNetCommandTypeAsString(msg->getNetCommandType()), msg->getPlayerID(), msg->getExecutionFrame()));
			m_frameData[m_localSlot]->addNetCommandMsg(msg);
		}
	}

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((relay & (1 << i)) != 0) {
			if ((m_connections[i] != nullptr) && (m_connections[i]->isQuitting() == FALSE)) {
				UnsignedByte temprelay = 1 << i;
				m_connections[i]->sendNetCommandMsg(msg, temprelay);
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendLocalCommandDirect - Sending direct command %d of type %s to player %d", msg->getID(), GetNetCommandTypeAsString(msg->getNetCommandType()), i));
			}
		}
	}

	msg->detach();
}

Int commandsReadyDebugSpewage = 0;

/**
 * Returns true if all the commands for the given frame are ready to be executed.
 */
Bool ConnectionManager::allCommandsReady(UnsignedInt frame, Bool justTesting /* = FALSE */) {
	Bool retval = TRUE;
	FrameDataReturnType frameRetVal = FRAMEDATA_NOTREADY;
//	retval = FALSE;  // ****for testing purposes only!!!!!!****
	Int i = 0;
	for (; (i < MAX_SLOTS) && retval; ++i) {
		if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == FALSE)) {
/*
			if (!(m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE)))) {
				if ((frame != commandsReadyDebugSpewage) && (justTesting == FALSE)) {
					DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d not ready.", frame, i));
					commandsReadyDebugSpewage = frame;
				}
				retval = FALSE;
			} else {
//				DEBUG_LOG(("ConnectionManager::allCommandsReady, frame %d player %d is ready.", frame, i));
			}
*/

			frameRetVal = m_frameData[i]->allCommandsReady(frame, (frame != commandsReadyDebugSpewage) && (justTesting == FALSE));
			if (frameRetVal == FRAMEDATA_NOTREADY) {
				retval = FALSE;
#if !RETAIL_COMPATIBLE_NETWORKING
				if ((justTesting == FALSE) && (i != (Int)m_localSlot)) {
					askForOutPlayerFrame(i, frame);
				}
#endif
			} else if (frameRetVal == FRAMEDATA_RESEND) {
				requestFrameDataResend(i, frame);
				retval = FALSE;
			}
#if defined(GENERALS_ONLINE)
			else if (i != (Int)m_localSlot && !isPassedOnFrameAuthentic(i, frame)) {
				retval = FALSE;
			}
#endif
		}
	}

	if (frameRetVal == FRAMEDATA_RESEND) {
		// this frame's data is really screwed up, we need to clean it out so it can be resent to us.
		for (i = 0; i < MAX_SLOTS; ++i) {
			if ((m_frameData[i] != nullptr) && (i != m_localSlot)) {
				m_frameData[i]->resetFrame(frame, FALSE);
#if defined(GENERALS_ONLINE)
				// the resent copy is judged by where it comes from, a signature that came already stays
				FrameSignature *entry = findFrameSignature(i, frame);
				if (entry != nullptr) {
					entry->relayed = FALSE;
					entry->verified = FALSE;
					entry->waitingSince = 0;
				}
#endif
			}
		}
	}

	if ((retval == TRUE) && (justTesting == FALSE)) {
		m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this);
		retval = m_disconnectManager->allowedToContinue(); // allow the disconnect manager to keep us on this frame
																											// in case we are waiting for a new packet router or something.
	}

	return retval;
}

void ConnectionManager::handleAllCommandsReady()
{
	m_disconnectManager->allCommandsReady(TheGameLogic->getFrame(), this, FALSE);
}


/**
 * Only call this after making sure that all the commands are there for this frame.
 * After calling this the commands for this frame will be removed from the connection.
 *
 * BGC - To account for the case where the host disconnects without sending the
 *       same commands to all players, we now have to keep around the last 'run ahead'
 *       frames so we can potentially send those commands to the other players in the
 *       game so they can catch up.
 */
NetCommandList *ConnectionManager::getFrameCommandList(UnsignedInt frame)
{
	NetCommandList *retlist = newInstance(NetCommandList);
	retlist->init();

	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_frameData[i] != nullptr) {
			retlist->appendList(m_frameData[i]->getFrameCommandList(frame));
			if (frame > FRAMES_TO_KEEP) {
				m_frameData[i]->resetFrame(frame - FRAMES_TO_KEEP);	// After getting the commands for that frame from this
													// FrameDataManager object, we need to tell it that we're
													// done with the messages for that frame.
				DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("getFrameCommandList - called reset frame on player %d for frame %d", i, frame - FRAMES_TO_KEEP));
			}
		}
	}

	return retlist; // retlist deallocated by calling function.
}

void ConnectionManager::setFrameGrouping(time_t frameGrouping) {
	// Since we are the packet router, we should send more packets per second since we
	// may become the latency bottleneck for sending packets from one player to the next.
	// This is probably ok since the packet router should have the fastest connection of all
	// the players in the game.

	NetworkLog(ELogVerbosity::LOG_DEBUG, "ConnectionManager::setFrameGrouping Frame grouping is %I64d, is packet router %d", (Int64)frameGrouping, m_localSlot == m_packetRouterSlot);

	if (m_localSlot == m_packetRouterSlot) {
		frameGrouping = frameGrouping / 2;
	}
#if defined(GENERALS_ONLINE)
	m_frameGroupingMs = (Int)frameGrouping;
#endif
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_connections[i]->setFrameGrouping(frameGrouping);
		}
	}
}

/*
void ConnectionManager::determineRouterFallbackPlan() {
	memset(m_packetRouterFallback, 0, sizeof(m_packetRouterFallback));
	Int curnum = 1;
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			m_packetRouterFallback[i] = curnum;
			if (curnum == 1) {
				m_packetRouterSlot = i;
			}
			++curnum;
		}
	}
}
*/

void ConnectionManager::doKeepAlive() {
	static Int nextIndex = 0;
	static time_t startTime = 0;

	time_t curTime = timeGetTime();

	if (startTime == 0) {
		startTime = curTime;
		return;
	}

	time_t numSeconds = (curTime - startTime) / 1000;

	while ((nextIndex <= numSeconds) && (nextIndex < MAX_SLOTS)) {
//		DEBUG_LOG(("ConnectionManager::doKeepAlive - trying to send keep alive message to player %d", nextIndex));
		if (m_connections[nextIndex] != nullptr) {
			NetKeepAliveCommandMsg *msg = newInstance(NetKeepAliveCommandMsg);
			msg->setPlayerID(m_localSlot);
			if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
				msg->setID(GenerateNextCommandID());
			}
//			DEBUG_LOG(("ConnectionManager::doKeepAlive - sending keep alive message to player %d", nextIndex));
			sendLocalCommandDirect(msg, 1 << nextIndex);
			msg->detach();
		}
		++nextIndex;
	}
	if (nextIndex == MAX_SLOTS) {
		nextIndex = 0;
		startTime = curTime;
	}
}

PlayerLeaveCode ConnectionManager::disconnectPlayer(Int slot) {
	// Need to do the deletion of the slot's connection and frame data here.
	PlayerLeaveCode retval = PLAYERLEAVECODE_CLIENT;
	DEBUG_LOG(("ConnectionManager::disconnectPlayer - disconnecting slot %d on frame %d", slot, TheGameLogic->getFrame()));

	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return PLAYERLEAVECODE_UNKNOWN;
	}

	if (TheGameInfo)
	{
		GameSlot *gSlot = TheGameInfo->getSlot( slot );
		if (gSlot && !gSlot->lastFrameInGame())
		{
			DEBUG_LOG(("ConnectionManager::disconnectPlayer(%d) - slot is last in the game on frame %d",
				slot, TheGameLogic->getFrame()));
			gSlot->setLastFrameInGame(TheGameLogic->getFrame());
		}
	}

	UnicodeString unicodeName;
	unicodeName = getPlayerName(slot);
	if (!unicodeName.isEmpty() && m_connections[slot]) {
		TheInGameUI->message("Network:PlayerLeftGame", unicodeName.str());

		// People are boneheads. Also play a sound
		static AudioEventRTS leftGameSound("GUIMessageReceived");
		TheAudio->addAudioEvent(&leftGameSound);
	}

	if ((m_frameData[slot] != nullptr) && (m_frameData[slot]->getIsQuitting() == FALSE)) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d frame data", slot));
		deleteInstance(m_frameData[slot]);
		m_frameData[slot] = nullptr;
	}

	if (m_connections[slot] != nullptr && !m_connections[slot]->isQuitting()) {
		DEBUG_LOG(("ConnectionManager::disconnectPlayer - deleting player %d connection", slot));
		deleteInstance(m_connections[slot]);
		m_connections[slot] = nullptr;
	}

//	if (playerID == m_localSlot) {
//		TheMessageStream->appendMessage(GameMessage::MSG_CLEAR_GAME_DATA);
//	}

	if (slot == m_packetRouterSlot) {
#if defined(GENERALS_ONLINE)
		m_formerRouterMask |= (1u << m_packetRouterSlot);
#endif
		const UnsignedInt nextRouter = getNextPacketRouterSlot(m_packetRouterSlot);
		if (nextRouter < (UnsignedInt)MAX_SLOTS) {
			m_packetRouterSlot = nextRouter;
		} else {
			DEBUG_LOG(("ConnectionManager::disconnectPlayer - packet router had no valid fallback, defaulting to local slot %d", m_localSlot));
			m_packetRouterSlot = m_localSlot;
		}
		DEBUG_LOG(("Packet router left.  New packet router is slot %d", m_packetRouterSlot));
		retval = PLAYERLEAVECODE_PACKETROUTER;
	}
	if (m_localSlot == slot) {
		DEBUG_LOG(("Disconnecting self"));
		retval = PLAYERLEAVECODE_LOCAL;
	}

	// Take the player out of the fallback plan
	Int fallbackindex = 0;
	while ((fallbackindex < MAX_SLOTS) && (m_packetRouterFallback[fallbackindex] != slot)) {
		++fallbackindex;
	}

	for (Int i = fallbackindex; i < MAX_SLOTS-1; ++i) {
		m_packetRouterFallback[i] = m_packetRouterFallback[i+1];
	}
	m_packetRouterFallback[MAX_SLOTS-1] = -1;

	return retval;
}

#if defined(GENERALS_ONLINE)
void ConnectionManager::playerClosedGracefully(Int64 userID)
{
	Int slot = slotForUserID(userID);
	if (slot >= 0 && slot < MAX_SLOTS && m_disconnectManager != nullptr)
	{
		m_disconnectManager->playerClosedGracefully(slot, this);
	}
}

PlayerLeaveCode ConnectionManager::disconnectPlayer(int64_t userID)
{
	// a player whose connection ended before the load finished must not hold the others at the load screen, the disconnect handling removes it once the match runs
	const Int slot = slotForUserID(userID);
	if (slot >= 0 && slot != (Int)m_localSlot && TheGameLogic != nullptr && TheGameLogic->isInInternetGame() && !TheGameLogic->isProgressComplete()) {
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[LOAD] slot %d is gone before the load finished, not waiting for it", (Int)slot);
		TheGameLogic->processProgressComplete(slot);
	}
	return PLAYERLEAVECODE_UNKNOWN;
// 	if (TheNGMPGame != nullptr)
// 	{
// 		for (int slot = 0; slot < MAX_SLOTS; ++slot)
// 		{
// 			NGMPGameSlot* pSlot = (NGMPGameSlot*)TheNGMPGame->getSlot(slot);
// 			if (pSlot)
// 			{
// 				if (pSlot->m_userID == userID)
// 				{
// 					return disconnectPlayer(slot);
// 				}
// 			}
// 		}
// 	}
// 
// 	return PLAYERLEAVECODE_UNKNOWN;
}
#endif

void ConnectionManager::quitGame() {
	// Need to do the NetDisconnectPlayerCommandMsg creation and sending here.
	NetDisconnectPlayerCommandMsg *disconnectMsg = newInstance(NetDisconnectPlayerCommandMsg);
	disconnectMsg->setDisconnectSlot(m_localSlot);
	disconnectMsg->setDisconnectFrame(TheGameLogic->getFrame());
	disconnectMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(disconnectMsg->getNetCommandType())) {
		disconnectMsg->setID(GenerateNextCommandID());
	}
#if defined(GENERALS_ONLINE)
	signLeaveNotice(disconnectMsg); // lets the peers pass the notice on
#endif
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to send disconnect command"));
	sendLocalCommandDirect(disconnectMsg, 0xff ^ (1 << m_localSlot));

	// the connections are deleted right after, so send now
	setFrameGrouping(0);

	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - about to flush connections"));
	flushConnections(); // need to do this so our packet actually gets sent before the connections are deleted.
	//DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer - done flushing connections"));

	disconnectMsg->detach();

#if RTS_GENERALS
	// if we get here, we hit Quit on the disconnect screen.  Mark everyone as having disconnected from us
	// so the online stats can give us appropriate feedback.
	if (TheGameInfo)
	{
		for (Int i = 0; i < MAX_SLOTS; ++i)
		{
			GameSlot *gSlot = TheGameInfo->getSlot( i );
			if (gSlot && !gSlot->lastFrameInGame())
			{
				gSlot->markAsDisconnected();
			}
		}
	}
#endif

	disconnectLocalPlayer();
}

void ConnectionManager::disconnectLocalPlayer() {
	// kill the frame data and the connections for all the other players.
	DEBUG_LOG(("ConnectionManager::disconnectLocalPlayer()"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (i != m_localSlot) {
			disconnectPlayer(i);
		}
	}
}

/**
 * Takes all the commands that are ready to send and sends them right now.
 */
void ConnectionManager::flushConnections() {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
//			DEBUG_LOG(("ConnectionManager::flushConnections - flushing connection to player %d", i));
			/*
			if (m_connections[i]->isQueueEmpty()) {
//				DEBUG_LOG(("ConnectionManager::flushConnections - connection queue empty"));
			}
			*/
			m_connections[i]->doSend();
		}
	}

	if (m_transport != nullptr) {
		m_transport->doSend();
	}
}

void ConnectionManager::resendPendingCommands() {
	//DEBUG_LOG(("ConnectionManager::resendPendingCommands()"));
	if (m_pendingCommands == nullptr) {
		return;
	}

	NetCommandRef *ref = m_pendingCommands->getFirstMessage();
	while (ref != nullptr) {
		//DEBUG_LOG(("ConnectionManager::resendPendingCommands - resending command %d", ref->getCommand()->getID()));
		sendLocalCommand(ref->getCommand(), ref->getRelay());
		ref = ref->getNext();
	}
}

UnsignedInt ConnectionManager::getLocalPlayerID() {
	return m_localSlot;
}

UnicodeString ConnectionManager::getPlayerName(Int playerNum) {
	UnicodeString retval;
	if( playerNum == m_localSlot ) {
		retval = m_localUser->GetName();
	}	else if (((m_connections[playerNum] != nullptr) && (m_connections[playerNum]->isQuitting() == FALSE))) {
		retval = m_connections[playerNum]->getUser()->GetName();
	}
	return retval;
}

/**
 * Take a user list and make connections and frame data manager objects for each of the players.
 * For now, this is also how we'll determine the packet router fallback plan.
 */
void ConnectionManager::parseUserList(const GameInfo *game)
{
	if (!game)
		return;

	Int i;
	Int numUsers = 0;
	m_localSlot = -1;
	DEBUG_LOG(("Local slot is %d", game->getLocalSlotNum()));
	for (i=0; i<MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);	// badness, but since we cast right back to const, we should be ok
		if (slot->isHuman())
		{
			if (game->getLocalSlotNum() == i)
			{
				m_localSlot = i;
   			m_localUser->setName(slot->getName());
			}

			if (m_localSlot != i)
			{
				m_connections[i] = newInstance(Connection)();
				m_connections[i]->init();
#if defined(GENERALS_ONLINE)
				m_connections[i]->setNetworkSettings(m_networkSettings, TheLAN != nullptr || m_networkSettings.send.IsDatagram());
#endif
				m_connections[i]->setSlot(i);
				m_connections[i]->attachTransport(m_transport);
//				UnsignedShort port = (TheNAT)?TheNAT->getSlotPort(i):8088;
				UnsignedShort port = slot->getPort();
				m_connections[i]->setUser(newInstance(User)(slot->getName(), slot->getIP(), port));
				m_frameData[i] = newInstance(FrameDataManager)(FALSE);
				DEBUG_LOG(("Remote user is at %X:%d", slot->getIP(), slot->getPort()));
			}
			else
			{
				DEBUG_LOG(("Local user is %d (%X:%d)", m_localSlot, slot->getIP(), slot->getPort()));
				m_frameData[i] = newInstance(FrameDataManager)(TRUE);
			}
			m_frameData[i]->init();
			m_frameData[i]->reset();

			m_packetRouterFallback[numUsers] = i;

			++numUsers;

		}
	}
#ifdef MEMORYPOOL_DEBUG
	TheMemoryPoolFactory->debugSetInitFillerIndex(m_localSlot);
#endif

	// Set the packet router slot to the first player (packet router fallback[0])
	// This fixes the issue where m_packetRouterSlot was hardcoded to 0 and never updated
	if (numUsers > 0) {
		m_packetRouterSlot = m_packetRouterFallback[0];
		DEBUG_LOG(("Packet router slot set to %d", m_packetRouterSlot));
	}

	/*
	if ( numUsers < 2 || m_localSlot == -1 )
	{
		DEBUG_CRASH(("FAILED parseUserList - network game won't work as expected"));
		return;
	}

	char * list = strdup(buf);
	char *listPtr = list;
	if (!list)
		return;

	User users[MAX_SLOTS];
	int localUser = -1;
	int i;

	for (i=0; i<MAX_SLOTS; i++)
	{
		users[i].setName("");
	}

	char *userStr, *nameStr, *addrStr, *portStr;
	AsciiString addrAsciiStr;

	char *listPos;

	DEBUG_LOG(("ConnectionManager::parseUserList - looking for local user at %d.%d.%d.%d:%d",
		PRINTF_IP_AS_4_INTS(m_localAddr),
		m_localPort));

	int numUsers = 0;
	while ( (userStr=strtok_r(listPtr, ",", &listPos)) != nullptr )
	{
		listPtr = nullptr;
		char *pos = nullptr;

		nameStr = strtok_r(userStr, "@", &pos);
		addrStr = strtok_r(nullptr, "@:", &pos);
		portStr = strtok_r(nullptr, ": ", &pos);

		if (!portStr || numUsers >= MAX_SLOTS)
		{
			DEBUG_LOG(("ConnectionManager::parseUserList - (numUsers = %d) FAILED parseUserList with list [%s]", numUsers, buf));
			return;
		}

		addrAsciiStr = addrStr;
		UnsignedInt addr = ResolveIP(addrAsciiStr);
		UnsignedInt port = atoi(portStr);

//		if ((m_localAddr != addr) || (m_localPort != port)) {
		if (loginName.compare(nameStr) != 0) {
			m_connections[numUsers] = newInstance(Connection)();
			m_connections[numUsers]->init();
			m_connections[numUsers]->attachTransport(m_transport);
			m_connections[numUsers]->setUser(newInstance(User)(nameStr, addr, port));

			m_frameData[numUsers] = newInstance(FrameDataManager)(FALSE);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
		} else {
			m_localSlot = numUsers;
			m_localUser.setName(nameStr);

			DEBUG_LOG(("ConnectionManager::parseUserList - User %d is %s", numUsers, nameStr));
			DEBUG_LOG(("Local user is %d", m_localSlot));

			m_frameData[numUsers] = newInstance(FrameDataManager)(TRUE);
		}
		m_frameData[numUsers]->init();
		m_frameData[numUsers]->reset();

		m_packetRouterFallback[numUsers] = numUsers;

		numUsers++;
	}

	if (numUsers < 2 || m_localSlot == -1)
	{
		DEBUG_LOG(("ConnectionManager::parseUserList - FAILED (local user = %d, num players = %d) with list [%s]", m_localSlot, numUsers, buf));
		return;
	}

	free(list); // from the strdup above.
	list = nullptr;
	*/
}

/**
 * Return the number of incoming bytes per second averaged over 30 sec.
 */
Real ConnectionManager::getIncomingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of incoming packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getIncomingPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getIncomingPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing bytes per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingBytesPerSecond()
{
	if (m_transport)
		return m_transport->getOutgoingBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number of outgoing packets per second averaged over the last 30 sec.
 */
Real ConnectionManager::getOutgoingPacketsPerSecond()
{
	if (m_transport) {
		return m_transport->getOutgoingPacketsPerSecond();
	} else {
	  return 0.0;
	}
}

/**
 * Return the number of bytes not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownBytesPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownBytesPerSecond();
	else
	  return 0.0;
}

/**
 * Return the number ov packets not from generals clients received per second averaged over the last 30 sec.
 */
Real ConnectionManager::getUnknownPacketsPerSecond()
{
	if (m_transport)
		return m_transport->getUnknownPacketsPerSecond();
	else
	  return 0.0;
}

/**
 * Return the smallest packet arrival cushion since this was last called.
 */
UnsignedInt ConnectionManager::getPacketArrivalCushion() {
	UnsignedInt retval = m_smallestPacketArrivalCushion;
	m_smallestPacketArrivalCushion = -1;
	return retval;
}

void ConnectionManager::sendChat(UnicodeString text, Int playerMask, UnsignedInt executionFrame)
{
	NetChatCommandMsg *msg = newInstance(NetChatCommandMsg);
	msg->setText(text);
	msg->setPlayerMask(playerMask);
	msg->setPlayerID(m_localSlot);
	msg->setID(0);
	msg->setExecutionFrame(executionFrame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType()))
	{
		msg->setID(GenerateNextCommandID());
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Chat message has ID of %d, mask of %8.8X, text of %ls", msg->getID(), msg->getPlayerMask(), msg->getText().str()));

	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));
#if defined(GENERALS_ONLINE)
	signLocalChat(msg);
#endif
	processChat(msg);

	msg->detach();
}

void ConnectionManager::sendDisconnectChat(UnicodeString text) {
	NetDisconnectChatCommandMsg *msg = newInstance(NetDisconnectChatCommandMsg);
	msg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}
	msg->setText(text);

	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	processDisconnectChat(msg);

	msg->detach();
}

UnsignedShort ConnectionManager::sendFileAnnounce(AsciiString path, UnsignedByte playerMask)
{
	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		UnicodeString uPath;
		uPath.translate(path);
		log.format(L"Not sending file '%s' to %X", uPath.str(), playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return 0;
	}

	theFile->close();

	Int announceMask = 0xff ^ (1 << m_localSlot);
	NetFileAnnounceCommandMsg *announceMsg = newInstance(NetFileAnnounceCommandMsg);
	announceMsg->setPlayerID(m_localSlot);
	if (DoesCommandRequireACommandID(announceMsg->getNetCommandType()) == TRUE) {
		announceMsg->setID(GenerateNextCommandID());
	}
	announceMsg->setRealFilename(path);
	announceMsg->setPlayerMask(playerMask);
	UnsignedShort fileID = GenerateNextCommandID();
	announceMsg->setFileID(fileID);
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFileAnnounce() - creating announce message with ID of %d from %d to mask %X for '%s' going to %X as command %d",
		announceMsg->getID(), announceMsg->getPlayerID(), announceMask, announceMsg->getRealFilename().str(),
		announceMsg->getPlayerMask(), announceMsg->getFileID()));

	processFileAnnounce(announceMsg); // set up things for the host

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file announce to %X", announceMask));
	sendLocalCommand(announceMsg, announceMask);
	announceMsg->detach();

	return fileID;
}

void ConnectionManager::sendFile(AsciiString path, UnsignedByte playerMask, UnsignedShort commandID)
{
	File *theFile = TheLocalFileSystem->openFile(path.str());
	if (!theFile || !theFile->size())
	{
		UnicodeString log;
		UnicodeString uPath;
		uPath.translate(path);
		log.format(L"Not sending file '%s' to %X", uPath.str(), playerMask);
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("%ls", log.str()));
		if (TheLAN)
			TheLAN->OnChat(L"sendFile", 0, log, LANAPI::LANCHAT_SYSTEM);
		return;
	}

	Int len = theFile->size();
	char *buf = theFile->readEntireAndClose();
	if (buf == nullptr)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Not sending file '%s' to %X, it could not be read", path.str(), playerMask));
		return;
	}
	NetCommandDataChunk rawDataChunk(buf, len);

	// compress Targas
#ifdef COMPRESS_TARGAS
	char *compressedBuf = nullptr;
	Int compressedLen = path.endsWith(".tga")?CompressionManager::getMaxCompressedSize(len, CompressionManager::getPreferredCompression()):0;
	Int compressedSize = 0;
	if (compressedLen)
		compressedSize = CompressionManager::compressData(CompressionManager::getPreferredCompression(),
		buf, len, compressedBuf, compressedLen);

	if (!compressedSize)
	{
		delete[] compressedBuf;
		compressedBuf = nullptr;
	}

	NetCommandDataChunk compressedDataChunk(compressedBuf, compressedSize);
#endif // COMPRESS_TARGAS

	NetFileCommandMsg *fileMsg = newInstance(NetFileCommandMsg);
	fileMsg->setPlayerID(m_localSlot);
	fileMsg->setID(commandID);
	fileMsg->setRealFilename(path);

#ifdef COMPRESS_TARGAS
	if (compressedBuf)
	{
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Compressed '%s' from %d to %d (%g%%) before transfer", path.str(), len, compressedSize,
			(Real)compressedSize/(Real)len*100.0f));
		fileMsg->setFileData(compressedDataChunk);
	}
	else
#endif // COMPRESS_TARGAS
	{
		fileMsg->setFileData(rawDataChunk);
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFile() - creating file message with ID of %d for '%s' going to %X from %d, size of %d",
		fileMsg->getID(), fileMsg->getRealFilename().str(), playerMask, fileMsg->getPlayerID(), fileMsg->getFileLength()));

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("Sending file: '%s', len %d, to %X", path.str(), len, playerMask));

	sendLocalCommand(fileMsg, playerMask);

	fileMsg->detach();
}

Int ConnectionManager::getFileTransferProgress(Int playerID, AsciiString path)
{
	FileCommandMap::iterator commandIt = s_fileCommandMap.begin();
	while (commandIt != s_fileCommandMap.end())
	{
		//DEBUG_LOG(("ConnectionManager::getFileTransferProgress(%s): looking at existing transfer of '%s'",
		//	path.str(), commandIt->second.str()));
		if (commandIt->second == path)
		{
			return s_fileProgressMap[playerID][commandIt->first];
		}
		++commandIt;
	}
	//DEBUG_LOG(("Falling back to 0, since we couldn't find the map"));
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::getFileTransferProgress: path %s not found",path.str()));
	return 0;
}


void ConnectionManager::voteForPlayerDisconnect(Int slot) {
	if (m_disconnectManager != nullptr) {
		m_disconnectManager->voteForPlayerDisconnect(slot, this);
	}
}

Int ConnectionManager::getNumPlayers()
{
	Int retval = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (isPlayerConnected(i)) {
			++retval;
		}
	}

	return retval;
}

void ConnectionManager::updateLoadProgress( Int progress )
{
	NetProgressCommandMsg *msg = newInstance(NetProgressCommandMsg);
	msg->setPercentage( progress );
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processProgress(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

void ConnectionManager::loadProgressComplete()
{
	NetLoadCompleteCommandMsg *msg = newInstance(NetLoadCompleteCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processLoadComplete(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

void ConnectionManager::sendTimeOutGameStart()
{
	NetTimeOutGameStartCommandMsg *msg = newInstance(NetTimeOutGameStartCommandMsg);
	msg->setPlayerID( m_localSlot );
	if (DoesCommandRequireACommandID(msg->getNetCommandType()) == TRUE) {
		msg->setID(GenerateNextCommandID());
	}
	processTimeOutGameStart(msg);
	sendLocalCommand(msg, 0xff ^ (1 << m_localSlot));

	msg->detach();
}

Bool ConnectionManager::isPacketRouter()
{
	return m_localSlot == m_packetRouterSlot;
}

Int ConnectionManager::getAverageFPS()
{
	return m_frameMetrics.getAverageFPS();
}

Int ConnectionManager::getSlotAverageFPS(Int slot) {
	if ((slot < 0) || (slot >= MAX_SLOTS)) {
		return -1;
	}
	if ((m_packetRouterSlot != m_localSlot) && (slot == m_localSlot)) {
		// our framerate data isn't valid for other players unless we are the
		// packet router, so don't fake someone out.
		return -1;
	}
	return m_fpsAverages[slot];
}

const ConnectionStats* ConnectionManager::getConnectionStats(Int slot) const {
	if ((slot < 0) || (slot >= MAX_SLOTS) || (m_connections[slot] == nullptr)) {
		return nullptr;
	}
	return &m_connections[slot]->getStats();
}

#if defined(RTS_DEBUG)
void ConnectionManager::debugPrintConnectionCommands() {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - begin commands"));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if (m_connections[i] != nullptr) {
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - commands for connection %d", i));
			m_connections[i]->debugPrintCommands();
		}
	}
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::debugPrintConnectionCommands - end commands"));
}
#endif

void ConnectionManager::notifyOthersOfCurrentFrame(Int frame) {
	NetDisconnectFrameCommandMsg *msg = newInstance(NetDisconnectFrameCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setDisconnectFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - sending disconnect frame of %d, command ID = %d", frame, msg->getID()));
	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(1 << m_localSlot);
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::notifyOthersOfCurrentFrame - start screen on debug stuff"));
#if defined(RTS_DEBUG)
	debugPrintConnectionCommands();
#endif
}

void ConnectionManager::notifyOthersOfNewFrame(UnsignedInt frame) {
	NetDisconnectScreenOffCommandMsg *msg = newInstance(NetDisconnectScreenOffCommandMsg);

	msg->setPlayerID(m_localSlot);
	msg->setNewFrame(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

	sendLocalCommandDirect(msg, 0xff ^ (1 << m_localSlot));
	NetCommandRef *ref = NEW_NETCOMMANDREF(msg);
	ref->setRelay(1 << m_localSlot);
	m_disconnectManager->processDisconnectCommand(ref, this);
	deleteInstance(ref);

	msg->detach();
}

// An id half the counter away from the player's own, so a passed on frame info never collides with its commands.
UnsignedShort ConnectionManager::generateImpersonatedCommandID(Int playerID, UnsignedInt frame) {
	return (UnsignedShort)(((m_lastSeenCommandID[playerID] + 0x8000) & 0xF000) | (frame & 0x0FFF));
}

void ConnectionManager::sendFrameDataToPlayer(UnsignedInt playerID, UnsignedInt startingFrame) {
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame data to player %d starting with frame %d", playerID, startingFrame));
	for (UnsignedInt frame = startingFrame; frame < TheGameLogic->getFrame(); ++frame) {
		sendSingleFrameToPlayer(playerID, frame);
	}
#if defined(GENERALS_ONLINE)
	// a peer that dropped a passed on frame asks for it while we have not run it yet: our own data for it is final
	if (startingFrame >= TheGameLogic->getFrame() && startingFrame <= m_lastFrameInfoSent) {
		sendSingleFrameToPlayer(playerID, startingFrame, TRUE);
	}
#endif
	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - done sending commands to player %d", playerID));
}

void ConnectionManager::sendSingleFrameToPlayer(UnsignedInt playerID, UnsignedInt frame, Bool ownOnly) {
	if (frame + FRAMES_TO_KEEP < TheGameLogic->getFrame()) {
		DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendSingleFrameToPlayer - player %d requested frame %d when we are on frame %d, this is too far in the past.", playerID, frame, TheGameLogic->getFrame()));
		return;
	}

	UnsignedByte relay = 1 << playerID;

#if !RETAIL_COMPATIBLE_NETWORKING
	const Bool isActingRouter = m_disconnectManager->isLocalPlayerNextPacketRouter(this);
#endif

	DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending data for frame %d", frame));
	for (Int i = 0; i < MAX_SLOTS; ++i) {
#if !RETAIL_COMPATIBLE_NETWORKING
		// Own data and that of a player that is out go from everyone, the rest only from the acting packet router.
		if ((isActingRouter == FALSE) && (i != (Int)m_localSlot) && (m_disconnectManager->isPlayerOutOfGame(i, this) == FALSE)) {
			continue;
		}
#endif
#if defined(GENERALS_ONLINE)
		// For a frame we have not run yet only our own data is final, and that of a player that is out once we hold all of it.
		if (ownOnly && (i != (Int)m_localSlot)) {
			Bool finalOfOutPlayer = FALSE;
#if !RETAIL_COMPATIBLE_NETWORKING
			finalOfOutPlayer = (m_frameData[i] != nullptr) && m_disconnectManager->isPlayerOutOfGame(i, this)
				&& (m_frameData[i]->getFrameCommandCount(frame) == m_frameData[i]->getCommandCount(frame));
#endif
			if (!finalOfOutPlayer) {
				continue;
			}
		}
#endif
		if ((m_frameData[i] != nullptr) && (i != playerID)) { // no need to send his own commands to him.
			NetCommandList *list = m_frameData[i]->getFrameCommandList(frame);
			if (list != nullptr) {
				NetCommandRef *ref = list->getFirstMessage();
				while (ref != nullptr) {
					DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending command %d from player %d to player %d using relay 0x%x", ref->getCommand()->getID(), i, playerID, relay));
					sendLocalCommandDirect(ref->getCommand(), relay);
					ref = ref->getNext();
				}
			}
			UnsignedInt frameCommandCount = m_frameData[i]->getFrameCommandCount(frame);
			NetFrameCommandMsg *msg = newInstance(NetFrameCommandMsg);
			msg->setExecutionFrame(frame);
			msg->setCommandCount(frameCommandCount);
			if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
				msg->setID(i == (Int)m_localSlot ? GenerateNextCommandID() : generateImpersonatedCommandID(i, frame));
			}
			msg->setPlayerID(i);
			DEBUG_LOG_LEVEL(DEBUG_LEVEL_NET, ("ConnectionManager::sendFrameDataToPlayer - sending frame info from player %d to player %d for frame %d with command count %d and ID %d and relay %d", i, playerID, msg->getExecutionFrame(), msg->getCommandCount(), msg->getID(), relay));
#if defined(GENERALS_ONLINE)
			if (i != (Int)m_localSlot) {
				attachStoredSignature(i, msg);
			}
#endif
			sendLocalCommandDirect(msg, relay);
			msg->detach();
		}
	}
}

UnsignedInt ConnectionManager::getNextPacketRouterSlot(UnsignedInt playerID) {
	Int index = 0;
	while ((index < MAX_SLOTS) && (m_packetRouterFallback[index] != playerID)) {
		++index;
	}
	++index;
	if (index < MAX_SLOTS) {
		return m_packetRouterFallback[index];
	}
	return MAX_SLOTS; // No valid next packet router; caller checks for >= MAX_SLOTS
}

void ConnectionManager::requestFrameDataResend(Int playerID, UnsignedInt frame) {
	NetFrameResendRequestCommandMsg *msg = newInstance(NetFrameResendRequestCommandMsg);
	msg->setPlayerID(m_localSlot);
	msg->setFrameToResend(frame);
	if (DoesCommandRequireACommandID(msg->getNetCommandType())) {
		msg->setID(GenerateNextCommandID());
	}

#if !RETAIL_COMPATIBLE_NETWORKING
	// A player that is out answers no more, so the others that may hold its frames are asked instead.
	if (m_disconnectManager->isPlayerOutOfGame(playerID, this) == TRUE) {
		sendLocalCommandDirect(msg, (UnsignedByte)(0xff & ~(1 << m_localSlot) & ~(1 << playerID)));
		msg->detach();
		return;
	}
#endif

	if (isPlayerConnected(playerID) == FALSE) {
		playerID = 0;
		while ((playerID < MAX_SLOTS) && (isPlayerConnected(playerID) == FALSE)) {
			++playerID;
		}
	}

	if (playerID < MAX_SLOTS) {
		sendLocalCommandDirect(msg, 1 << playerID);
	}

	msg->detach();
}

#if !RETAIL_COMPATIBLE_NETWORKING
void ConnectionManager::askForOutPlayerFrame(Int slot, UnsignedInt frame) {
	if (m_disconnectManager->isPlayerOutOfGame(slot, this) == FALSE) {
		return;
	}
	const UnsignedInt now = timeGetTime();
	if ((slot == m_outFrameAskedSlot) && (frame == m_outFrameAskedFrame) && ((now - m_outFrameAskedTime) < 1000)) {
		return;
	}
	m_outFrameAskedSlot = slot;
	m_outFrameAskedFrame = frame;
	m_outFrameAskedTime = now;
	requestFrameDataResend(slot, frame);
}

Int ConnectionManager::getSlotOfAddress(UnsignedInt addr, UnsignedShort port) {
	for (Int i = 0; i < MAX_SLOTS; ++i) {
		if ((m_connections[i] != nullptr) && (m_connections[i]->getUser()->GetIPAddr() == addr) && (m_connections[i]->getUser()->GetPort() == port)) {
			return i;
		}
	}
	return -1;
}
#endif

#if defined(GENERALS_ONLINE)
void ConnectionManager::noteStalling(Bool stalling, UnsignedInt frame) {
	time_t now = timeGetTime();

	if (stalling && !m_stallActive) {
		// list the senders whose commands for this frame are missing
		m_stallMissingMask = 0;
		for (Int i = 0; i < MAX_SLOTS; ++i) {
			if ((m_frameData[i] != nullptr) && (m_frameData[i]->getIsQuitting() == FALSE)
				&& (m_frameData[i]->getFrameCommandCount(frame) != m_frameData[i]->getCommandCount(frame))) {
				m_stallMissingMask |= (1u << i);
			}
		}
		m_stallActive = TRUE;
		m_stallStartTime = now;
	} else if (!stalling && m_stallActive) {
		m_stallActive = FALSE;
		m_stallStats.note((Int)(now - m_stallStartTime), m_stallMissingMask, now);
	}
}

Bool ConnectionManager::getOverlayInfo(Int slot, NetworkOverlayInfo &info) {
	time_t now = timeGetTime();
	m_stallStats.update(now);

	info = NetworkOverlayInfo();
	info.stalledNow = m_stallActive;
	info.runAhead = m_lastKnownRunAhead;
	info.frameGroupingMs = m_frameGroupingMs;

	if (slot >= 0 && slot < MAX_SLOTS) {
		info.stallsWindow = m_stallStats.getCountForSlot(slot, now, NETWORK_STATS_WINDOW_MS);
		info.stallsMinute = m_stallStats.getCountForSlot(slot, now, 60000);
		info.waitingOnSlot = m_stallActive && ((m_stallMissingMask & (1u << slot)) != 0);
		if (isPlayerConnected(slot)) {
			info.slotInBandMs = (Int)(m_latencyAverages[slot] * 1000.0f);
		}
		m_cushionStats[slot].update(now);
		info.cushionSamples = m_cushionStats[slot].getSampleCount();
		info.cushionP5 = m_cushionStats[slot].getPercentile(5);
		info.cushionP50 = m_cushionStats[slot].getPercentile(50);
	}
	return TRUE;
}
#endif
