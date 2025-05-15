#pragma once

#include "NGMP_include.h"
#include <ws2ipdef.h>

enum class EConnectionState
{
	NOT_CONNECTED,
	CONNECTING,
	CONNECTED_DIRECT,
	CONNECTED_RELAY_1,
	CONNECTED_RELAY_2,
	CONNECTION_FAILED
};

class PlayerConnection
{
public:
	PlayerConnection()
	{

	}


	PlayerConnection(int64_t userID, ENetAddress addr, ENetPeer* peer)
	{
		m_userID = userID;
		m_address = addr;
		m_peer = peer;

		enet_peer_timeout(m_peer, 5, 1000, 1000);
	}

	std::string GetIPAddrString()
	{
#if defined(_DEBUG)
		char ip[INET_ADDRSTRLEN + 1] = { 0 };
		enet_address_get_host_ip(&m_address, ip, sizeof(ip));
		return std::string(ip);
#else
		return std::string("<redacted>");
#endif
	}

	
	int64_t m_userID = -1;
	ENetAddress m_address;
	ENetPeer* m_peer = nullptr;

	EConnectionState m_State = EConnectionState::NOT_CONNECTED;
	int m_ConnectionAttempts = 0;
	int64_t m_lastConnectionAttempt = -1;

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
	}

	~NetworkMesh()
	{

	}

	
	std::queue<QueuedGamePacket> m_queueQueuedGamePackets;

	bool HasGamePacket();
	QueuedGamePacket RecvGamePacket();
	bool SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t userID);

	void SendToMesh(NetworkPacket& packet, std::vector<int64_t> vecTargetUsers);

	void SyncConnectionListToLobbyMemberList(std::vector<LobbyMemberEntry> vecLobbyMembers);

	void ConnectToSingleUser(LobbyMemberEntry& member, bool bIsReconnect = false);
	void ConnectToSingleUser(ENetAddress addr, Int64 user_id, bool bIsReconnect = false);
	void ConnectToMesh(LobbyEntry& lobby);

	void Disconnect();

	void Tick();

	const int64_t m_thresoldToCheckConnected = 2000;
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
		for (auto& connection : m_mapConnections)
		{
			if (connection.second.m_peer->address.host == peer->address.host
				&& connection.second.m_peer->address.port == peer->address.port)
			{
				return &connection.second;
			}
		}
		return nullptr;
	}
private:
	ENetworkMeshType m_meshType;

	ENetAddress server_address;
	ENetHost* enetInstance = nullptr;

	std::map<int64_t, PlayerConnection> m_mapConnections;
};