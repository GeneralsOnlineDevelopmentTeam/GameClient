#include "GameNetwork/GeneralsOnline/NetworkMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"

void NetworkMesh::SendHelloMsg(uint64_t targetUser)
{
	NetRoom_HelloPacket helloPacket;

	std::vector<uint64_t> vecUsers;
	vecUsers.push_back(targetUser);

	SendToMesh(helloPacket, vecUsers);
}

void NetworkMesh::SendHelloAckMsg(uint64_t targetUser)
{
	NetRoom_HelloAckPacket helloAckPacket;

	std::vector<uint64_t> vecUsers;
	vecUsers.push_back(targetUser);

	SendToMesh(helloAckPacket, vecUsers);
}

void NetworkMesh::SendToMesh(NetworkPacket& packet, std::vector<uint64_t> vecTargetUsers)
{
	/*
	auto P2PHandle = EOS_Platform_GetP2PInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	CBitStream* pBitStream = packet.Serialize();

	for (EOS_ProductUserId targetUser : vecTargetUsers)
	{
		EOS_P2P_SendPacketOptions sendPacketOptions;
		sendPacketOptions.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
		sendPacketOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
		sendPacketOptions.RemoteUserId = targetUser;
		sendPacketOptions.SocketId = &m_SockID;
		sendPacketOptions.Channel = (uint8_t)m_meshType;
		sendPacketOptions.DataLengthBytes = (uint32_t)pBitStream->GetNumBytesUsed();
		sendPacketOptions.Data = (void*)pBitStream->GetRawBuffer();
		sendPacketOptions.bAllowDelayedDelivery = true;
		sendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
		sendPacketOptions.bDisableAutoAcceptConnection = false;

		// TODO_NGMP: Support more packet types obviously

		EOS_EResult result = EOS_P2P_SendPacket(P2PHandle, &sendPacketOptions);

		char szEOSUserID[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
		int32_t outLenLocal = sizeof(szEOSUserID);
		EOS_ProductUserId_ToString(targetUser, szEOSUserID, &outLenLocal);
		NetworkLog("[NGMP]: Sending Packet with %d bytes to %s with result %d", pBitStream->GetNumBytesUsed(), szEOSUserID, result);
	}
	*/
}

void NetworkMesh::ConnectToMesh(const char* szRoomID)
{
	/*
	m_SockID.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
	strcpy(m_SockID.SocketName, szRoomID);

	// TODO_NGMP: Dont automatically accept connections that aren't in the room roster, security

	// connection created callback
	auto P2PHandle = EOS_Platform_GetP2PInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	// TODO_NGMP: This is specific to socket ID, unregister it when we leave or join another room
	EOS_P2P_AddNotifyPeerConnectionEstablishedOptions opts;
	opts.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
	opts.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	opts.SocketId = &m_SockID;
	EOS_P2P_AddNotifyPeerConnectionEstablished(P2PHandle, &opts, this, [](const EOS_P2P_OnPeerConnectionEstablishedInfo* Data)
		{
			NetworkMesh* pMesh = (NetworkMesh * )Data->ClientData;

			NetworkMemberBase* pMember = nullptr;

			if (pMesh->GetMeshType() == ENetworkMeshType::NETWORK_ROOM)
			{
				pMember = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetRoomMemberFromID(Data->RemoteUserId);
			}
			else if (pMesh->GetMeshType() == ENetworkMeshType::GAME_LOBBY)
			{
				// TODO_NGMP: Custom
				//pMember = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetRoomMemberFromID(Data->RemoteUserId);
			}

			if (pMember != nullptr)
			{
				// TODO_NGMP: Handle reconnection
				if (Data->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_NoConnection)
				{
					pMember->m_connectionState = ENetworkConnectionState::NOT_CONNECTED;
				}
				else if (Data->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_DirectConnection)
				{
					pMember->m_connectionState = ENetworkConnectionState::CONNECTED_DIRECT;
				}
				else if (Data->NetworkType == EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection)
				{
					pMember->m_connectionState = ENetworkConnectionState::CONNECTED_RELAYED;
				}
			}

			// TODO_NGMP: abstract lobbies away better

			// invoke a roster change so the UI updates
			if (pMesh->GetMeshType() == ENetworkMeshType::NETWORK_ROOM)
			{
				if (NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_RosterNeedsRefreshCallback != nullptr)
				{
					NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_RosterNeedsRefreshCallback();
				}
			}
			else if (pMesh->GetMeshType() == ENetworkMeshType::GAME_LOBBY)
			{
				if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_RosterNeedsRefreshCallback != nullptr)
				{
					NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_RosterNeedsRefreshCallback();
				}
			}

			
		});
		*/
}

void NetworkMesh::Tick()
{
	/*
	auto P2PHandle = EOS_Platform_GetP2PInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	uint8_t channelToUse = (uint8_t)m_meshType;

	// recv
	EOS_P2P_GetNextReceivedPacketSizeOptions sizeOptions;
	sizeOptions.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
	sizeOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	sizeOptions.RequestedChannel = &channelToUse; // room mesh channels

	uint32_t numBytes = 0;
	while (EOS_P2P_GetNextReceivedPacketSize(P2PHandle, &sizeOptions, &numBytes) == EOS_EResult::EOS_Success)
	{
		CBitStream bitstream(numBytes);

		EOS_P2P_ReceivePacketOptions options;
		options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
		options.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
		options.MaxDataSizeBytes = numBytes;
		options.RequestedChannel = &channelToUse; // room mesh channels

		EOS_ProductUserId outRemotePeerID = nullptr;
		EOS_P2P_SocketId outSocketID;
		uint8_t outChannel = 0;
		EOS_EResult result = EOS_P2P_ReceivePacket(P2PHandle, &options, &outRemotePeerID, &outSocketID, &outChannel, (void*)bitstream.GetRawBuffer(), &numBytes);

		if (result == EOS_EResult::EOS_Success)
		{
			// TODO_NGMP: Reject any packets from members not in the room? or mesh
			char szEOSUserID[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
			int32_t outLenLocal = sizeof(szEOSUserID);
			EOS_ProductUserId_ToString(outRemotePeerID, szEOSUserID, &outLenLocal);
			NetworkLog("[NGMP]: Received %d bytes from user %s", numBytes, szEOSUserID);

			EPacketID packetID = bitstream.Read<EPacketID>();

			// Game Mesh / Lobby packets only
			if (m_meshType == ENetworkMeshType::GAME_LOBBY)
			{
				if (packetID == EPacketID::PACKET_ID_LOBBY_START_GAME)
				{
					// TODO_NGMP: Ignore if not host sending
					NetworkLog("[NGMP]: Got start game packet from %s", szEOSUserID);

					NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket();
				}
			}
			else if (m_meshType == ENetworkMeshType::NETWORK_ROOM || m_meshType == ENetworkMeshType::GAME_LOBBY)// NetRoom AND Game Mesh / Lobby packets
			{
				if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO)
				{
					NetRoom_HelloPacket helloPacket(bitstream);

					NetworkLog("[NGMP]: Got hello from %s, sending ack", szEOSUserID);
					SendHelloAckMsg(outRemotePeerID);
				}
				else if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO_ACK)
				{
					NetRoom_HelloAckPacket helloAckPacket(bitstream);

					NetworkLog("[NGMP]: Received ack from %s, we're now connected", szEOSUserID);
				}
				else if (packetID == EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG)
				{
					NetRoom_ChatMessagePacket chatPacket(bitstream);

					// TODO_NGMP: Support longer msgs
					NetworkLog("[NGMP]: Received chat message of len %d: %s", chatPacket.GetMsg().length(), chatPacket.GetMsg().c_str());

					// determine the username
					// TODO_NGMP: Use inheritence

					if (m_meshType == ENetworkMeshType::NETWORK_ROOM)
					{
						std::map<EOS_ProductUserId, NetworkRoomMember>& mapRoomMembers = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetMembersListForCurrentRoom();

						if (mapRoomMembers.find(outRemotePeerID) != mapRoomMembers.end())
						{
							UnicodeString str;
							str.format(L"%hs: %hs", mapRoomMembers[outRemotePeerID].m_strName.str(), chatPacket.GetMsg().c_str());
							NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_OnChatCallback(str);
						}
						else
						{
							// TODO_NGMP: Error, user sending us messages isnt in the room
						}
					}
					else if (m_meshType == ENetworkMeshType::GAME_LOBBY)
					{
						// TODO_NGMP: Custom
						/*
						std::map<EOS_ProductUserId, LobbyMember*>& mapRoomMembers = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();

						if (mapRoomMembers.find(outRemotePeerID) != mapRoomMembers.end())
						{
							UnicodeString str;
							str.format(L"%hs: %hs", mapRoomMembers[outRemotePeerID]->m_strName.str(), chatPacket.GetMsg().c_str());
							NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str);
						}
						else
						{
							// TODO_NGMP: Error, user sending us messages isnt in the room
						}
						*/
/*
					}
				}
			}
			
		}
	}
	*/
}
