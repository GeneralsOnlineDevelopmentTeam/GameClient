#pragma once

#define STEAMNETWORKINGSOCKETS_STATIC_LINK 1

#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"
#include "../../Transport.h"
#include "../NGMP_include.h"


#pragma comment(lib, "ENet/enet.lib")
#include "GameNetwork/GeneralsOnline/Vendor/ValveNetworkingSockets/steam/isteamnetworkingmessages.h"
#pragma comment(lib, "ValveNetworkingSockets/GameNetworkingSockets.lib")
#pragma comment(lib, "ValveNetworkingSockets/abseil_dll.lib")
#pragma comment(lib, "ValveNetworkingSockets/libcrypto.lib")
#pragma comment(lib, "ValveNetworkingSockets/libprotobuf.lib")
#pragma comment(lib, "ValveNetworkingSockets/libssl.lib")
#pragma comment(lib, "ValveNetworkingSockets/steamwebrtc.lib")
#pragma comment(lib, "ValveNetworkingSockets/webrtc-lite.lib")
#pragma comment(lib, "Secur32.lib")

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