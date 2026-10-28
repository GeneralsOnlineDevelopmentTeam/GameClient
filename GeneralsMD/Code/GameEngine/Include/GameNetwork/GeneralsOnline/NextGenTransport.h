#pragma once

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/Transport.h"
#include "../NGMP_include.h"

#include <steam/isteamnetworkingmessages.h>

#pragma comment(lib, "Secur32.lib")

// Struct to track retry state for outgoing packets
struct OutgoingPacketState
{
	Int retryCount = 0;
	static constexpr Int MAX_RETRIES = 3;
};

// it to be a MemoryPoolObject (srj)
class NextGenTransport : public Transport //: public MemoryPoolObject
{
	//MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Transport, "Transport")		
public:

	NextGenTransport();
	~NextGenTransport();

	Bool init( AsciiString ip, UnsignedShort port ) override;
	Bool init( UnsignedInt ip, UnsignedShort port ) override;
	void reset( void ) override;
	Bool update( void ) override;									///< Call this once a GameEngine tick, regardless of whether the frame advances.

	Bool doRecv( void ) override;		///< call this to service the receive packets
	Bool doSend( void ) override;		///< call this to service the send queue.

	Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
		NetMessageFlags flags, Int id */);				///< Queue a packet for sending to the specified address and port.  This will be sent on the next update() call.

	inline Bool allowBroadcasts(Bool val) override { return false; }

	// Helper to clear a packet from the receive buffer (accounts for zero-length packets)
	void clearInBufferSlot(int slotIndex)
	{
		if (slotIndex >= 0 && slotIndex < MAX_MESSAGES)
		{
			m_inBuffer[slotIndex].length = 0;
			m_inBufferOccupied[slotIndex] = false;
		}
	}

private:
	Bool sendFailureLogDue();	///< true once a second, the other failure lines are counted
	UnsignedInt m_nextSendFailLogMs = 0;
	Int m_skippedSendFailLogs = 0;
	OutgoingPacketState m_outPacketState[MAX_MESSAGES];
	// Track which incoming buffer slots are occupied (handles zero-length packets)
	bool m_inBufferOccupied[MAX_MESSAGES];
};
