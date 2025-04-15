#pragma once

#include "NGMP_include.h"

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

	void ConnectToSingleUser(LobbyMemberEntry& member, bool bIsReconnect = false);
	void ConnectToSingleUser(ENetAddress addr, Int64 user_id, bool bIsReconnect = false);
	void ConnectToMesh(LobbyEntry& lobby);

	void Disconnect();

	void Tick();

	int64_t m_lastPing = -1;
	void SendPing();

	ENetworkMeshType GetMeshType() const { return m_meshType; }

	//EOS_P2P_SocketId GetSocketID() const { return m_SockID; }


	std::map<int64_t, PlayerConnection>& GetAllConnections()
	{
		return m_mapConnections;
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
	//EOS_P2P_SocketId m_SockID;

	ENetworkMeshType m_meshType;

	ENetAddress server_address;
	ENetHost* enetInstance = nullptr;

	std::map<int64_t, PlayerConnection> m_mapConnections;

	// TODO_NGMP: Everywhere we use notifications, we should check if after creating it it is invalid, if so, error
};