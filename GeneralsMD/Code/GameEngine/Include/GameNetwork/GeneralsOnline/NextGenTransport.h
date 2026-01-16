#pragma once

#define STEAMNETWORKINGSOCKETS_STATIC_LINK 1

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/Transport.h"
#include "../NGMP_include.h"

#include "GameNetwork/GeneralsOnline/Vendor/ValveNetworkingSockets/steam/isteamnetworkingmessages.h"

// Library linking moved to NextGenTransport.cpp to prevent early DLL loading during static initialization
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */
/* Removed - moved to .cpp */

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

private:
	
};
