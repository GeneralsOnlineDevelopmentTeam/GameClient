#include "GameNetwork/GeneralsOnline/NetworkMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Ping.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Pong.h"
#include "../ngmp_include.h"
#include "../NGMP_interfaces.h"
#include <ws2ipdef.h>
#include "../../NetworkDefs.h"
#include "../../NetworkInterface.h"
#include "GameLogic/GameLogic.h"


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

bool NetworkMesh::SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t user_id)
{
	// TODO_NGMP: Reduce memcpy's done here
	// encrypt
	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	CBitStream bitstream(totalDataSize, (BYTE*)pBuffer, totalDataSize);
	bitstream.Encrypt(currentLobby.EncKey, currentLobby.EncIV);

	ENetPacket* pENetPacket = enet_packet_create((void*)bitstream.GetRawBuffer(), bitstream.GetNumBytesUsed(), ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags


	if (m_mapConnections.contains(user_id))
	{
		int ret = enet_peer_send(m_mapConnections[user_id].m_peer, 1, pENetPacket);

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
	// 
	// TODO_NGMP: Respect vecTargetUsers again
	CBitStream* pBitStream = packet.Serialize();

	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

	ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
		0); // TODO_NGMP: Support flags


	for (auto& connection : m_mapConnections)
	{
		ENetPeer* peer = connection.second.m_peer;

		if (peer != nullptr)
		{
			int ret = enet_peer_send(peer, 0, pENetPacket);

			std::string ip = connection.second.GetIPAddrString();
			if (ret == 0)
			{
				NetworkLog("Packet Sent! %s:%d", ip.c_str(), peer->address.port);
			}
			else
			{
				NetworkLog("Packet Failed To Send! %s:%d", ip.c_str(), peer->address.port);
			}
		}
	}
}

void NetworkMesh::SyncConnectionListToLobbyMemberList(std::vector<LobbyMemberEntry> vecLobbyMembers)
{
	std::vector<int64_t> vecConnectionsToRemove;
	for (auto& connectionData : m_mapConnections)
	{
		int64_t thisConnectionUserID = connectionData.first;

		// do we have a lobby member for it? otherwise disconnect
		bool bFoundLobbyMemberForConnection = false;
		for (LobbyMemberEntry& lobbyMember : vecLobbyMembers)
		{
			if (lobbyMember.IsHuman())
			{
				if (lobbyMember.user_id == thisConnectionUserID)
				{
					bFoundLobbyMemberForConnection = true;
					break;
				}
			}
		}

		if (!bFoundLobbyMemberForConnection)
		{
			NetworkLog("We have a connection open to user id %d, but no lobby member exists, disconnecting", thisConnectionUserID);
			vecConnectionsToRemove.push_back(thisConnectionUserID);
		}
	}

	// now delete + remove from map
	for (int64_t userIDToDisconnect : vecConnectionsToRemove)
	{
		enet_peer_disconnect_now(m_mapConnections[userIDToDisconnect].m_peer, 0);
		m_mapConnections.erase(userIDToDisconnect);
	}
}

void NetworkMesh::ConnectToSingleUser(LobbyMemberEntry& lobbyMember, bool bIsReconnect)
{
	ENetAddress addr;
	enet_address_set_host(&addr, lobbyMember.strIPAddress.c_str());
	addr.port = lobbyMember.preferredPort;

	// TODO_NGMP: error handle on get host ip
#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
	char ip[INET_ADDRSTRLEN + 1] = { 0 };
	enet_address_get_host_ip(&addr, ip, sizeof(ip));
	NetworkLog("Connecting to %s:%d. (pref port was actually %d)", ip, addr.port, lobbyMember.preferredPort);
#endif

	ConnectToSingleUser(addr, lobbyMember.user_id, bIsReconnect);
}

void NetworkMesh::ConnectToSingleUser(ENetAddress addr, Int64 user_id, bool bIsReconnect /*= false*/)
{
	// is it already connected?
	if (m_mapConnections.contains(user_id) && !bIsReconnect)
	{
		NetworkLog("NetworkMesh::ConnectToSingleUser - Duplicate connection for user %lld, not making new connection and returning.", user_id);
		return;
	}

	// is it local?
	if (user_id == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
	{
		// TODO_NGMP: Is this really necessary
		enet_address_set_host(&addr, "127.0.0.1");
	}

	/* Initiate the connection, allocating the 3 channels. */
	ENetPeer* peer = enet_host_connect(enetInstance, &addr, 3, 0);

	// don't care about in-game
	if (!TheGameLogic->isInGame())
	{
		m_connectionCheckGracePeriodStart = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	}
	else
	{
		m_connectionCheckGracePeriodStart = -1;
	}

	// TODO_NGMP: do one last connectivity check before moving to start game
	// TODO_NGMP: do one last lobby data check before moving to start game

	if (peer == nullptr)
	{
		// TODO_NGMP: Better handling
		NetworkLog("No available peers for initiating an ENet connection.");
		return;
	}

	NetworkLog("[SERVER] Connecting to user %lld.\n", user_id);

	// store it
	if (!bIsReconnect)
	{
		m_mapConnections[user_id] = PlayerConnection(user_id, addr, peer);
	}

	m_mapConnections[user_id].m_State = EConnectionState::CONNECTING;

	if (bIsReconnect)
	{
		m_mapConnections[user_id].m_ConnectionAttempts++;
	}
	else
	{
		m_mapConnections[user_id].m_ConnectionAttempts = 1;
	}
	m_mapConnections[user_id].m_lastConnectionAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
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
		NetworkLog("Network Listening on port %d!", server_address.port);
		// TODO_NGMP: Correct values here
		enetInstance = enet_host_create(&server_address,
			32,  // max game size is 8 and we are p2p and fake a connection to ourselves // TODO_NGMP: Do we need to support more, e.g. spectators?
			3,  // 3 channels, 0 is lobby, 1 is gameplay, 2 is handshake
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
		if (lobbyMember.IsHuman())
		{
			ConnectToSingleUser(lobbyMember);
		}
	}
}

void NetworkMesh::Disconnect()
{
	if (enetInstance != nullptr)
	{
		for (auto& connectionData : m_mapConnections)
		{
			ENetPeer* peer = connectionData.second.m_peer;
			if (peer != nullptr)
			{
				enet_peer_disconnect(peer, 0);
				enet_peer_reset(peer);
			}
		}

		m_mapConnections.clear();

		enet_host_destroy(enetInstance);
	}
}

void NetworkMesh::Tick()
{
	// TODO_NGMP: calculate latencies
	Int64 currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	if (currTime - m_lastPing > 1000)
	{
		SendPing();
	}

	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();

	// are we missing players starting 5s after attempting to connect and every 5s afterwards (except for in-game)?
	if (m_connectionCheckGracePeriodStart != -1)
	{
		if (currTime - m_connectionCheckGracePeriodStart >= m_thresoldToCheckConnected)
		{
			// we only care about this in the lobby stage, once in-game, the game handles the connection
			if (!TheGameLogic->isInGame())
			{
				m_connectionCheckGracePeriodStart = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();;

				// NOTE: Host shouldn't do this, why would we leave our own game? let the joining client leave instead
				if (!NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->IsHost())
				{
					int expectedConnections = 0;

					for (const auto& lobbyMember : currentLobby.members)
					{
						if (lobbyMember.IsHuman())
						{
							++expectedConnections; // we should also have a local 'connection'
						}
					}

					int numConnectedConnections = 0;
					for (const auto& connectionData : m_mapConnections)
					{
						if (connectionData.second.m_State == EConnectionState::CONNECTED_DIRECT || connectionData.second.m_State == EConnectionState::CONNECTED_RELAY_1 || connectionData.second.m_State == EConnectionState::CONNECTED_RELAY_2)
						{
							++numConnectedConnections;
						}
					}

					if (numConnectedConnections != expectedConnections)
					{
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnCannotConnectToLobbyCallback();
						return;
					}
				}
			}
			else
			{
				m_connectionCheckGracePeriodStart = -1;
			}
		}
	}

	// service connection attempts
	/*
		m_mapConnections[lobbyMember.user_id].m_State = EConnectionState::CONNECTING;
	m_mapConnections[lobbyMember.user_id].m_ConnectionAttempts = 1;
	m_mapConnections[lobbyMember.user_id].m_lastConnectionAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	*/

	/*
	for (auto& kvPair : m_mapConnections)
	{
		if (kvPair.second.m_State == EConnectionState::CONNECTING)
		{
			int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
			if ((currTime - kvPair.second.m_lastConnectionAttempt) > 1000)
			{
				// TODO_NGMP: Handle connection failure
				kvPair.second.m_ConnectionAttempts++;
				kvPair.second.m_lastConnectionAttempt = currTime;
			}
		}
	}*/

	// tick
	{
		

		ENetEvent event;
		// TODO_NGMP: If we cant connect to someone, log it and leave the lobby
		// TODO_NGMP: Switch to send/recv model isntead of events
		while (enet_host_service(enetInstance, &event, 0) > 0)
		{
			switch (event.type)
			{
			case ENET_EVENT_TYPE_CONNECT:
			{
				// TODO_NGMP: Set a timeout where we remove it, and only accept hello if not connected
#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
				char ip[INET_ADDRSTRLEN + 1] = { 0 };
				if (enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip)) == 0)
				{
					NetworkLog("[SERVER] A new client connected from %s:%u. Starting wait for hello\n",
						ip,
						event.peer->address.port);
				}
#endif

				// send challenge
				Net_ChallengePacket challengePacket;
				CBitStream* pBitStream = challengePacket.Serialize();
				pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

				ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
					ENET_PACKET_FLAG_RELIABLE);

				enet_peer_send(event.peer, 2, pENetPacket);

				break;
			}
				

			case ENET_EVENT_TYPE_RECEIVE:
			{
// 				NetworkLog("[SERVER] A packet of length %u containing %s was received from %s on channel %u.\n",
// 					event.packet->dataLength,
// 					event.packet->data,
// 					event.peer->data,
// 					event.channelID);

				// was it on the game channel? just queue it for generals and bail
				if (event.channelID == 1)
				{
					// find conneciton
					PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

					if (pConnection != nullptr)
					{
						// decrypt
						CBitStream* bitstream = new CBitStream(event.packet->dataLength, event.packet->data, event.packet->dataLength);
						bitstream->Decrypt(currentLobby.EncKey, currentLobby.EncIV);
						//bitstream->ResetOffsetForLocalRead();

						m_queueQueuedGamePackets.push(QueuedGamePacket{ bitstream, pConnection->m_userID });

						enet_packet_destroy(event.packet);
					}
					else
					{
						// TODO_NGMP: Handle
					}
					continue;
				}
				else if (event.channelID == 2) // handshake channel
				{
					CBitStream bitstream(event.packet->data, event.packet->dataLength, (EPacketID)event.packet->data[0]);
					NetworkLog("[NGMP]: Received %d bytes from peer %d on handshake channel", event.packet->dataLength, event.peer->incomingPeerID);


					bitstream.Decrypt(currentLobby.EncKey, currentLobby.EncIV);

					EPacketID packetID = bitstream.Read<EPacketID>();

					if (packetID == EPacketID::PACKET_ID_CHALLENGE) // remote host is challenging us
					{
						// server sends hello ack in response to hello
#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
						char ip[INET_ADDRSTRLEN + 1] = { 0 };
						enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
						NetworkLog("[NGMP]: Got challenge req from %s:%d, sending challenge resp", ip, event.peer->address.port);
#endif

						Net_ChallengePacket challengePacket(bitstream);

						// just send manually to that one user, dont broadcast
						Net_ChallengeRespPacket challengeRespPacket(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
						CBitStream* pBitStream = challengeRespPacket.Serialize();

						pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

						ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
							ENET_PACKET_FLAG_RELIABLE);

						int ret = enet_peer_send(event.peer, 2, pENetPacket);

						if (ret == 0)
						{
							NetworkLog("Packet Sent!");

							// TODO_NGMP: Have a full handshake here, dont just assume we're connected because we sent an ack
							// store the connection
							//m_mapConnections[helloPacket.GetUserID()] = PlayerConnection(helloPacket.GetUserID(), event.peer->address, event.peer);

							//NetworkLog("[NGMP]: Registered connection for user %s:%d (user ID: %d)", ip, event.peer->address.port, helloPacket.GetUserID());
						}
						else
						{
							NetworkLog("Packet Failed To Send!");
						}
					}
					else if (packetID == EPacketID::PACKET_ID_CHALLENGE_RESP)
					{
						Net_ChallengeRespPacket challengeRespPacket(bitstream);

#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
						char ip[INET_ADDRSTRLEN + 1] = { 0 };
						enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
						NetworkLog("[NGMP]: Received ack from %s (user ID: %d), we're now connected", ip, challengeRespPacket.GetUserID());
#endif

						// TODO_NGMP: Have a full handshake here, dont just assume we're connected because we sent an ack
						// store the connection
						m_mapConnections[challengeRespPacket.GetUserID()] = PlayerConnection(challengeRespPacket.GetUserID(), event.peer->address, event.peer);
						m_mapConnections[challengeRespPacket.GetUserID()].m_State = EConnectionState::CONNECTED_DIRECT;

#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
						NetworkLog("[NGMP]: Registered client connection for user %s:%d (user ID: %d)", ip, event.peer->address.port, challengeRespPacket.GetUserID());
#endif
					}

					continue;
				}

				// process
				// TODO_NGMP: Reject any packets from members not in the room? or mesh


				int64_t connUserID = -1;
				PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

				if (pConnection != nullptr)
				{
					connUserID = pConnection->m_userID;
				}

				CBitStream bitstream(event.packet->data, event.packet->dataLength, (EPacketID)event.packet->data[0]);
				NetworkLog("[NGMP]: Received %d bytes from peer %d (user id is %lld)", event.packet->dataLength, event.peer->incomingPeerID, connUserID);

				
				bitstream.Decrypt(currentLobby.EncKey, currentLobby.EncIV);

				EPacketID packetID = bitstream.Read<EPacketID>();

				// Game Mesh / Lobby packets only
				if (m_meshType == ENetworkMeshType::GAME_LOBBY)
				{
					// TODO_NGMP: Determine this all on connect instead of with a handshake, or keep the handshake for hole punching...
					if (packetID == EPacketID::PACKET_ID_LOBBY_START_GAME)
					{
						// TODO_NGMP: Ignore if not host sending
						NetworkLog("[NGMP]: Got start game packet from %d", event.peer->incomingPeerID);

						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket();

						// increase our timeout, Generals has its own timeout code and allows reconnecting, so just set an extremely long value and let the game handle it.
						for (auto& connectionInfo : m_mapConnections)
						{
							enet_peer_timeout(connectionInfo.second.m_peer, 0, 0, 0);
						}
					}
					else if (packetID == EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG)
					{
						NetRoom_ChatMessagePacket chatPacket(bitstream);

						// TODO_NGMP: Support longer msgs
						NetworkLog("[NGMP]: Received chat message of len %d: %s from %d", chatPacket.GetMsg().length(), chatPacket.GetMsg().c_str(), event.peer->incomingPeerID);

						// get host ID
						int64_t localID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();

						PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

						if (pConnection != nullptr)
						{
							auto lobbyUsers = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();
							for (const auto& lobbyUser : lobbyUsers)
							{
								if (lobbyUser.user_id == pConnection->m_userID)
								{
									// if its an announce, dont show it to the sender, they did something locally instead
									if (chatPacket.IsAnnouncement())
									{
										// if its not us, show the message
										if (chatPacket.ShowAnnouncementToHost() || lobbyUser.user_id != localID)
										{
											UnicodeString str;
											str.format(L"%hs", chatPacket.GetMsg().c_str());
											NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str);
										}
									}
									else
									{
										UnicodeString str;
										str.format(L"%hs: %hs", lobbyUser.display_name.c_str(), chatPacket.GetMsg().c_str());
										NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str);
									}


									break;
								}
							}
						}

					}
					else if (packetID == EPacketID::PACKET_ID_PING)
					{
						NetworkLog("Received Ping");
						// send pong
						NetworkPacket_Pong pongPacket;
						CBitStream* pBitStream = pongPacket.Serialize();
						pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

						ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
							ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

						PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

						if (pConnection != nullptr)
						{
							enet_peer_send(pConnection->m_peer, 0, pENetPacket);
						}
					}
					else if (packetID == EPacketID::PACKET_ID_PONG)
					{
						NetworkLog("Received Pong");

						
						// store delta on connection
						PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

						if (pConnection != nullptr)
						{
							int l = currTime - pConnection->pingSent;
							pConnection->latency = currTime - pConnection->pingSent;
							//pConnection->pingSent = -1;

							NetworkLog("Latency for connection to user %lld is %d (var %d)", pConnection->m_userID, pConnection->latency, l);
						}
					}
				}
				
				/* Clean up the packet now that we're done using it. */
				enet_packet_destroy(event.packet);

				break;
			}

			case ENET_EVENT_TYPE_DISCONNECT:
				
				// was it a timeout while attempting to connect?

				PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

				if (pConnection != nullptr)
				{
					if (pConnection->m_State == EConnectionState::CONNECTING)
					{
						NetworkLog("[SERVER] %d timed out while connecting.\n", pConnection->m_userID);

						// remove it, they disconnected
						m_mapConnections.erase(pConnection->m_userID);

						/*
						// should we retry?
						if (pConnection->m_ConnectionAttempts < 5)
						{
							NetworkLog("[SERVER] Attemping to connect to %d, attempt number %d\n", pConnection->m_userID, pConnection->m_ConnectionAttempts+1);
							//ConnectToSingleUser(pConnection->m_address, pConnection->m_userID, true);
						}
						else
						{
							// TODO_NGMP: Leave lobby etc
							NetworkLog("[SERVER] Exhausted retry attempts connecting to %d.\n", pConnection->m_userID);
						}
						*/
					}
					else
					{
						// if the local user requested the disconnect, the connection wont be in the connection map anymore, so we can only hit this code if we disconnected forcefully, or via remote client disconnecting us
						
						// TODO_NGMP: What if the remote client disconnected us, we should have them reject any re-connects
						NetworkLog("[SERVER] %d disconnected.\n", pConnection->m_userID);

						const int numReconnectAttempts = 10;
						if (pConnection->m_ConnectionAttempts < numReconnectAttempts)
						{
							NetworkLog("[SERVER] Attempting to reconnect to %d, attempt number %d\n", pConnection->m_userID, pConnection->m_ConnectionAttempts + 1);

							// if connected, and user is still in lobby, lets try to reconnect
							ConnectToSingleUser(pConnection->m_address, pConnection->m_userID, true);
						}
						else
						{
							// TODO_NGMP: Leave lobby etc
							NetworkLog("[SERVER] Exhausted retry attempts connecting to %d.\n", pConnection->m_userID);

							// remove it, they disconnected
							m_mapConnections.erase(pConnection->m_userID);
						}
					}

				}

				NetworkLog("[SERVER] %s disconnected.\n", event.peer->data);
			}
		}
	}

}

void NetworkMesh::SendPing()
{
	m_lastPing = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// TODO_NGMP: Better way of checking we have everything we need / are fully in the lobby
	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	if (currentLobby.EncKey.empty() || currentLobby.EncIV.empty())
	{
		NetworkLog("No encryption key or IV, not sending ping");
		return;
	}

	for (auto& connectionInfo : m_mapConnections)
	{
		// this also does some hole punching... so don't even check if we're connected, just sent
		NetworkPacket_Ping pingPacket;
		CBitStream* pBitStream = pingPacket.Serialize();
		
		pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

		ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
			ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

		enet_peer_send(connectionInfo.second.m_peer, 0, pENetPacket);

		connectionInfo.second.pingSent = m_lastPing;
	}
}