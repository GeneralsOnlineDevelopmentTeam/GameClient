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

NextGenTransport::NextGenTransport()
{

}

NextGenTransport::~NextGenTransport()
{
	reset();
}

Bool NextGenTransport::init(AsciiString ip, UnsignedShort port)
{
	if (!m_bHasSocket)
	{
		//m_SockID.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
		//strcpy(m_SockID.SocketName, std::format("GAME_TRANSPORT_SOCKET", port).c_str());
	}

	return true;
}

Bool NextGenTransport::init(UnsignedInt ip, UnsignedShort port)
{
	if (!m_bHasSocket)
	{
		//m_SockID.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
		//strcpy(m_SockID.SocketName, std::format("GAME_TRANSPORT_SOCKET", port).c_str());
	}

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
	TransportMessage incomingMessage;
	unsigned char* buf = (unsigned char*)&incomingMessage;

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
	while (pMesh->HasGamePacket())
	{
		QueuedGamePacket gamePacket = pMesh->RecvGamePacket();

		NetworkLog("[NGMP]: Received %d bytes from user %d", gamePacket.m_packet->dataLength, gamePacket.m_userID);

		uint32_t numBytes = gamePacket.m_packet->dataLength;

		// avoiding memcpy, since the game memcpy's it into a free slot anyway
		buf = (unsigned char*)gamePacket.m_packet->data;

			/*
			TransportMessageHeader header;
	UnsignedByte data[MAX_MESSAGE_LEN];
	Int length;
	UnsignedInt addr;
	UnsignedShort port;
			*/

		// generals logic
#if defined(_DEBUG) || defined(_INTERNAL)
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
		decryptBuf(buf, numBytes);

		incomingMessage.length = numBytes - sizeof(TransportMessageHeader);

		//if (numBytes <= sizeof(TransportMessageHeader) || !isGeneralsPacket(&incomingMessage))
		if (numBytes <= sizeof(TransportMessageHeader))
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

	/*
	TransportMessage incomingMessage;
	unsigned char* buf = (unsigned char*)&incomingMessage;

	auto P2PHandle = EOS_Platform_GetP2PInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	uint8_t channelToUse = (uint8_t)ENetworkMeshType::GAME_TRANSPORT;

	// recv
	EOS_P2P_GetNextReceivedPacketSizeOptions sizeOptions;
	sizeOptions.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
	sizeOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	sizeOptions.RequestedChannel = &channelToUse; // room mesh channels

	bool bRet = false;

	uint32_t numBytes = 0;

	EOS_EResult sizeResult = EOS_P2P_GetNextReceivedPacketSize(P2PHandle, &sizeOptions, &numBytes);
	while (sizeResult == EOS_EResult::EOS_Success)
	{
		EOS_P2P_ReceivePacketOptions options;
		options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
		options.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
		options.MaxDataSizeBytes = numBytes;
		options.RequestedChannel = &channelToUse; // room mesh channels

		EOS_ProductUserId outRemotePeerID = nullptr;
		EOS_P2P_SocketId outSocketID;
		uint8_t outChannel = 0;
		EOS_EResult result = EOS_P2P_ReceivePacket(P2PHandle, &options, &outRemotePeerID, &outSocketID, &outChannel, (void*)buf, &numBytes);

		if (result == EOS_EResult::EOS_Success)
		{
			bRet = true;

			// TODO_NGMP: Reject any packets from members not in the room? or mesh
			char szEOSUserID[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
			int32_t outLenLocal = sizeof(szEOSUserID);
			EOS_ProductUserId_ToString(outRemotePeerID, szEOSUserID, &outLenLocal);
			NetworkLog("[NGMP]: Received %d bytes from user %s", numBytes, szEOSUserID);

			if (outRemotePeerID == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser())
			{
				NetworkLog("Received from self...");
			}

			// generals logic
#if defined(_DEBUG) || defined(_INTERNAL)
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
			decryptBuf(buf, numBytes);

			incomingMessage.length = numBytes - sizeof(TransportMessageHeader);

			if (numBytes <= sizeof(TransportMessageHeader) || !isGeneralsPacket(&incomingMessage))
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
					//m_inBuffer[i].addr = ntohl(from.sin_addr.S_un.S_addr);
					//m_inBuffer[i].port = ntohs(from.sin_port);
					memcpy(&m_inBuffer[i], buf, numBytes);
					break;
				}
			}

		}

		sizeResult = EOS_P2P_GetNextReceivedPacketSize(P2PHandle, &sizeOptions, &numBytes);
	}

	return bRet;
	*/

	return false;
}

Bool NextGenTransport::doSend(void)
{
	//auto P2PHandle = EOS_Platform_GetP2PInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());
	
	bool retval = true;

	int i;
	for (i = 0; i < MAX_MESSAGES; ++i)
	{
		if (m_outBuffer[i].length != 0)
		{
			// TODO_NGMP: Get this from game info, not the lobby, we should tear lobby down probably
			// addr is actually player index...
			// TODO: What if it's empty?
			LobbyMemberEntry lobbyMember = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetRoomMemberFromIndex(m_outBuffer[i].addr);

			if (lobbyMember.IsValid())
			{
				retval = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh()->SendGamePacket((void*)(&m_outBuffer[i]), (uint32_t)m_outBuffer[i].length + sizeof(TransportMessageHeader), lobbyMember);
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

			// TODO_NGMP: Custom

			/*
			NGMPGameSlot* pSlot = (NGMPGameSlot*)NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentGame()->getSlot(m_outBuffer[i].addr);
			EOS_ProductUserId eosUser = pSlot->m_userID;

			EOS_P2P_SendPacketOptions sendPacketOptions;
			sendPacketOptions.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
			sendPacketOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
			sendPacketOptions.RemoteUserId = eosUser;
			sendPacketOptions.SocketId = &m_SockID;
			sendPacketOptions.Channel = (uint8_t)ENetworkMeshType::GAME_TRANSPORT;
			sendPacketOptions.DataLengthBytes = (uint32_t)m_outBuffer[i].length + sizeof(TransportMessageHeader);
			sendPacketOptions.Data = (void*)(&m_outBuffer[i]);
			sendPacketOptions.bAllowDelayedDelivery = true;
			sendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
			sendPacketOptions.bDisableAutoAcceptConnection = false;

			if (eosUser == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser())
			{
				NetworkLog("Sending to self...");
			}

			EOS_EResult result = EOS_P2P_SendPacket(P2PHandle, &sendPacketOptions);

			//NetworkLog("[NGMP]: Sending Packet with %d bytes to %s with result %d", m_outBuffer->length, m_eosUserID, result);

			// Send this message
			if (result == EOS_EResult::EOS_Success)
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
			*/
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

			// Encrypt packet
//			DEBUG_LOG(("buffer: "));
			encryptBuf((unsigned char*)&m_outBuffer[i], len + sizeof(TransportMessageHeader));
			//			DEBUG_LOG(("\n"));

			return true;
		}
	}
	return false;
}
