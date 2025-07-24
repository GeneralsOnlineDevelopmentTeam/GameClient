#pragma once

#include "NGMP_include.h"
#include <ws2ipdef.h>
#include "ValveNetworkingSockets/steam/steamnetworkingsockets.h"

class NetRoom_ChatMessagePacket;

enum class EConnectionState
{
	NOT_CONNECTED,
	CONNECTING_DIRECT,
	FINDING_ROUTE,
	CONNECTED_DIRECT,
	CONNECTION_FAILED,
	CONNECTION_DISCONNECTED
};

// trivial signalling client interface
class ISignalingClient
{
public:
	virtual ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(const SteamNetworkingIdentity& identityPeer, SteamNetworkingErrMsg& errMsg) = 0;

	virtual void Poll() = 0;

	/// Disconnect from the server and close down our polling thread.
	virtual void Release() = 0;
};

class NetworkMesh;
class PlayerConnection
{
public:
	PlayerConnection()
	{
		
	}

	PlayerConnection(int64_t userID, HSteamNetConnection hSteamConnection);

	EConnectionState GetState() const { return m_State; }

	int SendGamePacket(void* pBuffer, uint32_t totalDataSize);

	void Recv();

	std::string GetStats();

	std::string GetConnectionType();

	void UpdateState(EConnectionState newState, NetworkMesh* pOwningMesh);
	void SetDisconnected(bool bWasError, NetworkMesh* pOwningMesh);
	
	int64_t m_userID = -1;

	EConnectionState m_State = EConnectionState::NOT_CONNECTED;
	
	int64_t pingSent = -1;
	
	int GetLatency();

	HSteamNetConnection m_hSteamConnection = k_HSteamNetConnection_Invalid;
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
	NetworkMesh();

	~NetworkMesh()
	{
		if (m_pSignaling != nullptr)
		{
			delete m_pSignaling;
			m_pSignaling = nullptr;
		}
	}

	void UpdateConnectivity(PlayerConnection* connection);

	std::function<void(int64_t, std::string, PlayerConnection*)> m_cbOnConnected = nullptr;
	void RegisterForConnectionEvents(std::function<void(int64_t, std::string, PlayerConnection*)> cb)
	{
		m_cbOnConnected = cb;
	}

	void DeregisterForConnectionEvents()
	{
		m_cbOnConnected = nullptr;
	}

	std::queue<QueuedGamePacket> m_queueQueuedGamePackets;

	bool HasGamePacket();
	QueuedGamePacket RecvGamePacket();
	int SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t userID);

	void SyncConnectionListToLobbyMemberList(std::vector<LobbyMemberEntry> vecLobbyMembers);

	void ConnectToSingleUser(LobbyMemberEntry& member, bool bIsReconnect = false);

	void ConnectToUserViaRelay(Int64 user_id);

	bool ConnectToMesh(LobbyEntry& lobby);

	void Disconnect();

	void Tick();

	HSteamListenSocket GetListenSocketHandle() const { return m_hListenSock; }

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
	std::map<int64_t, PlayerConnection> m_mapConnections;

	ISignalingClient* m_pSignaling = nullptr;

	HSteamListenSocket m_hListenSock = k_HSteamListenSocket_Invalid;
};