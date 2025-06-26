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
#include "../OnlineServices_RoomsInterface.h"
#include "../json.hpp"
#include "../HTTP/HTTPManager.h"
#include "../OnlineServices_Init.h"


void NetworkMesh::UpdateConnectivity(PlayerConnection* connection)
{
	nlohmann::json j;
	j["target"] = connection->m_userID;
	j["outcome"] = connection->GetState();
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ConnectionOutcome", true);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// dont care about the response
		});
}

void NetworkMesh::OnRelayUpgrade(int64_t targetUserID)
{
	NetworkLog("Performing relay upgrade for user %lld", targetUserID);
	if (m_mapConnections.find(targetUserID) != m_mapConnections.end())
	{
		PlayerConnection& pConnection = m_mapConnections[targetUserID];
		EConnectionState connState = pConnection.GetState();

		if (connState == EConnectionState::NOT_CONNECTED || connState == EConnectionState::CONNECTING_DIRECT)
		{
			NetworkLog("Starting relay upgrade for real");
			ConnectToUserViaRelay(targetUserID);
		}
		else
		{
			NetworkLog("Invalid connection state for relay upgrade (%d)", (int)connState);
		}
	}
	else
	{
		NetworkLog("User was not found for relay upgrade");
	}
}

void NetworkMesh::ProcessChatMessage(NetRoom_ChatMessagePacket& chatPacket, int64_t sendingUserID)
{
	GameSpyColors color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY, true, chatPacket.IsAction());

	// TODO_NGMP: Dont make one string per iter
	int64_t localID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
	auto lobbyUsers = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetMembersListForCurrentRoom();
	for (const auto& lobbyUser : lobbyUsers)
	{
		if (lobbyUser.user_id == sendingUserID)
		{
			// if its an announce, dont show it to the sender, they did something locally instead
			if (chatPacket.IsAnnouncement())
			{
				// if its not us, show the message
				if (chatPacket.ShowAnnouncementToHost() || lobbyUser.user_id != localID)
				{
					UnicodeString str;
					str.format(L"%hs", chatPacket.GetMsg().c_str());

					if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback != nullptr)
					{
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str, color);
					}
				}
			}
			else
			{
				UnicodeString str;
				str.format(L"[%hs] %hs", lobbyUser.display_name.c_str(), chatPacket.GetMsg().c_str());

				if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback != nullptr)
				{
					NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str, color);
				}
			}


			break;
		}
	}
}

void NetworkMesh::ProcessGameStart(Lobby_StartGamePacket& startGamePacket)
{
	if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket != nullptr)
	{
		NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket();
	}

	// increase our timeout, Generals has its own timeout code and allows reconnecting, so just set an extremely long value and let the game handle it.
	for (auto& connectionInfo : m_mapConnections)
	{
		if (connectionInfo.second.m_peer != nullptr)
		{
			enet_peer_timeout(connectionInfo.second.m_peer, 0, 0, 0);
		}
	}
}

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

int NetworkMesh::SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t user_id)
{
	if (m_mapConnections.contains(user_id))
	{
		return m_mapConnections[user_id].SendGamePacket(pBuffer, totalDataSize);
	}
	
	return -2;
}

void NetworkMesh::SendToMesh(NetworkPacket& packet, std::vector<int64_t> vecTargetUsers)
{
	// 
	// TODO_NGMP: Respect vecTargetUsers again

	// TODO_RELAY: Impl this again, right now we serialize PER target, which is wasteful
	/*
	CBitStream* pBitStream = packet.Serialize();

	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

	ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(),
		0); // TODO_NGMP: Support flags
		*/


	for (auto& connection : m_mapConnections)
	{
		//ENetPeer* peer = connection.second.m_peer;

		//if (peer != nullptr)
		{
			int ret = connection.second.SendPacket(packet, 0);


			if (connection.second.m_State == EConnectionState::CONNECTING_RELAY || connection.second.m_State == EConnectionState::CONNECTED_RELAY)
			{
				if (ret == 0)
				{
					NetworkLog("Packet Sent! Via Relay");
				}
				else
				{
					NetworkLog("Packet Failed To Send! Via Relay");
				}
			}
			else
			{
				std::string ip = connection.second.GetIPAddrString();
				ENetPeer* peer = connection.second.m_peer;

				if (ret == 0)
				{
					NetworkLog("Packet Sent! %s:%d", ip.c_str(), peer->address.port);
				}
				else
				{
					NetworkLog("Packet Failed To Send! %s:%d, result was %d", ip.c_str(), peer->address.port, ret);
				}
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
		if (m_mapConnections.find(userIDToDisconnect) != m_mapConnections.end())
		{
			if (m_mapConnections[userIDToDisconnect].m_peer != nullptr)
			{
				enet_peer_disconnect_now(m_mapConnections[userIDToDisconnect].m_peer, 0);
			}
			m_mapConnections.erase(userIDToDisconnect);
		}
	}
}

void NetworkMesh::ConnectToSingleUser(LobbyMemberEntry& lobbyMember, bool bIsReconnect)
{
	// never connect to ourself
	if (lobbyMember.user_id == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
	{
		NetworkLog("NetworkMesh::ConnectToSingleUser - Skipping connection to user %lld - user is local", lobbyMember.user_id);
		return;
	}

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

void NetworkMesh::ConnectToUserViaRelay(Int64 user_id)
{
	NetworkLog("NetworkMesh::ConnectToUserViaRelay - Attempting to connect to user %lld via relay", user_id);

	// update

	// TODO_RELAY: How do we want to handle m_peer?
	//m_mapConnections[user_id].m_peer = nullptr;
	//m_mapConnections[user_id].m_peer = m_pRelayPeer;
	NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_ConnectionRelayUpgrade(user_id);
	m_mapConnections[user_id].m_State = EConnectionState::CONNECTING_RELAY;
	m_mapConnections[user_id].m_ConnectionAttempts = 0;
	m_mapConnections[user_id].m_lastConnectionAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	UpdateConnectivity(&m_mapConnections[user_id]);
	
	NetworkLog("[SERVER] Connecting to user %lld via relay.\n", user_id);

	

	// relay details
	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();

	// find our relay details
	LobbyMemberEntry myLobbyEntry;
	for (LobbyMemberEntry& member : currentLobby.members)
	{
		if (member.user_id == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
		{
			myLobbyEntry = member;
			break;
		}
	}

	// find member
	bool bFoundRelay = false;
	for (LobbyMemberEntry& member : currentLobby.members)
	{
		if (member.user_id == user_id)
		{
			if (!member.strRelayIP.empty() && member.relayPort > 0)
			{
				std::string strRelayIPToUse = std::string();
				uint16_t relayPortToUse = 0;

				// should we use our relay or theirs?
				int slotIdToUseForRelay = m_mapConnectionSelection[myLobbyEntry.m_SlotIndex][member.m_SlotIndex];
				if (slotIdToUseForRelay == myLobbyEntry.m_SlotIndex)
				{
					NetworkLog("Per the relay connection map, using my relay details for connection to %lld", user_id);
					strRelayIPToUse = myLobbyEntry.strRelayIP;
					relayPortToUse = myLobbyEntry.relayPort;
				}
				else
				{
					NetworkLog("Per the relay connection map, using the remote users relay details for connection to %lld", user_id);
					strRelayIPToUse = member.strRelayIP;
					relayPortToUse = member.relayPort;
				}

				ENetAddress addr;
				enet_address_set_host(&addr, strRelayIPToUse.c_str());
				addr.port = relayPortToUse;

				NetworkLog("Attempting to connect to relay. The relay for this lobby pair is %s:%d.", strRelayIPToUse.c_str(), relayPortToUse);

				// Do we already have a peer we can reuse?
				ENetPeer* pExistingRelayPeer = nullptr;
				for (auto connection : m_mapConnections)
				{
					if (connection.second.GetRelayPeer() != nullptr)
					{
						enet_uint16 port = connection.second.GetRelayPeer()->address.port;
						if (port == relayPortToUse)
						{
							char existingRelayIP[INET_ADDRSTRLEN + 1] = { 0 };
							enet_address_get_host_ip(&connection.second.GetRelayPeer()->address, existingRelayIP, sizeof(existingRelayIP));

							if (strcmp(existingRelayIP, strRelayIPToUse.c_str()) == 0)
							{
								pExistingRelayPeer = connection.second.GetRelayPeer();
								break;
							}
						}
					}
				}

				if (pExistingRelayPeer == nullptr)
				{
					NetworkLog("Making a new relay connection...");
					enet_uint32 connectData = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
					ENetPeer* pNewRelayPeer = enet_host_connect(enetInstance, &addr, 4, connectData);

					if (pNewRelayPeer == nullptr)
					{
						// TODO_NGMP: Better handling
						NetworkLog("No available peers for initiating an ENet connection to relay.");
						return;
					}

					m_mapConnections[user_id].m_pRelayPeer = pNewRelayPeer;
					m_mapConnections[user_id].m_peer = nullptr;

					// send challenge
					// TODO_RELAY
					////Net_ChallengePacket challengePacket;
					////m_mapConnections[user_id].SendPacket(challengePacket, 2);
				}
				else
				{
					NetworkLog("Reusing an existing lobby connection");
					m_mapConnections[user_id].m_pRelayPeer = pExistingRelayPeer;
				}


				bFoundRelay = true;
				break;
			}
		}
	}

	if (!bFoundRelay)
	{
		NetworkLog("We need a relay, but couldn't find a relay for user %lld, connection will fail.", user_id);
	}
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
		// dont connect to ourselves
		NetworkLog("NetworkMesh::ConnectToSingleUser - not connecting to ourselves (%lld)", user_id);
		return;
	}
	else
	{

#if defined(GENERALS_ONLINE_FORCE_RELAY_EVERYONE)
		enet_address_set_host(&addr, "127.1.2.3");
		addr.port = 1000 + (rand() % (50000 - 1000 + 1));
#elif defined(GENERALS_ONLINE_FORCE_RELAY_ONE_PLAYER_ONLY)
		//if (user_id == -2)
		if (!m_bDidOneTimeForceRelay)
		{
			m_bDidOneTimeForceRelay = true;
			enet_address_set_host(&addr, "127.1.2.3");
			addr.port = 1000 + (rand() % (50000 - 1000 + 1));
		}
#endif
	}

	/* Initiate the connection, allocating the 3 channels. */
	
	ENetPeer* peer = enet_host_connect(enetInstance, &addr, 3, 0);
	enet_peer_timeout(peer, 3, 1000, 1000);

#if defined(NETWORK_CONNECTION_DEBUG)
	char ip1[INET_ADDRSTRLEN + 1] = { 0 };
	char ip2[INET_ADDRSTRLEN + 1] = { 0 };
	if (enet_address_get_host_ip(&peer->host->receivedAddress, ip1, sizeof(ip1)) == 0 && enet_address_get_host_ip(&peer->host->address, ip2, sizeof(ip2)) == 0)
	{
		NetworkLog("[DEBUG] New enet_host_connect with address %s:%u. and received address %s:%u",
			ip1,
			peer->host->receivedAddress.port,
			ip2,
			peer->address.port);
	}
#endif
	

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
		m_mapConnections[user_id] = PlayerConnection(user_id, addr, peer, true);
	}

	m_mapConnections[user_id].m_State = EConnectionState::CONNECTING_DIRECT;
	UpdateConnectivity(&m_mapConnections[user_id]);

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
bool NetworkMesh::ConnectToMesh(LobbyEntry& lobby)
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
			return false;
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
		NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().ForceReleaseNATPort();

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
			return false;
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

	return true;
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

			// tear down all relay connections, because we are exiting
			ENetPeer* relayPeer = connectionData.second.m_pRelayPeer;
			if (relayPeer != nullptr)
			{
				// set any connections using this to null, relay peers can be shared across connections, but only need to shut things down once
				for (auto& connectionDataInner : m_mapConnections)
				{
					if (connectionDataInner.second.m_pRelayPeer == relayPeer)
					{
						connectionDataInner.second.m_pRelayPeer = nullptr;
					}
				}

				enet_peer_disconnect(relayPeer, 0);
				enet_peer_reset(relayPeer);
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

	// tick connections
	// TODO_RELAY: does this ever get cleaned up
	for (auto& connectionData : m_mapConnections)
	{
		connectionData.second.Tick();
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
							// we should not have a local 'connection'
							if (lobbyMember.user_id != NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
							{
								++expectedConnections;
							}
						}
					}

					int numConnectedConnections = 0;
					for (const auto& connectionData : m_mapConnections)
					{
						if (connectionData.second.m_State == EConnectionState::CONNECTED_DIRECT || connectionData.second.m_State == EConnectionState::CONNECTED_RELAY)
						{
							++numConnectedConnections;
						}
					}

					if (numConnectedConnections != expectedConnections)
					{
						// record fialure
						{
							AsciiString sentryMsg;
							sentryMsg.format("Failed to connect. Saw %d connections, expected %d", numConnectedConnections, expectedConnections);

							// append players in the lobby
							int i = 0;
							sentryMsg.concat("\n\nLobby Members:\n");
							for (const auto& lobbyMember : currentLobby.members)
							{
								AsciiString sentryMsgPlayer;
								sentryMsgPlayer.format("Lobby Member %d = %s [%lld]\n", i, lobbyMember.display_name.c_str(), lobbyMember.user_id);
								sentryMsg.concat(sentryMsgPlayer);
								++i;
							}

							// append who we're really connected to
							i = 0;
							sentryMsg.concat("\n\nLobby Members:\n");
							for (auto& connectionData : m_mapConnections)
							{
								std::string strIPAddr = connectionData.second.GetIPAddrString();

								AsciiString sentryMsgPlayer;
								sentryMsgPlayer.format("Connection %d = UserID: %lld, State: %d, ConnectionAttempts: %d Latency: %d IpPort: %s:%d\n", i, connectionData.second.m_userID, connectionData.second.m_State,
									connectionData.second.m_ConnectionAttempts, connectionData.second.latency, strIPAddr.c_str(), connectionData.second.m_address.port);
								sentryMsg.concat(sentryMsgPlayer);
								++i;
							}

							// local player info
							int64_t userID = -1;
							std::string strDisplayname = "Unknown";
							int64_t lobbyID = -1;
							if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
							{
								userID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
								strDisplayname = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName().str();
								lobbyID = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby().lobbyID;
							}

							sentry_set_extra("user_id", sentry_value_new_int32(userID));
							sentry_set_extra("user_displayname", sentry_value_new_string(strDisplayname.c_str()));
							sentry_set_extra("lobby_id", sentry_value_new_bool(lobbyID));

							// send event to sentry
							sentry_capture_event(sentry_value_new_message_event(
								SENTRY_LEVEL_INFO,
								"CONNECTION_ERROR",
								sentryMsg.str()
							));
						}

						if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnCannotConnectToLobbyCallback != nullptr)
						{
							NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnCannotConnectToLobbyCallback();
						}
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
#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
				char ip[INET_ADDRSTRLEN + 1] = { 0 };
				if (enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip)) == 0)
				{
					NetworkLog("[SERVER] A new client connected from %s:%u. Starting wait for hello",
						ip,
						event.peer->address.port);
				}
#endif

				NetworkLog("[SERVER] Got a new connection, waiting for hello");

				/*
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

				
				*/
				break;
			}
				

			case ENET_EVENT_TYPE_RECEIVE:
			{
// 				NetworkLog("[SERVER] A packet of length %u containing %s was received from %s on channel %u.\n",
// 					event.packet->dataLength,
// 					event.packet->data,
// 					event.peer->data,
// 					event.channelID);

				PlayerConnection* pConnection = nullptr;
				int64_t connUserID = -1;

				// handle relayed packets
				if (event.channelID == 3)
				{
					// get user + channel data

					int64_t sourceUser = *(int64_t*)(event.packet->data + event.packet->dataLength - sizeof(int64_t) - sizeof(int64_t) - sizeof(byte));
					int64_t targetUser = *(int64_t*)(event.packet->data + event.packet->dataLength - sizeof(int64_t) - sizeof(byte));
					byte channel = *(byte*)(event.packet->data + event.packet->dataLength - sizeof(byte));

					// TODO_RELAY: check target user, if not us, ignore

					// used later, in some scenarios
					pConnection = &m_mapConnections[sourceUser];
					connUserID = sourceUser;

					// resize packet

					NetworkLog("Got relayed packed from %lld (%lld) to %lld on channel %d, size was %d", sourceUser, connUserID, targetUser, channel, event.packet->dataLength);

					// correct channel and length
					event.packet->dataLength -= sizeof(int64_t) + sizeof(int64_t) + sizeof(byte);
					event.channelID = channel;
					/*
					byte* dataPointerSource = netEvent.packet->data + netEvent.packet->dataLength - sizeof(byte) - sizeof(Int64) - sizeof(Int64);
                                Int64 sourceUserId = *(Int64*)dataPointerSource;

                                byte* dataPointerTarget = netEvent.packet->data + netEvent.packet->dataLength - sizeof(byte) - sizeof(Int64);
                                Int64 targetUserId = *(Int64*)dataPointerTarget;

                                //Int64 targetUserId = netEvent.packet->data[netEvent.packet->dataLength - sizeof(byte) - sizeof(Int64)];
                                byte originalChannel = netEvent.packet->data[netEvent.packet->dataLength - sizeof(byte)];
								*/
				}
				else // if not relayed, find our real connection
				{
					pConnection = GetConnectionForPeer(event.peer);

					if (pConnection != nullptr)
					{
						connUserID = pConnection->m_userID;
					}
				}

				// was it on the game channel? just queue it for generals and bail
				if (event.channelID == 1)
				{
					// decrypt
					CBitStream* bitstream = new CBitStream(event.packet->dataLength, event.packet->data, event.packet->dataLength);
					bitstream->Decrypt(currentLobby.EncKey, currentLobby.EncIV);
					//bitstream->ResetOffsetForLocalRead();

					NetworkLog("Got game packet source user is %lld", connUserID);
					NetworkLog("Got game packet source user is %lld, of size %lld (bs: %lld), ", connUserID, event.packet->dataLength, bitstream->GetNumBytesAllocated());

					m_queueQueuedGamePackets.push(QueuedGamePacket{ bitstream,connUserID });

					enet_packet_destroy(event.packet);
					continue;
				}
				else if (event.channelID == 2) // handshake channel
				{
					CBitStream bitstream(event.packet->data, event.packet->dataLength, (EPacketID)event.packet->data[0]);
					NetworkLog("[NGMP]: Received %d bytes from peer %d on handshake channel", event.packet->dataLength, event.peer->incomingPeerID);


					bitstream.Decrypt(currentLobby.EncKey, currentLobby.EncIV);

					EPacketID packetID = bitstream.Read<EPacketID>();
					if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO) // remote host is sending us hello
					{
						NetRoom_HelloPacket helloPacket(bitstream);

						// send ack
						NetRoom_HelloAckPacket ackPacket(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
						int ret = m_mapConnections[helloPacket.GetUserID()].SendPacket(ackPacket, 2);
						NetworkLog("Send Hello ret: %d", ret);
					}
					else if (packetID == EPacketID::PACKET_ID_NET_ROOM_HELLO_ACK)
					{
						NetRoom_HelloAckPacket ackPacket(bitstream);

						// we no longer need to send hellos, challenge is the response to hello
						NetworkLog("Stopping sending hellos 1");
						m_mapConnections[ackPacket.GetUserID()].m_bNeedsHelloSent = false;

						if (ackPacket.GetUserID() == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
						{
							continue;
						}

						// only if not already connected
						if (m_mapConnections[ackPacket.GetUserID()].m_State == EConnectionState::CONNECTED_DIRECT || m_mapConnections[ackPacket.GetUserID()].m_State == EConnectionState::CONNECTED_RELAY)
						{
							continue;
						}

#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
						char ip[INET_ADDRSTRLEN + 1] = { 0 };
						enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
						NetworkLog("[NGMP]: Received ack from %s (user ID: %d), we're now connected", ip, ackPacket.GetUserID());
#else // same log, no IP
						NetworkLog("[NGMP]: Received ack from user ID: %d, we're now connected", ackPacket.GetUserID());
#endif


						// TODO_NGMP: Have a full handshake here, dont just assume we're connected because we sent an ack
						// store the connection

						// only do this if it's not a relayed connection
						if (m_mapConnections[ackPacket.GetUserID()].m_State != EConnectionState::CONNECTING_RELAY)
						{
							m_mapConnections[ackPacket.GetUserID()] = PlayerConnection(ackPacket.GetUserID(), event.peer->address, event.peer, false);
						}


						if (m_mapConnections[ackPacket.GetUserID()].m_State == EConnectionState::CONNECTING_RELAY)
						{
							if (m_mapConnections[ackPacket.GetUserID()].m_State != EConnectionState::CONNECTED_RELAY)
							{
								m_mapConnections[ackPacket.GetUserID()].m_State = EConnectionState::CONNECTED_RELAY;
								UpdateConnectivity(&m_mapConnections[ackPacket.GetUserID()]);

								std::string strDisplayName = "Unknown User";
								auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
								for (const auto& member : currentLobby.members)
								{
									if (member.user_id == ackPacket.GetUserID())
									{
										strDisplayName = member.display_name;
										break;
									}
								}

								if (m_cbOnConnected != nullptr)
								{
									m_cbOnConnected(ackPacket.GetUserID(), strDisplayName, EConnectionState::CONNECTED_RELAY);
								}
							}
						}
						else
						{
							if (m_mapConnections[ackPacket.GetUserID()].m_State != EConnectionState::CONNECTED_DIRECT)
							{
								m_mapConnections[ackPacket.GetUserID()].m_State = EConnectionState::CONNECTED_DIRECT;
								UpdateConnectivity(&m_mapConnections[ackPacket.GetUserID()]);

								std::string strDisplayName = "Unknown User";
								auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
								for (const auto& member : currentLobby.members)
								{
									if (member.user_id == ackPacket.GetUserID())
									{
										strDisplayName = member.display_name;
										break;
									}
								}

								if (m_cbOnConnected != nullptr)
								{
									m_cbOnConnected(ackPacket.GetUserID(), strDisplayName, EConnectionState::CONNECTED_DIRECT);
								}
							}
						}

#if defined(_DEBUG) || defined(NETWORK_CONNECTION_DEBUG)
						NetworkLog("[NGMP]: Registered client connection for user %s:%d (user ID: %d)", ip, event.peer->address.port, ackPacket.GetUserID());
#endif
					}

					continue;
				}

				// process
				// TODO_NGMP: Reject any packets from members not in the room? or mesh

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

						Lobby_StartGamePacket startGamePacket(bitstream);
						ProcessGameStart(startGamePacket);
					}
					else if (packetID == EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG)
					{
						NetRoom_ChatMessagePacket chatPacket(bitstream);

						// TODO_NGMP: Support longer msgs
						NetworkLog("[NGMP]: Received chat message of len %d: %s from %d", chatPacket.GetMsg().length(), chatPacket.GetMsg().c_str(), event.peer->incomingPeerID);

						ProcessChatMessage(chatPacket, connUserID);

					}
					else if (packetID == EPacketID::PACKET_ID_PING)
					{
						NetworkLog("Received Ping");
						// send pong
						NetworkPacket_Pong pongPacket;
						if (pConnection != nullptr)
						{
							pConnection->SendPacket(pongPacket, 0);
						}
					}
					else if (packetID == EPacketID::PACKET_ID_PONG)
					{
						NetworkLog("Received Pong");

						// store delta on connection
						
						if (pConnection != nullptr)
						{
							int l = currTime - pConnection->pingSent;
							pConnection->latency = currTime - pConnection->pingSent;
							//pConnection->pingSent = -1;

							// store it on the slot
							int64_t connectionUserID = pConnection->m_userID;
							
							for (Int i = 0; i < MAX_SLOTS; i++)
							{
								NGMPGameSlot* pSlot = (NGMPGameSlot*)TheNGMPGame->getSlot(i);

								if (pSlot != nullptr)
								{
									if (pSlot->m_userID == connectionUserID)
									{
										pSlot->UpdateLatencyFromConnection(AsciiString(""), pConnection->latency);
										
										// TODO_NGMP: Have a separate callback from this? or do we care? it's essentially a lobby update anyway
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface();
										if (pLobbyInterface != nullptr)
										{
											if (pLobbyInterface->m_RosterNeedsRefreshCallback != nullptr)
											{
												// call the callback to update the roster
												pLobbyInterface->m_RosterNeedsRefreshCallback();
											}
										}
									}
								}
							}
							
							NetworkLog("Latency for connection to user %lld is %d (var %d)", pConnection->m_userID, pConnection->latency, l);
						}
					}
				}
				
				/* Clean up the packet now that we're done using it. */
				enet_packet_destroy(event.packet);

				break;
			}

			case ENET_EVENT_TYPE_DISCONNECT:
				
				// TODO_RELAY: How to handle remote user disconnects/timeouts?
				// was it a timeout while attempting to connect?

				PlayerConnection* pConnection = GetConnectionForPeer(event.peer);

				// TODO_RELAY: Handle relay disconnect?
				if (pConnection != nullptr)
				{
					if (pConnection->m_State == EConnectionState::CONNECTING_DIRECT || pConnection->m_State == EConnectionState::CONNECTING_RELAY)
					{
						if (pConnection->m_State == EConnectionState::CONNECTING_DIRECT)
						{
							if (pConnection->m_ConnectionAttempts < 3)
							{
								NetworkLog("[SERVER] Attempting to connect to %d, attempt number %d\n", pConnection->m_userID, pConnection->m_ConnectionAttempts + 1);
								ConnectToSingleUser(pConnection->m_address, pConnection->m_userID, true);
							}
							else
							{
								NetworkLog("[SERVER] %d timed out while connecting directly, attempting to use relay\n", pConnection->m_userID);
								ConnectToUserViaRelay(pConnection->m_userID);
							}
						}
						else
						{
							NetworkLog("[SERVER] %d timed out while connecting via relay, closing connection and throwing error\n", pConnection->m_userID);

							// remove it, they disconnected
							m_mapConnections.erase(pConnection->m_userID);
						}
						

						

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
		connectionInfo.second.SendPacket(pingPacket, 0);

		connectionInfo.second.pingSent = m_lastPing;
	}
}


PlayerConnection::PlayerConnection(int64_t userID, ENetAddress addr, ENetPeer* peer, bool bStartSendingHellosAgain)
{
	m_userID = userID;
	m_address = addr;
	m_peer = peer;
	m_pRelayPeer = nullptr;

	if (bStartSendingHellosAgain)
	{
		NetworkLog("Starting sending hellos 1");
		m_bNeedsHelloSent = true;
	}
	// otherwise, keep whatever start we were in, its just a connection update

	enet_peer_timeout(m_peer, 3, 1000, 1000);

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
	if (pMesh != nullptr)
	{
		pMesh->UpdateConnectivity(this);
	}
}

ENetPeer* PlayerConnection::GetPeerToUse()
{
	if (m_peer == nullptr)
	{
		return m_pRelayPeer;
	}
	
	return m_peer;
}

int PlayerConnection::SendGamePacket(void* pBuffer, uint32_t totalDataSize)
{
	const int gameChannel = 1;

	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	CBitStream bitstream(totalDataSize, (BYTE*)pBuffer, totalDataSize);

	if (m_State == EConnectionState::NOT_CONNECTED
		|| m_State == EConnectionState::CONNECTION_FAILED)
	{
		return -1;
	}
	else if (m_State == EConnectionState::CONNECTING_DIRECT || m_State == EConnectionState::CONNECTED_DIRECT)
	{
		bitstream.Encrypt(currentLobby.EncKey, currentLobby.EncIV);

		ENetPacket* pENetPacket = enet_packet_create((void*)bitstream.GetRawBuffer(), bitstream.GetNumBytesUsed(), ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

		if (m_peer != nullptr)
		{
			return enet_peer_send(m_peer, gameChannel, pENetPacket);
		}
		else
		{
			return -4;
		}
	}
	else if (m_State == EConnectionState::CONNECTING_RELAY || m_State == EConnectionState::CONNECTED_RELAY)
	{
		// use relay peer
		// encrypt the game packet as normal
		bitstream.Encrypt(currentLobby.EncKey, currentLobby.EncIV);

		// repackage with relay header (unencrypted, 9 bytes) + use relay channel
		// grow
		bitstream.GetMemoryBuffer().ReAllocate(bitstream.GetMemoryBuffer().GetAllocatedSize() + 8 + 8 + 1);
		bitstream.Write<int64_t>(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
		bitstream.Write<int64_t>(m_userID);
		bitstream.Write<uint8_t>(gameChannel);


		ENetPacket* pENetPacket = enet_packet_create((void*)bitstream.GetRawBuffer(), bitstream.GetNumBytesUsed(), ENET_PACKET_FLAG_RELIABLE); // TODO_NGMP: Support flags

		// TODO_RELAY: enet_peer_send On failure, the caller still must destroy the packet on its own as ENet has not queued the packet.
		// TODO_RELAY: When relay connection fails too, eventually timeout and leave lobby (only if not host, but what if 2 clients cant connect? one can stay...)
		if (m_pRelayPeer != nullptr)
		{
			return enet_peer_send(m_pRelayPeer, 3, pENetPacket);
		}
		else
		{
			return -5;
		}
	}

	return -3;
}

// TODO_RELAY: determine channel from packet type
int PlayerConnection::SendPacket(NetworkPacket& packet, int channel)
{
	// handshake is allowed during connecting state
	if (channel != 2)
	{
		if (m_State != EConnectionState::CONNECTED_DIRECT && m_State != EConnectionState::CONNECTED_RELAY)
		{
			NetworkLog("WARNING: Attempting to send packet before connected, state is %d", m_State);
			return -7;
		}
	}
	
	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();

	// Allow handshake in all stages
	if ((m_State == EConnectionState::CONNECTING_DIRECT || m_State == EConnectionState::CONNECTED_DIRECT) || ((m_State == EConnectionState::NOT_CONNECTED
		|| m_State == EConnectionState::CONNECTION_FAILED) && channel == 2))
	{
		CBitStream* pBitStream = packet.Serialize();
		pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

		ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(), ENET_PACKET_FLAG_RELIABLE);

		if (m_peer == m_pRelayPeer && m_pRelayPeer != nullptr)
		{
			NetworkLog("Unexpected!");

			if (m_State == EConnectionState::CONNECTING_DIRECT)
			{
				NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_ConnectionRelayUpgrade(m_userID);
				m_State = EConnectionState::CONNECTING_RELAY;
				NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
				if (pMesh != nullptr)
				{
					pMesh->UpdateConnectivity(this);
				}
			}
			else if (m_State == EConnectionState::CONNECTED_DIRECT)
			{
				m_State = EConnectionState::CONNECTED_RELAY;
				NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
				if (pMesh != nullptr)
				{
					pMesh->UpdateConnectivity(this);
				}
			}

			return -4;
		}

		if (m_peer != nullptr)
		{
			return enet_peer_send(m_peer, channel, pENetPacket);
		}
		else
		{
			return -5;
		}
	}
	else if (m_State == EConnectionState::CONNECTING_RELAY || m_State == EConnectionState::CONNECTED_RELAY)
	{
		// use relay peer
		// encrypt the game packet as normal
		CBitStream* pBitStream = packet.Serialize();
		pBitStream->Encrypt(currentLobby.EncKey, currentLobby.EncIV);

		// repackage with relay header (unencrypted, 9 bytes) + use relay channel
		// grow
		pBitStream->GetMemoryBuffer().ReAllocate(pBitStream->GetMemoryBuffer().GetAllocatedSize() + 8 + 8 + 1);
		pBitStream->Write<int64_t>(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
		pBitStream->Write<int64_t>(m_userID);
		pBitStream->Write<uint8_t>(channel);

		ENetPacket* pENetPacket = enet_packet_create((void*)pBitStream->GetRawBuffer(), pBitStream->GetNumBytesUsed(), ENET_PACKET_FLAG_RELIABLE);

		// TODO_RELAY: enet_peer_send On failure, the caller still must destroy the packet on its own as ENet has not queued the packet.
		// TODO_RELAY: When relay connection fails too, eventually timeout and leave lobby (only if not host, but what if 2 clients cant connect? one can stay...)
		if (m_pRelayPeer != nullptr)
		{
			return enet_peer_send(m_pRelayPeer, 3, pENetPacket);
		}
		else
		{
			return -6;
		}
	}

	return -3;
}

void PlayerConnection::Tick()
{
	if (m_bNeedsHelloSent)
	{
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();;
		if (currTime - lastHelloSent >= 1000)
		{
			lastHelloSent = currTime;

			NetworkLog("Sending hello again");

			auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();

			NetRoom_HelloPacket helloPacket(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID());
			
			int ret = SendPacket(helloPacket, 2);
			NetworkLog("Sending hello again, result was %d", ret);
		}


	}
}
