#include "GameNetwork/GeneralsOnline/NetworkMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"
#include "../ngmp_include.h"
#include "../NGMP_interfaces.h"
#include <ws2ipdef.h>


bool NetworkMesh::HasGamePacket()
{
	return !m_queueQueuedGamePackets.empty();
}

QueuedGamePacket NetworkMesh::RecvGamePacket()
{
	if (HasGamePacket())
	{
		QueuedGamePacket frontPacket = m_queueQueuedGamePackets.front();
		m_queueQueuedGamePackets.pop();
		return frontPacket;
	}

	return QueuedGamePacket();
}

bool NetworkMesh::SendGamePacket(void* pBuffer, uint32_t totalDataSize, LobbyMemberEntry& lobbyMember)
{
	ENetPacket* pENetPacket = enet_packet_create(pBuffer, totalDataSize, ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags


	if (m_mapConnections.contains(lobbyMember.user_id))
	{
		int ret = enet_peer_send(m_mapConnections[lobbyMember.user_id].m_peer, 1, pENetPacket);

		if (ret == 0)
		{
			NetworkLog("Game Packet Sent!");
			return true;
		}
		else
		{
			NetworkLog("Game Packet Failed To Send!");
			return false;
		}
	}

	// TODO_NGMP: Error
	NetworkLog("Packet Failed To Send, client connection not found!");
	return false;
}

void NetworkMesh::SendToMesh(NetworkPacket& packet, std::vector<int64_t> vecTargetUsers)
{
	// TODO_NGMP: Respect vecTargetUsers again
	CBitStream* pBitStream = packet.Serialize();

	ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
		ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

	for (auto& connection : m_mapConnections)
	{
		ENetPeer* peer = connection.second.m_peer;

		if (peer != nullptr)
		{
			int ret = enet_peer_send(peer, 0, pENetPacket);

			if (ret == 0)
			{
				NetworkLog("Packet Sent!");
			}
			else
			{
				NetworkLog("Packet Failed To Send!");
			}
		}
	}

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

void NetworkMesh::ConnectToSingleUser(LobbyMemberEntry& lobbyMember)
{
	//m_mapConnections[lobbyMember.user_id] = PlayerConnection();

	ENetAddress addr;


	// TODO_NGMP: Connect to correct IP + the remote preferred port
	enet_address_set_host(&addr, lobbyMember.strIPAddress.c_str());
	addr.port = lobbyMember.preferredPort;

	// TODO_NGMP: error handle on get host ip
	char ip[INET_ADDRSTRLEN + 1] = { 0 };
	enet_address_get_host_ip(&addr, ip, sizeof(ip));

	/* Initiate the connection, allocating the two channels 0 and 1. */
	ENetPeer* peer = enet_host_connect(enetInstance, &addr, 2, 0);

	if (peer == nullptr)
	{
		// TODO_NGMP: Better handling
		NetworkLog("No available peers for initiating an ENet connection.");
		return;
	}

	// store it
	m_mapConnections[lobbyMember.user_id] = PlayerConnection(lobbyMember.user_id, addr, peer);


}

// TODO_NGMP: enet_deinitialize
// TODO_NGMP: enet_host_destroy(server);
// TODO_NGMP: enet_host_destroy(client);
static bool m_bEnetInitialized = false;
void NetworkMesh::ConnectToMesh(LobbyEntry& lobby)
{
	// TODO_NGMP: Cleanup properly
	m_mapConnections.clear();

	if (!m_bEnetInitialized)
	{
		if (enet_initialize() != 0)
		{
			// TODO_NGMP: Handle error
			NetworkLog("Network Init Failed!");
			m_bEnetInitialized = false;
			return;
		}
		else
		{
			NetworkLog("Network Initialized!");
			m_bEnetInitialized = true;
		}
	}

	// create server
	if (enetInstance == nullptr)
	{
		server_address.host = ENET_HOST_ANY;
		server_address.port = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();

		enetInstance = enet_host_create(&server_address,
			8,  // max game size is 8 and we are p2p and fake a connection to ourselves // TODO_NGMP: Do we need to support more, e.g. spectators?
			2,  // 2 channels, 0 is lobby, 1 is gameplay
			0,
			0);

		if (enetInstance == NULL)
		{
			// TODO_NGMP: Handle error
			NetworkLog("Network Listen Failed!");
			m_bEnetInitialized = false;
			return;
		}
	}

	// now connect to everyone in the lobby
	for (LobbyMemberEntry& lobbyMember : lobby.members)
	{
		ConnectToSingleUser(lobbyMember);
	}
}

void NetworkMesh::Tick()
{
	// tick
	{
		ENetEvent event;

		// TODO_NGMP: Switch to send/recv model isntead of events
		while (enet_host_service(enetInstance, &event, 0) > 0)
		{
			switch (event.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				char ip[INET_ADDRSTRLEN + 1] = { 0 };
				if (enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip)) == 0)
				{
					NetworkLog("[SERVER] A new client connected from %s:%u.\n",
						ip,
						event.peer->address.port);
				}

				// find user
				// TODO_NGMP: What if it isnt found? could be an unauthorized user but could also be someone connecting before we know they're in the lobby
				PlayerConnection* pConnection = GetConnectionForPeer(event.peer);
				if (pConnection != nullptr)
					{
						NetworkLog("Found connection for user %d", pConnection->m_userID);
					}

				break;
			}
				

			case ENET_EVENT_TYPE_RECEIVE:
			{
				NetworkLog("[SERVER] A packet of length %u containing %s was received from %s on channel %u.\n",
					event.packet->dataLength,
					event.packet->data,
					event.peer->data,
					event.channelID);

				// was it on the game channel? just queue it for generals and bail
				if (event.channelID == 1)
				{
					// find conneciton
					PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

					if (pConnection != nullptr)
					{
						m_queueQueuedGamePackets.push(QueuedGamePacket{ event.packet, pConnection->m_userID });
					}
					else
					{
						// TODO_NGMP: Handle
					}
					continue;
				}

				// process
				// TODO_NGMP: Reject any packets from members not in the room? or mesh

				CBitStream bitstream(event.packet->data, event.packet->dataLength, (EPacketID)event.packet->data[0]);
				NetworkLog("[NGMP]: Received %d bytes from user %d", event.packet->dataLength, event.peer->incomingPeerID);

				EPacketID packetID = bitstream.Read<EPacketID>();

				// Game Mesh / Lobby packets only
				if (m_meshType == ENetworkMeshType::GAME_LOBBY)
				{
					// TODO_NGMP: Determine this all on connect instead of with a handshake, or keep the handshake for hole punching...
					if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO)
					{

						// server sends hello ack in response to hello
						char ip[INET_ADDRSTRLEN + 1] = { 0 };
						enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));

						NetRoom_HelloPacket helloPacket(bitstream);

						NetworkLog("[NGMP]: Got hello from %s:%d (user ID: %d), sending ack", ip, event.peer->address.port, helloPacket.GetUserID());

						// just send manually to that one user, dont broadcast
						NetRoom_HelloAckPacket ackPacket(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
						CBitStream* pBitStream = ackPacket.Serialize();

						ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
							ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

						int ret = enet_peer_send(event.peer, 0, pENetPacket);
						if (ret == 0)
						{
							NetworkLog("Packet Sent!");

							// TODO_NGMP: Have a full handshake here, dont just assume we're connected because we sent an ack
							// store the connection
							m_mapConnections[helloPacket.GetUserID()] = PlayerConnection(helloPacket.GetUserID(), event.peer->address, event.peer);

							NetworkLog("[NGMP]: Registered connection for user %s:%d (user ID: %d)", ip, event.peer->address.port, helloPacket.GetUserID());
						}
						else
						{
							NetworkLog("Packet Failed To Send!");
						}
					}
					else if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO_ACK)
					{
						NetRoom_HelloAckPacket helloAckPacket(bitstream);

						char ip[INET_ADDRSTRLEN + 1] = { 0 };
						enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
						NetworkLog("[NGMP]: Received ack from %s (user ID: %d), we're now connected", ip, helloAckPacket.GetUserID());

						// TODO_NGMP: Have a full handshake here, dont just assume we're connected because we sent an ack
						// store the connection
						m_mapConnections[helloAckPacket.GetUserID()] = PlayerConnection(helloAckPacket.GetUserID(), event.peer->address, event.peer);

						NetworkLog("[NGMP]: Registered client connection for user %s:%d (user ID: %d)", ip, event.peer->address.port, helloAckPacket.GetUserID());
					}
					
					else if (packetID == EPacketID::PACKET_ID_LOBBY_START_GAME)
					{
						// TODO_NGMP: Ignore if not host sending
						NetworkLog("[NGMP]: Got start game packet from %d", event.peer->incomingPeerID);

						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket();
					}
					else if (packetID == EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG)
					{
						NetRoom_ChatMessagePacket chatPacket(bitstream);

						// TODO_NGMP: Support longer msgs
						NetworkLog("[NGMP]: Received chat message of len %d: %s from %d", chatPacket.GetMsg().length(), chatPacket.GetMsg().c_str(), event.peer->incomingPeerID);

						// find user
						for (auto& connectionData : m_mapConnections)
						{
							if (connectionData.second.m_peer->address.host == event.peer->address.host
								&& connectionData.second.m_peer->address.port == event.peer->address.port)
							{
								auto lobbyUsers = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();
								for (const auto& lobbyUser : lobbyUsers)
								{
									if (lobbyUser.user_id == connectionData.first)
									{
										UnicodeString str;
										str.format(L"%hs: %hs", lobbyUser.display_name.c_str(), chatPacket.GetMsg().c_str());
										NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str);

										break;
									}
								}

								break;
								//NetworkLog("Found connection for user %d", connectionData.first);
							}
						}

					}
				}
				
				/* Clean up the packet now that we're done using it. */
				enet_packet_destroy(event.packet);

				break;
			}

			case ENET_EVENT_TYPE_DISCONNECT:
				NetworkLog("[SERVER] %s disconnected.\n", event.peer->data);

				/* Reset the peer's client information. */

				event.peer->data = NULL;
			}
		}
	}

}
