#pragma once

#include "NGMP_include.h"

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

	void SendHelloMsg(uint64_t targetUser);
	void SendHelloAckMsg(uint64_t targetUser);
	void SendToMesh(NetworkPacket& packet, std::vector<uint64_t> vecTargetUsers);
	void ConnectToMesh(const char* szRoomID);

	void Tick();

	ENetworkMeshType GetMeshType() const { return m_meshType; }

	//EOS_P2P_SocketId GetSocketID() const { return m_SockID; }

private:
	//EOS_P2P_SocketId m_SockID;

	ENetworkMeshType m_meshType;

	// TODO_NGMP: Everywhere we use notifications, we should check if after creating it it is invalid, if so, error
};