/*
**	Command & Conquer Generals(tm)
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

// Transport.h ///////////////////////////////////////////////////////////////
// Transport layer - a thin layer around a UDP socket, with queues.
// Author: Matthew D. Campbell, July 2001

#pragma once

// NGMP NOTE: We have multiple transports now, so UDPTransport is what Transport was. It's the legacy, direct connection transport the original game used. Transport is now a base class.

#ifndef _UDPTRANSPORT_H_
#define _UDPTRANSPORT_H_

#include "GameNetwork/Transport.h"
#include "GameNetwork/udp.h"
#include "GameNetwork/NetworkDefs.h"

/**
 * The transport layer handles the UDP socket for the game, and will packetize and
 * de-packetize multiple ACK/CommandPacket/etc packets into larger aggregates.
 */
// we only ever allocate one of there, and it is quite large, so we really DON'T want
// it to be a MemoryPoolObject (srj)
class UDPTransport : public Transport //: public MemoryPoolObject
{
	//MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(Transport, "Transport")		
public:

	UDPTransport();
	~UDPTransport();

	Bool init( AsciiString ip, UnsignedShort port ) override;
	Bool init( UnsignedInt ip, UnsignedShort port ) override;
	void reset( void ) override;
	Bool update( void ) override;									///< Call this once a GameEngine tick, regardless of whether the frame advances.

	Bool doRecv( void ) override;		///< call this to service the receive packets
	Bool doSend( void ) override;		///< call this to service the send queue.

	Bool queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte *buf, Int len /*,
		NetMessageFlags flags, Int id */) override;				///< Queue a packet for sending to the specified address and port.  This will be sent on the next update() call.

	inline Bool allowBroadcasts(Bool val) override { if (!m_udpsock) return false; return (m_udpsock->AllowBroadcasts(val))?true:false; }

	UnsignedShort m_port;
private:
	Bool m_winsockInit;
	UDP *m_udpsock;
};

#endif // _UDPTRANSPORT_H_
