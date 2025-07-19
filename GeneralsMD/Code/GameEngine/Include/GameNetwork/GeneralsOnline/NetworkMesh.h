#pragma once

#include "NGMP_include.h"
#include <ws2ipdef.h>

/* NET CHANNELS:
0 = genonline basic traffic - chat etc
1 = generals game traffic
2 = handshake
3 = relay traffic
*/

class NetRoom_ChatMessagePacket;
class Lobby_StartGamePacket;
enum class EConnectionState
{
	NOT_CONNECTED,
	CONNECTING_DIRECT,
	CONNECTING_RELAY,
	CONNECTED_DIRECT,
	CONNECTED_RELAY,
	CONNECTION_FAILED
};

class PlayerConnection
{
public:
	// TODO_RELAY: Add destructor that shuts down peers etc, but only shut down relay peer if not being used by another connection
	PlayerConnection()
	{
		
	}


	PlayerConnection(int64_t userID, ENetAddress addr, ENetPeer* peer, bool bStartSendingHellosAgain);

	EConnectionState GetState() const { return m_State; }
	ENetPeer* GetPeerToUse();
	int SendPacket(NetworkPacket& packet, int channel);
	int SendGamePacket(void* pBuffer, uint32_t totalDataSize);

	std::string GetIPAddrString(bool bForceReveal = false)
	{
#if defined(_DEBUG)
		char ip[INET_ADDRSTRLEN + 1] = { 0 };
		enet_address_get_host_ip(&m_address, ip, sizeof(ip));
		return std::string(ip);
#else
		if (bForceReveal)
		{
			char ip[INET_ADDRSTRLEN + 1] = { 0 };
			enet_address_get_host_ip(&m_address, ip, sizeof(ip));
			return std::string(ip);
		}
		else
		{
			return std::string("<redacted>");
		}
#endif
	}

	ENetPeer* GetRelayPeer()
	{
		return m_pRelayPeer;
	}

	void Tick();

	
	int64_t m_userID = -1;
	ENetAddress m_address;
	ENetPeer* m_peer = nullptr;

	ENetPeer* m_pRelayPeer = nullptr;

	EConnectionState m_State = EConnectionState::NOT_CONNECTED;
	int m_ConnectionAttempts = 0;
	int64_t m_lastConnectionAttempt = -1;

	int64_t lastHelloSent = -1;
	bool m_bNeedsHelloSent = true;

	int64_t pingSent = -1;
	int latency = -1;
};

struct LobbyMemberEntry;

struct QueuedGamePacket
{
	CBitStream* m_bs = nullptr;
	int64_t m_userID = -1;
};

class NetworkMesh
{
public:
	NetworkMesh(ENetworkMeshType meshType)
	{
		m_meshType = meshType;

		// generate the map
		for (int src = 0; src < 8; ++src)
		{
			for (int target = 0; target < 8; ++target)
			{
				if (src == target) // no self connections
				{
					m_mapConnectionSelection[src][target] = -1;
				}
				else
				{
					// only if not set already
					if (m_mapConnectionSelection[src][target] == -2 && m_mapConnectionSelection[target][src] == -2)
					{
						bool bUseSrcRelay = (src % 2 == 0);

						m_mapConnectionSelection[src][target] = bUseSrcRelay ? src : target;
						m_mapConnectionSelection[target][src] = bUseSrcRelay ? src : target;
					}
				}
			}
		}

		// debug
#if defined(_DEBUG)
		for (int src = 0; src < 8; ++src)
		{
			for (int target = 0; target < 8; ++target)
			{
				if (m_mapConnectionSelection[src][target] == -2)
				{
					__debugbreak();
				}

				if (m_mapConnectionSelection[src][target] != m_mapConnectionSelection[target][src])
				{
					__debugbreak();
				}
			}
		}
#endif
	}

	~NetworkMesh()
	{

	}

	void Flush()
	{
		if (enetInstance != nullptr)
		{
			enet_host_flush(enetInstance);
		}
	}

	void UpdateConnectivity(PlayerConnection* connection);

	void OnRelayUpgrade(int64_t targetUserID);

	// users need to use the SAME relay, this mapping ensures they connect to the same one.
		// Which one is selected doesn't really matter for latency, but we need to do send/recv on the same route
		// user slot ID to use is stored in each field, if its us, use ours, otherwise use the other persons
	int m_mapConnectionSelection[8][8] =
	{
		//	  |0| |1| |2| |3| |4| |5| |6| |7| // player 0 to 7
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 0
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 1
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 2
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 3
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 4
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 5
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 6
			{ -2, -2, -2, -2, -2, -2, -2, -2 }, // player 7
	};

	std::function<void(int64_t, std::string, EConnectionState)> m_cbOnConnected = nullptr;
	void RegisterForConnectionEvents(std::function<void(int64_t, std::string, EConnectionState)> cb)
	{
		m_cbOnConnected = cb;
	}

	void DeregisterForConnectionEvents()
	{
		m_cbOnConnected = nullptr;
	}

	void ProcessGameStart(Lobby_StartGamePacket& startGamePacket);

	std::queue<QueuedGamePacket> m_queueQueuedGamePackets;

	bool HasGamePacket();
	QueuedGamePacket RecvGamePacket();
	int SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t userID);

	void SendToMesh(NetworkPacket& packet, std::vector<int64_t> vecTargetUsers);

	void SyncConnectionListToLobbyMemberList(std::vector<LobbyMemberEntry> vecLobbyMembers);

	void ConnectToSingleUser(LobbyMemberEntry& member, bool bIsReconnect = false);
	void ConnectToSingleUser(ENetAddress addr, Int64 user_id, bool bIsReconnect = false);

	void ConnectToUserViaRelay(Int64 user_id);

	bool ConnectToMesh(LobbyEntry& lobby);

	void Disconnect();

	void Tick();

	const int64_t m_thresoldToCheckConnected = 10000;
	int64_t m_connectionCheckGracePeriodStart = -1;

	int64_t m_lastPing = -1;
	void SendPing();

	ENetworkMeshType GetMeshType() const { return m_meshType; }

	std::map<int64_t, PlayerConnection>& GetAllConnections()
	{
		return m_mapConnections;
	}

	PlayerConnection* GetConnectionForUser(int64_t user_id)
	{
		if (m_mapConnections.contains(user_id))
		{
			return &m_mapConnections[user_id];
		}

		return nullptr;
	}


private:
	PlayerConnection* GetConnectionForPeer(ENetPeer* peer)
	{
		// TODO_RELAY: need to update how this works
		for (auto& connection : m_mapConnections)
		{
			if (connection.second.m_peer != nullptr)
			{
				if (connection.second.m_peer->address.host == peer->address.host
					&& connection.second.m_peer->address.port == peer->address.port)
				{
					return &connection.second;
				}
			}
		}

		return nullptr;
	}

private:
	ENetworkMeshType m_meshType;

	ENetAddress server_address;
	ENetHost* enetInstance = nullptr;

	std::map<int64_t, PlayerConnection> m_mapConnections;

#if defined(GENERALS_ONLINE_FORCE_RELAY_ONE_PLAYER_ONLY)
	bool m_bDidOneTimeForceRelay = false;
#endif
};