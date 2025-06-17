#include "PreRTS.h"	// This must go first in EVERY cpp file int the GameEngine

#include "Common/CRC.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/Packets/NextGenTransport.h"

#include "GameNetwork/GeneralsOnline/ngmp_include.h"
#include "GameNetwork/GeneralsOnline/ngmp_interfaces.h"

#ifdef _INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

NextGenTransport::NextGenTransport()
{

}

NextGenTransport::~NextGenTransport()
{
	reset();
}

Bool NextGenTransport::init(AsciiString ip, UnsignedShort port)
{
	return true;
}

Bool NextGenTransport::init(UnsignedInt ip, UnsignedShort port)
{
	return true;
}

void NextGenTransport::reset(void)
{

}

Bool NextGenTransport::update(void)
{
	// TODO_NGMP: Check more here
	Bool retval = TRUE;
	if (doRecv() == FALSE)
	{
		retval = FALSE;
	}
	if (doSend() == FALSE)
	{
		retval = FALSE;
	}

	return retval;
}

Bool NextGenTransport::doRecv(void)
{
	bool bRet = false;

	TransportMessage incomingMessage;
	unsigned char* buf = (unsigned char*)&incomingMessage;

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
	while (pMesh->HasGamePacket())
	{
		bRet = true;

		QueuedGamePacket gamePacket = pMesh->RecvGamePacket();

		uint32_t numBytes = gamePacket.m_bs->GetNumBytesAllocated();

		NetworkLog("[NGMP]: Received %d bytes from user %d", numBytes, gamePacket.m_userID);

		

		// avoiding memcpy, since the game memcpy's it into a free slot anyway
		//buf = (unsigned char*)gamePacket.m_packet->data;
		memcpy(buf, gamePacket.m_bs->GetRawBuffer(), numBytes);

		// delete the bitstream
		delete gamePacket.m_bs;


		// generals logic
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

		incomingMessage.length = numBytes - sizeof(TransportMessageHeader);

		// is it a generals packet?
		if (isGeneralsPacket(&incomingMessage))
		{
			//NetworkLog("Game Packet Recv: Is a generals packet");
		}
		else
		{
			NetworkLog("Game Packet Recv: Is NOT a generals packet");
		}

		if (numBytes <= sizeof(TransportMessageHeader) || !isGeneralsPacket(&incomingMessage))
		//if (numBytes <= sizeof(TransportMessageHeader))
		{
			m_unknownPackets[m_statisticsSlot]++;
			m_unknownBytes[m_statisticsSlot] += numBytes;
			continue;
		}

		// Something there; stick it somewhere
//		DEBUG_LOG(("Saw %d bytes from %d:%d\n", len, ntohl(from.sin_addr.S_un.S_addr), ntohs(from.sin_port)));
		m_incomingPackets[m_statisticsSlot]++;
		m_incomingBytes[m_statisticsSlot] += numBytes;

		for (int i = 0; i < MAX_MESSAGES; ++i)
		{
			if (m_inBuffer[i].length == 0)
			{
				// Empty slot; use it
				m_inBuffer[i].length = incomingMessage.length;

				// dont care about address anymore
				//m_inBuffer[i].addr = ntohl(from.sin_addr.S_un.S_addr);
				//m_inBuffer[i].port = ntohs(from.sin_port);

				memcpy(&m_inBuffer[i], buf, numBytes);
				break;
			}
		}
	}

	return bRet;
}

Bool NextGenTransport::doSend(void)
{
	bool retval = true;

	int i;
	for (i = 0; i < MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length != 0)
		{
			// TODO_NGMP: Get this from game info, not the lobby, we should tear lobby down probably
			// addr is actually player index...
			// TODO: What if it's empty?
			NGMPGameSlot* pSlot = (NGMPGameSlot*)NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame()->getSlot(m_outBuffer[i].addr);

			if (pSlot != nullptr)
			{
				retval = (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh()->SendGamePacket((void*)(&m_outBuffer[i]), (uint32_t)m_outBuffer[i].length + sizeof(TransportMessageHeader), pSlot->m_userID) >= 0);
			}
			else
			{
				retval = false;
			}

			if (retval)
			{
				//DEBUG_LOG(("Sending %d bytes to %d:%d\n", m_outBuffer[i].length + sizeof(TransportMessageHeader), m_outBuffer[i].addr, m_outBuffer[i].port));
				m_outgoingPackets[m_statisticsSlot]++;
				m_outgoingBytes[m_statisticsSlot] += m_outBuffer[i].length + sizeof(TransportMessageHeader);
				m_outBuffer[i].length = 0;  // Remove from queue
				//				DEBUG_LOG(("UDPTransport::doSend - sent %d butes to %d.%d.%d.%d:%d\n", bytesSent,
				//					(m_outBuffer[i].addr >> 24) & 0xff,
				//					(m_outBuffer[i].addr >> 16) & 0xff,
				//					(m_outBuffer[i].addr >> 8) & 0xff,
				//					m_outBuffer[i].addr & 0xff,
				//					m_outBuffer[i].port));
			}
			else
			{
				retval = FALSE;
			}
		}
	}

	return retval;
}

Bool NextGenTransport::queueSend(UnsignedInt addr, UnsignedShort port, const UnsignedByte* buf, Int len /*, NetMessageFlags flags, Int id */)
{
	// TODO_NGMP: Do we care about addr/port here in the new impl?
	int i;

	if (len < 1 || len > MAX_PACKET_SIZE)
	{
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

			// is it a generals packet?
			if (isGeneralsPacket(&m_outBuffer[i]))
			{
				//NetworkLog("Game Packet Queue Sending: Is a generals packet");
			}
			else
			{
				//NetworkLog("Game Packet Queue Sending: Is NOT a generals packet");
			}

			return true;
		}
	}
	return false;
}
