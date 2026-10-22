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

#include "GameNetwork/Connection.h"
#include "GameNetwork/networkutil.h"
#include "GameLogic/GameLogic.h"
#include "../NGMP_include.h"
#if defined(GENERALS_ONLINE)
#include "../ngmp_interfaces.h"
#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include "../NetworkMesh.h"
#endif

#if defined(GENERALS_ONLINE)
enum { MaxQuitFlushTime = 5000 }; // wait this many milliseconds at most to retry things before quitting
#else
enum { MaxQuitFlushTime = 30000 }; // wait this many milliseconds at most to retry things before quitting
#endif

#if defined(GENERALS_ONLINE)
enum { MinScaledRetryTime = 50, MinAutoRedundancyWindow = 100, MaxRedundantCandidates = 128, MinLateAckMargin = 50, MaxScaledRetryTime = 1000, MaxFastRetryTime = 400, StartDepth = 2 };
enum { DataLossWindowMs = 3000, DataLossBurstRun = 2 }; // the peer's reports of the last 3 s, a lost run of 2 packets is a burst
#endif

/**
 * The constructor.
 */
Connection::Connection() {
	m_transport = nullptr;
	m_user = nullptr;
	m_netCommandList = nullptr;
	m_retryTime = 2000; // set retry time to 2 seconds.
	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_configuredFrameGrouping = 1;
	m_shortGrouping = FALSE;
	m_isQuitting = false;
	m_quitTime = 0;
	resetRedundancy();
	m_slot = -1;
#if defined(GENERALS_ONLINE)
	m_networkSettings = NetworkSettings::LocalDefaults();
	m_unreliableTransport = FALSE;
#endif
}

/**
 * The destructor.
 */
Connection::~Connection() {
	deleteInstance(m_user);
	m_user = nullptr;

	deleteInstance(m_netCommandList);
	m_netCommandList = nullptr;
}

/**
 * Initialize the connection and any subsystems.
 */
void Connection::init() {
	m_transport = nullptr;

	deleteInstance(m_user);
	m_user = nullptr;

	if (m_netCommandList == nullptr) {
		m_netCommandList = newInstance(NetCommandList);
		m_netCommandList->init();
	}
	m_netCommandList->reset();

	m_lastTimeSent = 0;
	m_frameGrouping = 1;
	m_configuredFrameGrouping = 1;
	m_shortGrouping = FALSE;
	m_stats.reset();
	resetRedundancy();
	m_isQuitting = FALSE;
	m_quitTime = 0;
}

/**
 * Take the connection back to the initial state.
 */
void Connection::reset() {
	init();
}

/**
 * Doesn't really do anything.
 */
void Connection::update() {
}

/**
 * Attach the transport object that this connection should use.
 */
void Connection::attachTransport(Transport *transport) {
	m_transport = transport;
}

/**
 * Assign this connection a user.  This is the user to whome we send all our packetized goodies.
 */
void Connection::setUser(User *user) {
	if (m_user != NULL) {
		deleteInstance(m_user);
	}

	m_user = user;
}

/**
 * Return the user object.
 */
User * Connection::getUser() {
	return m_user;
}

/**
 * Add this network command to the send queue for this connection.
 * The relay is the mask specifying the people the person we are sending to should send to.
 * The relay mostly has to do with the packet router.
 */
void Connection::sendNetCommandMsg(NetCommandMsg *msg, UnsignedByte relay) {
	static NetPacket *packet = nullptr;

	// this is done so we don't have to allocate and delete a packet every time we send a message.
	if (packet == nullptr) {
		packet = newInstance(NetPacket);
	}


	if (m_isQuitting)
		return;

#if defined(GENERALS_ONLINE)
	// a signed frame info goes out in its signed form
	if (msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO && ((NetFrameCommandMsg *)msg)->hasSignature()) {
		NetSignedFrameCommandMsg *signedMsg = newInstance(NetSignedFrameCommandMsg)((NetFrameCommandMsg *)msg);
		sendNetCommandMsg(signedMsg, relay);
		signedMsg->detach();
		return;
	}
#endif

	if (m_netCommandList != nullptr) {
		// check to see if this command will fit in a packet.  If not, we need to split it up.
		// we are splitting up the command here so that the retry logic will not try to
		// resend the ENTIRE command (i.e. multiple packets work of data) and only do the retry
		// one wrapper command at a time.
		packet->reset();

		NetCommandRef *tempref = NEW_NETCOMMANDREF(msg);
		tempref->setRelay(relay); // the relay byte takes room in the packet too

		Bool msgFits = packet->addCommand(tempref);
		deleteInstance(tempref); // delete the temporary reference.
		tempref = nullptr;

		if (!msgFits) {
			NetCommandRef *origref = NEW_NETCOMMANDREF(msg);
			origref->setRelay(relay);
			// the message doesn't fit in a single packet, need to split it up.
			NetPacketList packetList = NetPacket::ConstructBigCommandPacketList(origref);
			NetPacketListIter tempPacketPtr = packetList.begin();

			while (tempPacketPtr != packetList.end()) {
				NetPacket *tempPacket = (*tempPacketPtr);

				NetCommandList *list = tempPacket->getCommandList();
				NetCommandRef *ref1 = list->getFirstMessage();
				while (ref1 != nullptr) {
					NetCommandRef *ref2 = m_netCommandList->addMessage(ref1->getCommand());
					ref2->setRelay(relay);

					ref1 = ref1->getNext();
				}

				deleteInstance(tempPacket);
				tempPacket = nullptr;
				++tempPacketPtr;

				deleteInstance(list);
				list = nullptr;
			}

			deleteInstance(origref);
			origref = nullptr;

			return;
		}

		// the message fits in a packet, add to the command list normally.
		NetCommandRef *ref = m_netCommandList->addMessage(msg);

		if (ref != nullptr) {

/*
#if defined(RTS_DEBUG)
			if (msg->getNetCommandType() == NETCOMMANDTYPE_GAMECOMMAND) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added game command %d to net command list for frame %d.",
					msg->getID(), msg->getExecutionFrame()));
			} else if (msg->getNetCommandType() == NETCOMMANDTYPE_FRAMEINFO) {
				DEBUG_LOG(("Connection::sendNetCommandMsg - added frame info for frame %d", msg->getExecutionFrame()));
			}
#endif // RTS_DEBUG
*/

			ref->setRelay(relay);
		}
	}
}

void Connection::clearCommandsExceptFrom( Int playerIndex )
{
	NetCommandRef *tmp = m_netCommandList->getFirstMessage();
	while (tmp)
	{
		NetCommandRef *next = tmp->getNext();
		NetCommandMsg *msg = tmp->getCommand();

		if (msg->getPlayerID() != playerIndex)
		{
			DEBUG_LOG(("Connection::clearCommandsExceptFrom(%d) - clearing a command from player %d for frame %d",
				playerIndex, tmp->getCommand()->getPlayerID(), tmp->getCommand()->getExecutionFrame()));

			m_netCommandList->removeMessage(tmp);
			deleteInstance(tmp);
		}

		tmp = next;
	}
}

Bool Connection::isQueueEmpty() {
	if (m_netCommandList->getFirstMessage() == nullptr) {
		return TRUE;
	}
	return FALSE;
}

void Connection::setQuitting()
{
	m_isQuitting = TRUE;
	m_quitTime = timeGetTime();
	DEBUG_LOG(("Connection::setQuitting() at time %d", m_quitTime));
}

void Connection::resetRedundancy() {
	m_redundancyDepth = 0;
	m_redundancyCap = 0;
	m_lastLossEventTime = 0;
	m_lastDepthChangeTime = 0;
	m_queueSuspected = FALSE;
}

#if defined(GENERALS_ONLINE)
// The mesh connection to the peer in a slot, nullptr when there is none.
static PlayerConnection *GetMeshConnection(Int slot) {
	if (TheNGMPGame == nullptr || slot < 0) {
		return nullptr;
	}
	const NGMPGameSlot *gameSlot = (const NGMPGameSlot *)TheNGMPGame->getConstSlot(slot);
	if (gameSlot == nullptr) {
		return nullptr;
	}
	NetworkMesh *mesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	return (mesh != nullptr) ? mesh->GetConnectionForUser(gameSlot->m_userID) : nullptr;
}
#endif

// While the redundancy depth is above 0 the connection sends at the lossy frame grouping, otherwise at the configured one.
void Connection::applyFrameGrouping() {
	time_t grouping = m_configuredFrameGrouping;
#if defined(GENERALS_ONLINE)
	if (m_redundancyDepth > 0) {
		const Int lossyFrames = m_networkSettings.send.frames_per_packet_lossy;
		if (lossyFrames > 0) {
			const time_t lossy = (time_t)(lossyFrames * 1000 / GENERALS_ONLINE_HIGH_FPS_LIMIT);
			if (lossy < grouping) {
				grouping = lossy;
			}
		}
	}
	if (m_shortGrouping) {
		const time_t oneFrame = (time_t)(1000 / GENERALS_ONLINE_HIGH_FPS_LIMIT);
		if (oneFrame < grouping) {
			grouping = oneFrame;
		}
	}
#endif
	m_frameGrouping = grouping;
}

void Connection::setRedundancyDepth(Int depth, Int cap, time_t now) {
#if defined(GENERALS_ONLINE)
	if (depth != m_redundancyDepth) {
		m_lastDepthChangeTime = now;
	}
	m_redundancyDepth = depth;
	m_stats.setRedundancyDepth(depth, cap);
	applyFrameGrouping();
#endif
}

// How our packets fared on their way to the peer, from the peer's own reports; FALSE when there are none, then nothing is gated on them.
Bool Connection::getOutgoingLoss(float &lossPercent, Int &longestRun) {
#if defined(GENERALS_ONLINE)
	PlayerConnection *connection = GetMeshConnection(m_slot);
	PacketFeedback::Summary summary;
	if (connection == nullptr || !connection->GetOutgoingLoss(DataLossWindowMs, summary)) {
		return FALSE;
	}
	lossPercent = summary.lossPercent;
	longestRun = summary.longestRun;
	return TRUE;
#else
	return FALSE;
#endif
}

// A resend or a late ack on a link that loses nothing is delay, and repeating commands cannot help it.
// TRUE when there is no packet feedback to say otherwise.
Bool Connection::hasLossEvidence() {
#if defined(GENERALS_ONLINE)
	PlayerConnection *connection = GetMeshConnection(m_slot);
	PacketFeedback::Summary outLoss;
	PacketFeedback::Summary inLoss;
	if (connection == nullptr) {
		return TRUE;
	}
	const Bool hasOut = connection->GetOutgoingLoss(NETWORK_STATS_WINDOW_MS, outLoss);
	const Bool hasIn = connection->GetIncomingLoss(NETWORK_STATS_WINDOW_MS, inLoss);
	if (!hasOut && !hasIn) {
		return TRUE;
	}
	return (hasOut && (outLoss.lossPercent > 0.0f || outLoss.longestRun > 0)) || (hasIn && (inLoss.lossPercent > 0.0f || inLoss.longestRun > 0));
#else
	return TRUE;
#endif
}

// A loss takes the depth to 2, a burst (all copies of a command lost) one deeper.
void Connection::noteLossEvent(time_t now, Bool burst) {
#if defined(GENERALS_ONLINE)
	if (!m_networkSettings.send.redundancy) {
		return;
	}

	if (!hasLossEvidence()) {
		return;
	}

	// a full queue loses packets because it is full, more copies would only fill it further
	if (m_queueSuspected) {
		return;
	}

	m_lastLossEventTime = now;

	Int target = m_redundancyDepth;
	if (m_redundancyDepth < StartDepth) {
		target = StartDepth;
	} else if (burst) {
		// retries of commands sent before the last change say nothing about the new depth, one round trip has to pass
		time_t settle = (time_t)(2.0f * m_stats.getLatency());
		if (settle < MinAutoRedundancyWindow) {
			settle = MinAutoRedundancyWindow;
		}
		if ((now - m_lastDepthChangeTime) >= settle) {
			// a lost ack also leaves copies unacked: only a lost run of 2+ packets (peer report) counts as a burst
			float outLoss = 0.0f;
			Int outRun = 0;
			if (!getOutgoingLoss(outLoss, outRun) || outRun >= DataLossBurstRun) {
				target = m_redundancyDepth + 1;
			}
		}
	}

	if (target > m_redundancyCap) {
		target = m_redundancyCap;
	}
	if (target != m_redundancyDepth) {
		setRedundancyDepth(target, m_redundancyCap, now);
	}
#endif
}

Bool Connection::isQueueBuilding(time_t now) {
#if defined(GENERALS_ONLINE)
	enum { RECENT_MS = 3000, MIN_SAMPLES = 20, MIN_RISE_MS = 50 };
	const time_t since = now - RECENT_MS;
	if (m_stats.getSampleCountSince(since) < MIN_SAMPLES) {
		return FALSE;
	}
	const Real low = m_stats.getMinLatency();
	const Real recent = m_stats.getLatencyPercentile(50, since);
	if (low <= 0.0f || recent < 2.0f * low || (recent - low) < (Real)MIN_RISE_MS) {
		return FALSE;
	}
	float outLoss = 0.0f;
	Int outRun = 0;
	const Bool outLossing = getOutgoingLoss(outLoss, outRun) && outLoss >= 1.0f;
	return outLossing || m_stats.getRetryPercent() >= 2.0f;
#else
	return FALSE;
#endif
}

void Connection::updateRedundancy(time_t curtime, Bool &active, time_t &window, Int &depth) {
	active = FALSE;
	window = 0;
	depth = 0;
	m_retryTime = 2000;

#if defined(GENERALS_ONLINE)
	const NetworkSettings::Send &sendSettings = m_networkSettings.send;
	const Real srtt = m_stats.getLatency();
	const time_t interval = (m_frameGrouping > 1) ? m_frameGrouping : 1;

	if ((sendSettings.redundancy || m_unreliableTransport) && (srtt > 0.0f)) {
		const Bool fastResend = sendSettings.fast_resend;
		// the peer sends its acks with its next packet, up to one send interval after it got the command
		time_t retry = (time_t)(srtt + (fastResend ? 2.0f : 4.0f) * m_stats.getJitter()) + interval;
		if (retry < MinScaledRetryTime) {
			retry = MinScaledRetryTime;
		}
		if (retry > (fastResend ? MaxFastRetryTime : MaxScaledRetryTime)) {
			retry = fastResend ? MaxFastRetryTime : MaxScaledRetryTime;
		}
		m_retryTime = retry;
	}

	if (!sendSettings.redundancy) {
		if (m_redundancyDepth != 0) {
			setRedundancyDepth(0, 0, curtime);
		}
		return;
	}

	// the deepest depth covers the longest burst for the current send interval
	Int cap = (Int)((sendSettings.redundancy_burst_ms + interval - 1) / interval);
	if (cap > ConnectionStats::MAX_REDUNDANCY_DEPTH) {
		cap = ConnectionStats::MAX_REDUNDANCY_DEPTH;
	}
	if (cap < 1) {
		cap = 1;
	}

	if (cap != m_redundancyCap) {
		m_redundancyCap = cap;
		if (m_redundancyDepth > cap) {
			setRedundancyDepth(cap, cap, curtime);
		}
	}

	const Bool queue = isQueueBuilding(curtime);
	m_queueSuspected = queue;
	if (queue && m_redundancyDepth > 0) {
		// one step per settle time, the next round trips show whether the queue drains
		time_t settle = (time_t)(2.0f * srtt);
		if (settle < MinAutoRedundancyWindow) {
			settle = MinAutoRedundancyWindow;
		}
		if ((curtime - m_lastDepthChangeTime) >= settle) {
			setRedundancyDepth(m_redundancyDepth - 1, cap, curtime);
		}
	}

	if ((m_redundancyDepth > 0) && ((curtime - m_lastLossEventTime) >= NETWORK_STATS_WINDOW_MS)) {
		setRedundancyDepth(m_redundancyDepth - 1, cap, curtime);
		m_lastLossEventTime = curtime;
	}
	m_stats.setRedundancyDepth(m_redundancyDepth, cap);

	depth = m_redundancyDepth;
	active = (depth > 0);

	window = sendSettings.redundancy_window_ms;
	if (window <= 0) {
		window = (time_t)(2.0f * srtt);
		if (window < MinAutoRedundancyWindow) {
			window = MinAutoRedundancyWindow;
		}
	}
#endif
}

// Fills the rest of the packet with young un-acked commands, so the next packet repairs a loss without a retry.
void Connection::addRedundantCommands(NetPacket *packet, time_t curtime, time_t window, Int depth) {
#if defined(GENERALS_ONLINE)
	NetCommandRef *candidates[MaxRedundantCandidates];
	Int count = 0;
	const time_t interval = (m_frameGrouping > 1) ? m_frameGrouping : 1; // a command sent in this packet is younger than this

	NetCommandRef *ref = m_netCommandList->getFirstMessage();
	while ((ref != nullptr) && (count < MaxRedundantCandidates)) {
		const time_t timeLastSent = ref->getTimeLastSent();
		if ((timeLastSent != -1) && CommandRequiresAck(ref->getCommand())
			&& ((curtime - timeLastSent) >= interval) && ((curtime - ref->getTimeFirstSent()) <= window)
			&& (ref->getRedundantCopies() < depth)) {
			Int slot = count;
			while ((slot > 0) && (candidates[slot - 1]->getTimeFirstSent() > ref->getTimeFirstSent())) {
				candidates[slot] = candidates[slot - 1];
				--slot;
			}
			candidates[slot] = ref;
			++count;
		}
		ref = ref->getNext();
	}

	for (Int i = 0; i < count; ++i) {
		if (!packet->addCommand(candidates[i])) {
			break;
		}
		candidates[i]->noteRedundantCopy(curtime);
		candidates[i]->setTimeLastSent(curtime); // not a retry, so the round trip is still taken from the first send
	}
#endif
}

/**
 * This is the good part. We take all the network commands queued up for this connection,
 * packetize them and put them on the transport's send queue for actual sending.
 */
UnsignedInt Connection::doSend() {
	Int numpackets = 0;
	time_t curtime = timeGetTime();
	Bool couldQueue = TRUE;

	m_stats.update(curtime);

	// Do this check first, since it's an important fail-safe
	if (m_isQuitting && curtime > m_quitTime + MaxQuitFlushTime)
	{
		DEBUG_LOG(("Timed out a quitting connection.  Deleting all %d messages", m_netCommandList->length()));
		m_netCommandList->reset();
		return 0;
	}

	if ((curtime - m_lastTimeSent) < m_frameGrouping) {
//		DEBUG_LOG(("not sending packet, time = %d, m_lastFrameSent = %d, m_frameGrouping = %d", curtime, m_lastTimeSent, m_frameGrouping));
		NetworkLog(ELogVerbosity::LOG_DEBUG, "not sending packet, time = %I64d, m_lastFrameSent = %I64d, m_frameGrouping = %I64d", (Int64)curtime, (Int64)m_lastTimeSent, (Int64)m_frameGrouping);
		return 0;
	}

	Bool redundancy = FALSE;
	time_t redundancyWindow = 0;
	Int redundancyDepth = 0;
	updateRedundancy(curtime, redundancy, redundancyWindow, redundancyDepth);
#if defined(GENERALS_ONLINE)
	const Bool fastResend = m_networkSettings.send.fast_resend;
#else
	const Bool fastResend = FALSE;
#endif

	// iterate through all the messages and put them into a packet(s).
	NetCommandRef *msg = m_netCommandList->getFirstMessage();

	while ((msg != nullptr) && couldQueue) {
		NetPacket *packet = newInstance(NetPacket);
		packet->init();
		packet->setAddress(m_user->GetIPAddr(), m_user->GetPort());

		Bool notDone = TRUE;

		// add the command messages until either we run out of messages or the packet is full.
		while ((msg != nullptr) && notDone) {
			NetCommandRef *next = msg->getNext(); // Need this since msg could be deleted

			time_t timeLastSent = msg->getTimeLastSent();

			// a command inside the redundancy window is not retried, it goes out again as a redundant copy
			// fast resend: the first resend counts from the first send, not from the last redundant copy, and the window does not hold it back
			const time_t sentAt = (fastResend && !msg->isRetried() && (timeLastSent != -1)) ? msg->getTimeFirstSent() : timeLastSent;
			Bool due = (timeLastSent == -1) || ((curtime - sentAt) > m_retryTime);
			if (due && redundancy && !fastResend && (timeLastSent != -1) && ((curtime - msg->getTimeFirstSent()) <= redundancyWindow)) {
				due = FALSE;
			}

			if (due) {
				if (packet->addCommand(msg)) {
					// the msg command was added to the packet.
					if (CommandRequiresAck(msg->getCommand())) {
						if (timeLastSent != -1) {
							m_stats.noteRetry(curtime);
							if (!msg->isRetried() && !fastResend) {
								noteLossEvent(curtime, TRUE); // the command and all its copies were lost, or their acks were
							}
							msg->setRetried(TRUE);
						} else {
							msg->setTimeFirstSent(curtime);
						}
						msg->setTimeLastSent(curtime);
					} else {
						m_netCommandList->removeMessage(msg);
						deleteInstance(msg);
					}
				} else {
					notDone = FALSE;
					if (packet->getNumCommands() > 0) {
						next = msg; // the packet is full, this one starts the next packet
					}
				}
			}
			msg = next;
		}

		if (redundancy && notDone && (packet->getNumCommands() > 0)) {
			addRedundantCommands(packet, curtime, redundancyWindow, redundancyDepth);
		}

		if (msg != nullptr) {
			DEBUG_LOG(("didn't finish sending all commands in connection"));
		}

		++numpackets;

		/// @todo Make the act of giving the transport object a packet to send more efficient.  Make the transport take a NetPacket object rather than the raw data, thus avoiding an extra memcpy.
		if (packet->getNumCommands() > 0) {
			// If the packet actually has any information to give, give it to the transport object
			// for transmission.
			couldQueue = m_transport->queueSend(packet->getAddr(), packet->getPort(), packet->getData(), packet->getLength());
			m_lastTimeSent = curtime;
		}
		if (packet != NULL) {
			deleteInstance(packet); // delete the packet now that we're done with it.
		}
	}

	return numpackets;
}

NetCommandRef * Connection::processAck(NetAckStage1CommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetAckBothCommandMsg *msg) {
	return processAck(msg->getCommandID(), msg->getOriginalPlayerID());
}

NetCommandRef * Connection::processAck(NetCommandMsg *msg) {
	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKSTAGE1) {
		NetAckStage1CommandMsg *ackmsg = (NetAckStage1CommandMsg *)msg;
		return processAck(ackmsg);
	}

	if (msg->getNetCommandType() == NETCOMMANDTYPE_ACKBOTH) {
		NetAckBothCommandMsg *ackmsg = (NetAckBothCommandMsg *)msg;
		return processAck(ackmsg);
	}

	return nullptr;
}

/**
 * The person we are sending to has ack'd one of the messages we sent him.
 * Take that message off the list of commands to send.
 */
NetCommandRef * Connection::processAck(UnsignedShort commandID, UnsignedByte originalPlayerID) {
	NetCommandRef *temp = m_netCommandList->getFirstMessage();
	while ((temp != nullptr) && ((temp->getCommand()->getID() != commandID) || (temp->getCommand()->getPlayerID() != originalPlayerID))) {

		// cycle through the commands till we find the one we need to remove.
		// Need to check for both the command ID and the player ID.
		temp = temp->getNext();
	}
	// an ack for a command not sent yet belongs to another command with the same id
	if (temp == nullptr || temp->getTimeLastSent() == -1) {
		return nullptr;
	}

#if defined(RTS_DEBUG)
	Bool doDebug = FALSE;
	if (temp->getCommand()->getNetCommandType() == NETCOMMANDTYPE_DISCONNECTFRAME) {
		doDebug = TRUE;
	}
#endif

	// A retried command was last sent after its first send, so its round trip time would be too short.
	// Redundant copies are not retries: any copy's ack counts as an ack of the first send.
	const time_t now = timeGetTime();
	if (temp->isRetried()) {
		// an ack faster than any round trip so far answers the earlier send: the retry was for a late ack
#if defined(GENERALS_ONLINE)
		const Bool fastResend = m_networkSettings.send.fast_resend;
#else
		const Bool fastResend = FALSE;
#endif
		if ((m_stats.getSampleCount() > 0) && ((now - temp->getTimeLastSent()) < (time_t)(0.9f * m_stats.getMinLatency()))) {
			m_stats.noteLateAck(now);
			if (fastResend) {
				// unambiguous: it answers the first send, the slow acks that cause fast resends must not be missing from srtt
				m_stats.noteAcked((Real)(now - temp->getTimeFirstSent()), now);
			}
		} else if (fastResend) {
			// Karn: no sample from an ambiguous ack; the burst event moves here from the resend, a slow ack is no loss
			noteLossEvent(now, TRUE);
		}
	} else {
		const Real rtt = (Real)(now - temp->getTimeFirstSent());
		const Real srtt = m_stats.getLatency();
		const Real lateMargin = (srtt > MinLateAckMargin) ? srtt : (Real)MinLateAckMargin;
		if ((m_stats.getSampleCount() > 0) && ((rtt - srtt) > lateMargin)) {
			noteLossEvent(now, FALSE); // an ack later than the smoothed round trip plus its own length, at least 50 ms
		} else if ((temp->getTimeFirstRedundant() != -1) && (m_stats.getSampleCount() > 0)
			&& ((now - temp->getTimeFirstRedundant()) >= (time_t)m_stats.getMinLatency() - m_frameGrouping / 2)) {
			// the ack came too late to answer the first copy, a repeat repaired a loss
			noteLossEvent(now, FALSE);
		}
		m_stats.noteAcked((Real)(now - temp->getTimeFirstSent()), now);
	}

#if defined(RTS_DEBUG)
	if (doDebug == TRUE) {
		DEBUG_LOG(("Connection::processAck - disconnect frame command %d found, removing from command list.", commandID));
	}
#endif
	m_netCommandList->removeMessage(temp);
	return temp;
}

void Connection::setShortGrouping(Bool shortGrouping) {
	if (shortGrouping != m_shortGrouping) {
		m_shortGrouping = shortGrouping;
		applyFrameGrouping();
	}
}

void Connection::setFrameGrouping(time_t frameGrouping) {
	m_configuredFrameGrouping = frameGrouping;
	applyFrameGrouping();
//	m_retryTime = frameGrouping * 4;
}

#if defined(RTS_DEBUG)
void Connection::debugPrintCommands() {
	NetCommandRef *ref = m_netCommandList->getFirstMessage();
	while (ref != nullptr) {
		DEBUG_LOG(("Connection::debugPrintCommands - ID: %d\tType: %s\tRelay: 0x%X for frame %d",
			ref->getCommand()->getID(), GetNetCommandTypeAsString(ref->getCommand()->getNetCommandType()),
			ref->getRelay(), ref->getCommand()->getExecutionFrame()));
		ref = ref->getNext();
	}
}
#endif
