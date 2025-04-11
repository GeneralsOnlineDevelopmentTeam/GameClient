#pragma once

#include "NGMP_include.h"

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
};

struct LobbyMemberEntry;

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

	void SendToMesh(NetworkPacket& packet, std::vector<int64_t> vecTargetUsers);

	void ConnectToSingleUser(LobbyMemberEntry& member);
	void ConnectToMesh(LobbyEntry& lobby);

	void Tick();

	ENetworkMeshType GetMeshType() const { return m_meshType; }

	//EOS_P2P_SocketId GetSocketID() const { return m_SockID; }

private:
	PlayerConnection* GetConnectionForPeer(ENetPeer* peer)
	{
		for (auto& connection : m_mapConnections)
		{
			if (connection.second.m_peer == peer)
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