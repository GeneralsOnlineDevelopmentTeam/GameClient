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

/**
 * The connection manager maintains a collection of Connections to each player.  Messages are queued up
 * from here, as well as removed through here.
 */

#pragma once

#include "GameNetwork/Connection.h"
#include "GameNetwork/NetCommandList.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/UDPTransport.h"
#include "GameNetwork/FrameDataManager.h"
#include "GameNetwork/FrameMetrics.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/DisconnectManager.h"
#if defined(GENERALS_ONLINE)
#include "GameNetwork/NetworkSettings.h"
#endif

class GameInfo;
class NetCommandWrapperList;

typedef std::map<UnsignedShort, AsciiString> FileCommandMap;
typedef std::map<UnsignedShort, UnsignedByte> FileMaskMap;
typedef std::map<UnsignedShort, Int> FileProgressMap;

class ConnectionManager
{
public:
	ConnectionManager();
	~ConnectionManager();

	virtual void init();				///< Initialize this instance.
	virtual void reset();				///< Take this instance back to the initial state.
	virtual void update(Bool isInGame);			///< Service the Connections being managed by this instance.

#if defined(GENERALS_ONLINE)
	const NetworkSettings &getNetworkSettings() const { return m_networkSettings; }
	void SeedLatencyData(int highestLatency)
	{
		for (int i = 0; i < MAX_SLOTS; ++i) {
			m_fpsAverages[i] = GENERALS_ONLINE_HIGH_FPS_LIMIT;
		}
		for (int i = 0; i < MAX_SLOTS; ++i) {
			m_latencyAverages[i] = highestLatency / 1000.f;
		}

		m_frameMetrics.SeedLatencyData(highestLatency);
	}
#endif

	// End SubsystemInterface functions

	void updateRunAhead(Int oldRunAhead, Int frameRate, Bool didSelfSlug, Int nextExecutionFrame);	///< Update the run ahead value.  If we are the current packet router, issue the command.

	void attachTransport(Transport *transport);

	void parseUserList(const GameInfo *game);
	void sendChat(UnicodeString text, Int playerMask, UnsignedInt executionFrame);
	void sendDisconnectChat(UnicodeString text);
	void sendLocalCommand(NetCommandMsg *msg, UnsignedByte relay = 0xff);		///< Send command to the players specified in the relay, goes through packet router.
	void sendLocalCommandDirect(NetCommandMsg *msg, UnsignedByte relay);		///< Send command directly to the players specified, doesn't go through packet router.
	void sendLocalGameMessage(GameMessage *msg, UnsignedInt frame);
	void sendCommand(NetCommandMsg *msg);
	Bool allCommandsReady(UnsignedInt frame, Bool justTesting = FALSE);
	void handleAllCommandsReady();
	NetCommandList *getFrameCommandList(UnsignedInt frame);
//	void AddConnection(User *user, UnsignedInt slot);
	void determineRouterFallbackPlan();
	void zeroFrames(UnsignedInt startingFrame, UnsignedInt numFrames);
	void destroyGameMessages();
//	void createConnections(UnsignedInt numberOfPlayers, UnsignedInt localSlot);
	void setLocalAddress(UnsignedInt ip, UnsignedInt port);
	void initTransport();
	void processFrameTick(UnsignedInt frame);
	void handleLocalPlayerLeaving(UnsignedInt frame);

	void sendFile(AsciiString path, UnsignedByte playerMask, UnsignedShort commandID);
	UnsignedShort sendFileAnnounce(AsciiString path, UnsignedByte playerMask);
	Int getFileTransferProgress(Int playerID, AsciiString path);
	Bool areAllQueuesEmpty();

	UnsignedInt getLocalPlayerID();
	UnicodeString getPlayerName(Int playerNum);
	Int getNumPlayers();

	UnsignedInt getPacketRouterFallbackSlot(Int packetRouterNumber);	///< Returns the slot of the given packet router number in the fallback plan.
	UnsignedInt getPacketRouterSlot();																///< Returns the current packet router's slot.
	PlayerLeaveCode disconnectPlayer(Int slot);												///< Disconnect this player immediately.  This should only be called by the disconnect manager.
#if defined(GENERALS_ONLINE)
	void playerClosedGracefully(Int64 userID);
	PlayerLeaveCode disconnectPlayer(int64_t userID);												///< Disconnect this player immediately.  This should only be called by the disconnect manager.
#endif
	void disconnectLocalPlayer();																			///< Does whatever is necessary to get TheNetwork to realize that it should be leaving the game now.
	void quitGame();																									///< Disconnect from the game RIGHT NOW!!  Tell everyone else we are disconnecting.

	void voteForPlayerDisconnect(Int slot);														///< Register a vote for a player to be disconnected.

	void resendPendingCommands();																			///< Resend the pending commands to the packet router.

	void setFrameGrouping(time_t frameGrouping);				///< Set the number of frames that are grouped together into packets.

	PlayerLeaveCode processPlayerLeave(NetPlayerLeaveCommandMsg *msg);
	Bool canILeave();																///< Returns true if the local player is allowed to leave.

	// Bandwidth metrics
	Real getIncomingBytesPerSecond();
	Real getIncomingPacketsPerSecond();
	Real getOutgoingBytesPerSecond();
	Real getOutgoingPacketsPerSecond();
	Real getUnknownBytesPerSecond();
	Real getUnknownPacketsPerSecond();
	UnsignedInt getPacketArrivalCushion();

	UnsignedInt getMinimumCushion();

	void flushConnections();

	void processChat(NetChatCommandMsg *msg); // this actually needs to be public because it is frame-synchronized

	void updateLoadProgress( Int progress );
	void loadProgressComplete();
	void sendTimeOutGameStart();

	Bool isPacketRouter();

	Bool isPlayerConnected( Int playerID );

	void notifyOthersOfCurrentFrame(Int frame);
	void sendFrameDataToPlayer(UnsignedInt playerID, UnsignedInt startingFrame);
	void sendSingleFrameToPlayer(UnsignedInt playerID, UnsignedInt frame, Bool ownOnly = FALSE);
	void notifyOthersOfNewFrame(UnsignedInt frame);

	UnsignedInt getNextPacketRouterSlot(UnsignedInt playerID); ///< returns the packet router player that comes after the given player.

	Int getAverageFPS();
	Int getSlotAverageFPS(Int slot);
#if defined(GENERALS_ONLINE)
	void noteStalling(Bool stalling, UnsignedInt frame);	///< Called every network update with whether the game is waiting for commands.
#endif
	const ConnectionStats* getConnectionStats(Int slot) const;	///< Returns nullptr when there is no connection for the slot.

#if defined(RTS_DEBUG)
	void debugPrintConnectionCommands();
#endif

	// For disconnect blame assignment
	UnsignedInt getPingFrame();
	Int getPingsSent();
	Int getPingsReceived();

private:
#if defined(GENERALS_ONLINE)
	NetworkSettings m_networkSettings;
#endif

	void doRelay();
	void doKeepAlive();
	void sendRemoteCommand(NetCommandRef *msg);
	void ackCommand(NetCommandRef *ref, UnsignedInt localSlot);

	Bool processNetCommand(NetCommandRef *ref);
	void processAckStage1(NetCommandMsg *msg);
	void processAckStage2(NetCommandMsg *msg);
	void processAck(NetCommandMsg *msg);
	void processFrameInfo(NetFrameCommandMsg *msg);
	void processRunAheadMetrics(NetRunAheadMetricsCommandMsg *msg);
	void processDisconnectChat(NetDisconnectChatCommandMsg *msg);
	void processProgress( NetProgressCommandMsg *msg );
	void processLoadComplete( NetCommandMsg *msg );
	void processTimeOutGameStart( NetCommandMsg *msg );
	void processWrapper(NetCommandRef *ref);
	void processFrameResendRequest(NetFrameResendRequestCommandMsg *msg);

	void processFile(NetFileCommandMsg *ref);
	void processFileAnnounce(NetFileAnnounceCommandMsg *ref);
	void processFileProgress(NetFileProgressCommandMsg *ref);

	//	void doPerFrameMetrics(UnsignedInt frame);
	void getMinimumFps(Int &minFps, Int &minFpsPlayer);			///< Returns the smallest FPS in the m_fpsAverages list.
	Real getMaximumLatency(); ///< Returns the average of the two highest average latencies between players.

	void requestFrameDataResend(Int playerID, UnsignedInt frame); ///< request of this player that he send the specified frame's data.
	UnsignedShort generateImpersonatedCommandID(Int playerID, UnsignedInt frame);
#if !RETAIL_COMPATIBLE_NETWORKING
	void askForOutPlayerFrame(Int slot, UnsignedInt frame);
	Int getSlotOfAddress(UnsignedInt addr, UnsignedShort port);
#endif

	// The connections are set up like the slot list. The connection corresponding to the local
	// player's position in the slot list will be null.  Connections corresponding to slots that
	// do not have a player will also be null.
	Connection *m_connections[MAX_SLOTS];

	Transport *m_transport;
	UnsignedInt m_localSlot;
	UnsignedInt m_packetRouterSlot;
	UnsignedInt m_packetRouterFallback[MAX_SLOTS];
	UnsignedInt m_localAddr;
	UnsignedInt m_localPort;
	User* m_localUser;

	DisconnectManager *m_disconnectManager;						///< Controls the disconnect dialog.

	FrameDataManager *m_frameData[MAX_SLOTS];

	NetCommandList *m_pendingCommands;
	NetCommandList *m_relayedCommands;
	UnsignedShort m_lastSeenCommandID[MAX_SLOTS];
	Bool m_haveLastSeenCommandID[MAX_SLOTS];
#if !RETAIL_COMPATIBLE_NETWORKING
	Int m_currentSenderSlot;	///< The slot of the peer the packet being read came from, -1 when not known.
	Int m_outFrameAskedSlot;
	UnsignedInt m_outFrameAskedFrame;
	UnsignedInt m_outFrameAskedTime;
#endif

	FrameMetrics m_frameMetrics;

	NetCommandWrapperList *m_netCommandWrapperList;

	// These variables are used to keep track of the other players' average fps and latency.
	// yup.
	Real m_latencyAverages[MAX_SLOTS];
	Int  m_fpsAverages[MAX_SLOTS];
	Int  m_minFpsPlayer;
	Int  m_minFps;
	UnsignedInt m_smallestPacketArrivalCushion;
	Bool m_didSelfSlug;

#if defined(GENERALS_ONLINE)
	// the last command ids seen per sender, so a redundant copy of a command that is not synchronized is dropped
	enum { RECENT_ID_COUNT = 256 };
	struct RecentCommandIds
	{
		Bool valid;
		UnsignedShort highest;
		Bool seen[RECENT_ID_COUNT];
	};
	RecentCommandIds m_recentIds[MAX_SLOTS];

	Int m_runAheadLowerCount;
	Int m_runAheadLowerPeak;
	Bool isDuplicateCommand(NetCommandMsg *msg);

	Int slotForUserID(Int64 userID) const;
	Bool isPeerActive(Int slot) const;
	void onDirectFrameAck(UnsignedInt frame, Int peerSlot);
	std::map<UnsignedInt, UnsignedInt> m_frameAckedMasks;	///< Frame -> the peers that acked its frame info so far.
	Bool isDirectSendActive();
	Bool m_directSendActive;
	time_t m_directSendCheckedAt;
	UnsignedInt m_formerRouterMask;	///< Slots that were the packet router in this match, their last packets can still be on the way.
	Bool isCommandFromSender(NetCommandMsg *msg, Int senderSlot);
	Bool noteSecurityDrop();
	Bool m_loggedSenderUnavailable;
	Int m_securityDrops;
	void resetRecentCommandIds();

	// Frames with commands and chat that a peer passes on for another player need that player's signature.
	// Every peer keeps the signatures it gets as long as the frames, so whoever holds a frame holds its proof.
	enum { SIGNATURE_BYTES = 64, SIGNATURE_WAIT_MS = 3000, HELD_CHAT_COUNT = 8 };
	struct FrameSignature
	{
		Bool used;
		UnsignedInt frame;
		Bool relayed;			///< a command or the frame info arrived from a peer other than the author
		Bool haveSignature;
		Bool verified;
		Int relayedBy;
		time_t waitingSince;	///< when the frame was complete without a signature, 0 when not waiting
		UnsignedByte signature[SIGNATURE_BYTES];
	};
	struct HeldChat
	{
		NetChatCommandMsg *msg;
		Int relayedBy;
		time_t since;
	};
	struct ChatSignature
	{
		Bool used;
		UnsignedInt author;
		UnsignedShort commandID;
		time_t since;
		UnsignedByte signature[SIGNATURE_BYTES];
	};
	enum { RELAYED_WRAPPER_COUNT = 8 };
	struct RelayedWrapper
	{
		Bool used;
		UnsignedShort wrappedCommandID;
		Int relayedBy;
	};
	std::vector<FrameSignature> m_frameSignatures[MAX_SLOTS];	///< Indexed like the frame data, by frame modulo FRAME_DATA_LENGTH.
	HeldChat m_heldChats[HELD_CHAT_COUNT];
	ChatSignature m_chatSignatures[HELD_CHAT_COUNT];
	RelayedWrapper m_relayedWrappers[MAX_SLOTS][RELAYED_WRAPPER_COUNT];
	Int m_nextRelayedWrapper[MAX_SLOTS];
	Int64 m_signerUserID[MAX_SLOTS];	///< The user id per slot, kept after the player leaves to verify its last frames.
	UnsignedInt m_lastFrameInfoSent;	///< The last frame our frame info went out for, our data up to it is final.
	void clearSignatureState(Bool releaseChats);
	void requestPassedOnFrameAgain(Int slot, UnsignedInt frame, Int passedOnBy);
	FrameSignature *findFrameSignature(Int slot, UnsignedInt frame);
	FrameSignature &frameSignatureFor(Int slot, UnsignedInt frame);
	Int64 signerUserID(Int slot);
	void noteFramePassedOn(Int author, UnsignedInt frame, Int relayedBy, Bool changesFrame);
	void noteWrapperPassedOn(NetCommandMsg *wrapper, Int relayedBy);
	Int wrapperPassedOnBy(NetCommandMsg *msg);
	Bool buildSignedFrame(Int slot, UnsignedInt frame, std::vector<UnsignedByte> &out);
	Bool buildSignedCommand(Int slot, NetCommandMsg *msg, std::vector<UnsignedByte> &out);
	void signLocalFrame(NetFrameCommandMsg *frameInfo);
	void signLocalChat(NetChatCommandMsg *msg);
	Bool buildSignedLeave(Int slot, std::vector<UnsignedByte> &out);
public:
	Bool signLeaveNotice(NetDisconnectPlayerCommandMsg *msg);	///< The leaving player signs its leave notice.
	Bool verifyLeaveNotice(NetDisconnectPlayerCommandMsg *msg);	///< The notice carries the signature of the player it says left.
private:
	void attachStoredSignature(Int author, NetFrameCommandMsg *frameInfo);
	Bool isPassedOnFrameAuthentic(Int slot, UnsignedInt frame);
	void storeFrameSignature(Int author, UnsignedInt frame, const UnsignedByte *signature, Bool fromAuthor);
	void processSignatures(NetSignaturesCommandMsg *msg);
	Bool holdPassedOnChat(NetChatCommandMsg *msg);
	void releaseHeldChat(Int index, const UnsignedByte *signature);
	void expireHeldChats(time_t now);

	ArrivalCushionStats m_cushionStats[MAX_SLOTS];
	StallStats m_stallStats;
	Bool m_stallActive;
	time_t m_stallStartTime;
	UnsignedInt m_stallMissingMask;
	Int m_lastKnownRunAhead;
	Bool m_runAheadStillInitial;		///< No run ahead change has taken effect yet, the first decisions apply without the decrease hold.
	Int m_pendingRunAhead;				///< Router: the run ahead of the last command it issued.
	UnsignedInt m_pendingExecFrame;		///< Router: the execution frame of that command.
	Int applyDecreaseHold(Int oldRunAhead, Int askedRunAhead, Int decreaseAfter);
public:
	void noteRunAheadApplied(Int oldRunAhead, Int newRunAhead);	///< The run ahead command took effect on this frame.
private:

	Bool m_shortGrouping;				///< The run ahead is within 2 frames of its floor, connections send at one frame
	Int getRunAheadFloor() const;
	time_t getEffectiveDecisionIntervalMs(Int runAhead) const;	///< The longer of the configured floor and 2 x the run ahead in ms
	void updateShortGrouping();
	Int m_frameGroupingMs;
public:
	Bool getOverlayInfo(Int slot, NetworkOverlayInfo &info);	///< Fills what the overlay and the connection rating need about one slot.
protected:
#endif

	// -----------------------------------------------------------------------------
	FileCommandMap s_fileCommandMap;
	FileMaskMap s_fileRecipientMaskMap;
	FileProgressMap s_fileProgressMap[MAX_SLOTS];
	// -----------------------------------------------------------------------------
};
