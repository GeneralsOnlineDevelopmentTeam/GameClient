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


#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/crc.h"
#include "GameNetwork/Transport.h"
#include "GameNetwork/NetworkInterface.h"

#ifdef RTS_INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

//--------------------------------------------------------------------------
// Packet-level encryption is an XOR operation, for speed reasons.  To get
// the max throughput, we only XOR whole 4-byte words, so the last bytes
// can be non-XOR'd.

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void encryptBuf(unsigned char* buf, Int len)
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt* uintPtr = (UnsignedInt*)(buf);

	for (int i = 0; i < len / 4; i++) {
		*uintPtr = (*uintPtr) ^ mask;
		*uintPtr = htonl(*uintPtr);
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

// This assumes the buf is a multiple of 4 bytes.  Extra is not encrypted.
static inline void decryptBuf(unsigned char* buf, Int len)
{
	UnsignedInt mask = 0x0000Fade;

	UnsignedInt* uintPtr = (UnsignedInt*)(buf);

	for (int i = 0; i < len / 4; i++) {
		*uintPtr = htonl(*uintPtr);
		*uintPtr = (*uintPtr) ^ mask;
		uintPtr++;
		mask += 0x00000321; // just for fun
	}
}

//--------------------------------------------------------------------------

UDPTransport::UDPTransport(void)
{
	m_winsockInit = false;
	m_udpsock = NULL;
}

UDPTransport::~UDPTransport(void)
{
	reset();
}

Bool UDPTransport::init(AsciiString ip, UnsignedShort port)
{
	return init(ResolveIP(ip), port);
}

Bool UDPTransport::init(UnsignedInt ip, UnsignedShort port)
{
	// ----- Initialize Winsock -----
	if (!m_winsockInit)
	{
		WORD verReq = MAKEWORD(2, 2);
		WSADATA wsadata;

		int err = WSAStartup(verReq, &wsadata);
		if (err != 0) {
			return false;
		}

		if ((LOBYTE(wsadata.wVersion) != 2) || (HIBYTE(wsadata.wVersion) != 2)) {
			WSACleanup();
			return false;
		}
		m_winsockInit = true;
	}

	// ------- Bind our port --------
	if (m_udpsock)
		delete m_udpsock;
	m_udpsock = NEW UDP();

	if (!m_udpsock)
		return false;

	int retval = -1;
	time_t now = timeGetTime();
	while ((retval != 0) && ((timeGetTime() - now) < 1000)) {
		retval = m_udpsock->Bind(ip, port);
	}

	if (retval != 0) {
		DEBUG_CRASH(("Could not bind to 0x%8.8X:%d\n", ip, port));
		DEBUG_LOG(("UDPTransport::init - Failure to bind socket with error code %x\n", retval));
		delete m_udpsock;
		m_udpsock = NULL;
		return false;
	}

	// ------- Clear buffers --------
	int i = 0;
	for (; i < MAX_MESSAGES; ++i)
	{
		m_outBuffer[i].length = 0;
		m_inBuffer[i].length = 0;
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
		m_delayedInBuffer[i].message.length = 0;
#endif
	}
	for (i = 0; i < MAX_TRANSPORT_STATISTICS_SECONDS; ++i)
	{
		m_incomingBytes[i] = 0;
		m_outgoingBytes[i] = 0;
		m_unknownBytes[i] = 0;
		m_incomingPackets[i] = 0;
		m_outgoingPackets[i] = 0;
		m_unknownPackets[i] = 0;
	}
	m_statisticsSlot = 0;
	m_lastSecond = timeGetTime();

	m_port = port;

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
	if (TheGlobalData->m_latencyAverage > 0 || TheGlobalData->m_latencyNoise)
		m_useLatency = true;

	if (TheGlobalData->m_packetLoss)
		m_usePacketLoss = true;
#endif

	return true;
}

void UDPTransport::reset(void)
{
	if (m_udpsock)
	{
		delete m_udpsock;
		m_udpsock = NULL;
	}

	if (m_winsockInit)
	{
		WSACleanup();
		m_winsockInit = false;
	}
}

Bool UDPTransport::update(void)
{
	Bool retval = TRUE;
	if (doRecv() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s\n", GetWSAErrorString(WSAGetLastError()).str()));
	if (doSend() == FALSE && m_udpsock && m_udpsock->GetStatus() == UDP::ADDRNOTAVAIL)
	{
		retval = FALSE;
	}
	DEBUG_ASSERTLOG(retval, ("WSA error is %s\n", GetWSAErrorString(WSAGetLastError()).str()));
	return retval;
}

Bool UDPTransport::doSend() {
	if (!m_udpsock)
	{
		DEBUG_LOG(("UDPTransport::doSend() - m_udpSock is NULL!\n"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Statistics gathering
	UnsignedInt now = timeGetTime();
	if (m_lastSecond + 1000 < now)
	{
		m_lastSecond = now;
		m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
		m_outgoingPackets[m_statisticsSlot] = 0;
		m_outgoingBytes[m_statisticsSlot] = 0;
		m_incomingPackets[m_statisticsSlot] = 0;
		m_incomingBytes[m_statisticsSlot] = 0;
		m_unknownPackets[m_statisticsSlot] = 0;
		m_unknownBytes[m_statisticsSlot] = 0;
	}

	// Send all messages
	int i;
	for (i = 0; i < MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length != 0)
		{
			int bytesSent = 0;
			int bytesToSend = m_outBuffer[i].length + sizeof(TransportMessageHeader);
			// Send this message
			if ((bytesSent = m_udpsock->Write((unsigned char*)(&m_outBuffer[i]), bytesToSend, m_outBuffer[i].addr, m_outBuffer[i].port)) > 0)
			{
				//DEBUG_LOG(("Sending %d bytes to %d.%d.%d.%d:%d\n", bytesToSend, PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				if (bytesSent != bytesToSend)
				{
					DEBUG_LOG(("UDPTransport::doSend - wanted to send %d bytes, only sent %d bytes to %d.%d.%d.%d:%d\n",
						bytesToSend, bytesSent,
						PRINTF_IP_AS_4_INTS(m_outBuffer[i].addr), m_outBuffer[i].port));
				}
			}
			else
			{
				//DEBUG_LOG(("Could not write to socket!!!  Not discarding message!\n"));
				retval = FALSE;
				//DEBUG_LOG(("UDPTransport::doSend returning FALSE\n"));
			}
		}
	} // for (i=0; i<MAX_MESSAGES; ++i)

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
	// Latency simulation - deliver anything we're holding on to that is ready
	if (m_useLatency)
	{
		for (i = 0; i < MAX_MESSAGES; ++i)
		{
			if (m_delayedInBuffer[i].message.length != 0 && m_delayedInBuffer[i].deliveryTime <= now)
			{
				for (int j = 0; j < MAX_MESSAGES; ++j)
				{
					if (m_inBuffer[j].length == 0)
					{
						// Empty slot; use it
						memcpy(&m_inBuffer[j], &m_delayedInBuffer[i].message, sizeof(TransportMessage));
						m_delayedInBuffer[i].message.length = 0;
						break;
					}
				}
			}
		}
	}
#endif
	return retval;
}

Bool UDPTransport::doRecv()
{
	if (!m_udpsock)
	{
		DEBUG_LOG(("UDPTransport::doRecv() - m_udpSock is NULL!\n"));
		return FALSE;
	}

	Bool retval = TRUE;

	// Read in anything on our socket
	sockaddr_in from;
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
	UnsignedInt now = timeGetTime();
#endif

	TransportMessage incomingMessage;
	unsigned char* buf = (unsigned char*)&incomingMessage;
	int len = MAX_MESSAGE_LEN;
	//	DEBUG_LOG(("UDPTransport::doRecv - checking\n"));
	while ((len = m_udpsock->Read(buf, MAX_MESSAGE_LEN, &from)) > 0)
	{
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
		// Packet loss simulation
		if (m_usePacketLoss)
		{
			if (TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100))
			{
				continue;
			}
		}
#endif

		//		DEBUG_LOG(("UDPTransport::doRecv - Got something! len = %d\n", len));
				// Decrypt the packet
		//		DEBUG_LOG(("buffer = "));
		//		for (Int munkee = 0; munkee < len; ++munkee) {
		//			DEBUG_LOG(("%02x", *(buf + munkee)));
		//		}
		//		DEBUG_LOG(("\n"));
		decryptBuf(buf, len);

		incomingMessage.length = len - sizeof(TransportMessageHeader);

		if (len <= sizeof(TransportMessageHeader) || !isGeneralsPacket(&incomingMessage))
		{
			DEBUG_LOG(("UDPTransport::doRecv - unknownPacket! len = %d\n", len));
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += len;
			continue;
		}

		// Something there; stick it somewhere
//		DEBUG_LOG(("Saw %d bytes from %d:%d\n", len, ntohl(from.sin_addr.S_un.S_addr), ntohs(from.sin_port)));
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += len;

		for (int i = 0; i < MAX_MESSAGES; ++i)
		{
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
			// Latency simulation
			if (m_useLatency)
			{
				if (m_delayedInBuffer[i].message.length == 0)
				{
					// Empty slot; use it
					m_delayedInBuffer[i].deliveryTime =
						now + TheGlobalData->m_latencyAverage +
						(Int)(TheGlobalData->m_latencyAmplitude * sin(now * TheGlobalData->m_latencyPeriod)) +
						GameClientRandomValue(-TheGlobalData->m_latencyNoise, TheGlobalData->m_latencyNoise);
					m_delayedInBuffer[i].message.length = incomingMessage.length;
					m_delayedInBuffer[i].message.addr = ntohl(from.sin_addr.S_un.S_addr);
					m_delayedInBuffer[i].message.port = ntohs(from.sin_port);
					memcpy(&m_delayedInBuffer[i].message, buf, len);
					break;
				}
			}
			else
			{
#endif
				if (m_inBuffer[i].length == 0)
				{
					// Empty slot; use it
					m_inBuffer[i].length = incomingMessage.length;
					m_inBuffer[i].addr = ntohl(from.sin_addr.S_un.S_addr);
					m_inBuffer[i].port = ntohs(from.sin_port);
					memcpy(&m_inBuffer[i], buf, len);
					break;
				}
#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
			}
#endif
		}
		//DEBUG_ASSERTCRASH(i<MAX_MESSAGES, ("Message lost!"));
	}

	if (len == -1) {
		// there was a socket error trying to perform a read.
		//DEBUG_LOG(("UDPTransport::doRecv returning FALSE\n"));
		retval = FALSE;
	}

	return retval;
}

Bool UDPTransport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len /*,
						  NetMessageFlags flags, Int id */)
{
	int i;

	if (len < 1 || len > MAX_PACKET_SIZE)
	{
		DEBUG_LOG(("UDPTransport::queueSend - Invalid Packet size\n"));
		return false;
	}

	for (i = 0; i < MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length == 0)
		{
			// Insert data here
			m_outBuffer[i].length = len;
			memcpy(m_outBuffer[i].data, buf, len);
			m_outBuffer[i].addr = addr;
			m_outBuffer[i].port = port;
			//			m_outBuffer[i].header.flags = flags;
			//			m_outBuffer[i].header.id = id;
			m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

			CRC crc;
			crc.computeCRC((unsigned char*)(&(m_outBuffer[i].header.magic)), m_outBuffer[i].length + sizeof(TransportMessageHeader) - sizeof(UnsignedInt));
			//			DEBUG_LOG(("About to assign the CRC for the packet\n"));
			m_outBuffer[i].header.crc = crc.get();

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char*)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
			//			DEBUG_LOG(("\n"));

			return true;
		}
	}
	DEBUG_LOG(("Send Queue is getting full, dropping packets\n"));
	return false;
}


