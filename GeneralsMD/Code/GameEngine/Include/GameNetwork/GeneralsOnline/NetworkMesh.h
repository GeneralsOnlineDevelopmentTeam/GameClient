#pragma once

#include "NGMP_include.h"
#include <ws2ipdef.h>
#include <chrono>
#include <mutex>
#include <steam/steamnetworkingcustomsignaling.h>
#include "PluginInterfaces.h"
#include "GameNetwork/GeneralsOnline/MeshConnectionTimeoutPolicy.h"

class NetRoom_ChatMessagePacket;

// trivial signalling client interface
class ISignalingClient
{
public:
	virtual ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(const SteamNetworkingIdentity& identityPeer, SteamNetworkingErrMsg& errMsg) = 0;

	virtual void Poll() = 0;

	/// Disconnect from the server and close down our polling thread.
	virtual void Release() = 0;
};

enum class EConnectionType
{
	Unknown = -1,
	BuiltIn_ValveSockets = 0,
	MiddlewarePluginGeneric = 1
};

class NetworkMesh;
class PlayerConnection
{
public:
 	PlayerConnection()
 	{
		m_ConnectionType = EConnectionType::Unknown;
        m_hSteamConnection = k_HSteamNetConnection_Invalid;
        m_strMiddlewareID = std::string("NOT SET");
 	}

	PlayerConnection(int64_t userID, HSteamNetConnection hSteamConnection);
	PlayerConnection(int64_t userID, const char* szMiddlewareID);

	EConnectionState GetState() const { return m_State; }

	int SendGamePacket(void* pBuffer, uint32_t totalDataSize);

	void SendACPacket(const void* pData, uint32_t dataLen);

	void UpdateLatencyHistogram();

	void Close();

	bool IsIPV4();
	bool IsDirect()
	{
		std::string strConnectionType = GetConnectionType();
		return strConnectionType.find("Relayed") == std::string::npos;
	}

	bool IsValid() const
	{
		return m_State != EConnectionState::NOT_CONNECTED && 
		       m_State != EConnectionState::CONNECTION_FAILED && 
		       m_State != EConnectionState::CONNECTION_DISCONNECTED;
	}

	// a closed connection has nothing to receive
	bool CanRecv() const { return m_hSteamConnection != k_HSteamNetConnection_Invalid; }

	int Recv(SteamNetworkingMessage_t** pMsg);

	int GetHighestHistoricalLatency()
	{
		int highestLatency = 0;
		for (int latencyHistory : m_vecLatencyHistory)
		{
			if (latencyHistory > highestLatency)
			{
				highestLatency = latencyHistory;
			}
		}

		return highestLatency;
	}

	std::vector<int> m_vecLatencyHistory;
	std::vector<float> m_vecQualityHistory;
	std::string GetStats();

	std::string GetConnectionType();

	void UpdateState(EConnectionState newState, NetworkMesh* pOwningMesh);
	void SetDisconnected(bool bWasError, NetworkMesh* pOwningMesh, bool bIsRetrying);
	
	int64_t m_userID = -1;
	EConnectionType m_ConnectionType = EConnectionType::Unknown;

	EConnectionState m_State = EConnectionState::NOT_CONNECTED;
	
	int64_t pingSent = -1;

	int m_SignallingAttempts = 0;
	bool m_retryTimeout = false; // keeps the extended timeouts when signalling restarts

	// when signalling for this connection started, for connect-time logging
	int64_t m_connectStartedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	
	int GetLatency();
	int GetJitter();
	float GetConnectionQuality();
	int ComputeConnectionScore();

	std::chrono::steady_clock::time_point m_connectedSinceTime = (std::chrono::steady_clock::time_point::min)();
	float m_smoothedScore = -1.0f;

	// Only set for Steam connections
	HSteamNetConnection m_hSteamConnection = k_HSteamNetConnection_Invalid;

	// Only set for MW connections
	std::string m_strMiddlewareID = std::string("NOT SET");

	void LiteUpdateForAC();
};

struct LobbyMemberEntry;

struct QueuedGamePacket
{
	CBitStream* m_bs = nullptr;
	int64_t m_userID = -1;
};

// Owns the process-wide GameNetworkingSockets library lifetime.
class NetworkMeshLibrary
{
public:
	// Inits once with userID's identity; stays up until Shutdown (a new login always follows a full teardown).
	// Returns false on failure; caller must not create a listen socket/connection in that case.
	static bool EnsureInitialized(int64_t userID);

	// Tears the library down. Safe to call when not initialized.
	static void Shutdown();

	static bool IsInitialized() { return s_bInitialized; }

	// Drives SteamNetworkingSockets()->RunCallbacks(), independent of NetworkMesh lifetime.
	static void Tick();

private:
	static bool s_bInitialized;
};

class NetworkMesh
{
public:
	NetworkMesh();

	~NetworkMesh()
	{
		Disconnect();

		if (m_pSignaling != nullptr)
		{
			delete m_pSignaling;
			m_pSignaling = nullptr;
		}
	}

	void Flush();

	void RegisterConnectivity(int64_t userID);
	void UpdateConnectivity(PlayerConnection* connection);

	std::function<void(int64_t, std::wstring, PlayerConnection*)> m_cbOnConnected = nullptr;
	void RegisterForConnectionEvents(std::function<void(int64_t, std::wstring, PlayerConnection*)> cb)
	{
		m_cbOnConnected = cb;
	}

	void DeregisterForConnectionEvents()
	{
		m_cbOnConnected = nullptr;
	}

	int getMaximumLatency()
	{
		int highestLatency = 0;

		for (auto& kvPair : m_mapConnections)
		{
			PlayerConnection& conn = kvPair.second;
			if (conn.GetLatency() > highestLatency)
			{
				highestLatency = conn.GetLatency();
			}
		}

		return highestLatency;
	}

	Real getMaximumHistoricalLatency()
	{
		int highestLatency = 0;

		for (auto& kvPair : m_mapConnections)
		{
			PlayerConnection& conn = kvPair.second;
			if (conn.GetHighestHistoricalLatency() > highestLatency)
			{
				highestLatency = conn.GetHighestHistoricalLatency();
			}
		}

		return Real(highestLatency);
	}


	int SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t userID);

	void SendACPacket(uint32_t userID, const void* pData, uint32_t dataLen);

	void ApplyConnectionTimeouts(HSteamNetConnection connection, int64_t userID);
	void StartConnectionSignalling(const char* szMiddlewareID, int64_t remoteUserID, uint16_t preferredPort);

	// A joiner gets its TURN credentials after its mesh exists; signalling waits for them
	void AwaitTurnCredentials();
	void SetTurnCredentials(const std::string& strUsername, const std::string& strToken);

	void DisconnectUser(int64_t remoteUserID);
	void Disconnect();

	void Tick();

	// false if construction failed; callers must not use a mesh that failed to initialize.
	bool IsInitialized() const { return m_bInitialized; }

	HSteamListenSocket GetListenSocketHandle() const { return m_hListenSock; }

	std::map<int64_t, PlayerConnection>& GetAllConnections()
	{
		return m_mapConnections;
	}

	// Thread-safe: may be invoked from anticheat plugin threads; applied on the main thread in Tick
	void UpdateConnectionStateForUser(int64_t userID, EConnectionState newState)
	{
		std::lock_guard<std::mutex> lock(m_pendingStateUpdatesMutex);
		m_vecPendingStateUpdates.emplace_back(userID, newState);
	}

	// drops a peer that left for good so nothing keeps polling it
	void RemoveConnection(int64_t userID);

	PlayerConnection* GetConnectionForUser(int64_t user_id)
	{
		if (m_mapConnections.contains(user_id))
		{
			return &m_mapConnections[user_id];
		}

		return nullptr;
	}


private:
	bool m_bInitialized = false;

	std::map<int64_t, PlayerConnection> m_mapConnections;
	mutable std::recursive_mutex m_mapConnectionsMutex;  // Synchronizes access to m_mapConnections

	std::mutex m_pendingStateUpdatesMutex;
	std::vector<std::pair<int64_t, EConnectionState>> m_vecPendingStateUpdates;

	ISignalingClient* m_pSignaling = nullptr;

	HSteamListenSocket m_hListenSock = k_HSteamListenSocket_Invalid;

	bool m_bDisconnected = false;

	std::string m_strTurnUsername;
	std::string m_strTurnToken;
	std::string m_strTurnUsernameString;
	std::string m_strTurnTokenString;
	std::string m_strTurnServerList;

	struct PendingSignalling
	{
		std::string strMiddlewareID;
		int64_t remoteUserID;
		uint16_t preferredPort;
	};
	bool m_bAwaitingTurnCredentials = false;
	std::vector<PendingSignalling> m_vecSignallingAwaitingTurn;

	// k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_* for this lobby's connections.
	int m_iceEnable = 0;
	int m_iceImplementation = 2;
	int m_reconnectTimeoutMs = MeshConnectionTimeoutPolicy::ReconnectTimeoutMs;
};
