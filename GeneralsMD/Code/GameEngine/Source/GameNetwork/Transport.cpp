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


#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/CRC.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"

#ifdef _INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf( unsigned char *buf, Int len )
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt *uintPtr = (UnsignedInt *) (buf);

	for (int i=0 ; i<len/4 ; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

Transport::Transport(void)
{

}

Transport::~Transport(void)
{

}

Bool Transport::isGeneralsPacket( TransportMessage *msg )
{
	if (!msg)
		return false;

	if (msg->length < 0 || msg->length > MAX_MESSAGE_LEN)
		return false;

	CRC crc;
//	crc.computeCRC( (unsigned char *)msg->data, msg->length );
	crc.computeCRC( (unsigned char *)(&(msg->header.magic)), msg->length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt) );

	if (crc.get() != msg->header.crc)
		return false;

	if (msg->header.magic != GENERALS_MAGIC_NUMBER)
		return false;

	return true;
}

// Statistics ---------------------------------------------------
Real Transport::getIncomingBytesPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getIncomingPacketsPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_incomingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingBytesPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getOutgoingPacketsPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_outgoingPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownBytesPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownBytes[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}

Real Transport::getUnknownPacketsPerSecond( void )
{
	Real val = 0.0;
	for (int i=0; i<MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		if (i != m_statisticsSlot)
			val += m_unknownPackets[i];
	}
	return val / (MAX_TRANSPORT_STATISTICS_SECONDS-1);
}



