#pragma once

#include "NGMP_include.h"
#include <ws2ipdef.h>
#include <chrono>
#include <mutex>
#include <steam/steamnetworkingcustomsignaling.h>
#include "PluginInterfaces.h"
#include "Lib/BaseType.h"
#include "GameNetwork/ConnectionStats.h"
#include "GameNetwork/GeneralsOnline/ConnectionRating.h"
#include "GameNetwork/GeneralsOnline/PacketFeedback.h"
#include "GameNetwork/GeneralsOnline/MatchKeys.h"

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
	bool LogInvalidDue();	///< true about every 5 s while the connection stays invalid, the rest of its lines stay out of the log
	int64_t m_nextInvalidLogMs = 0;

	void SendACPacket(const void* pData, uint32_t dataLen);

	void UpdateLatencyHistogram();

	void TickProbe(int64_t nowMs);
	void HandleProbe(const uint8_t* payload, size_t len, int64_t arrivalMs);
	void ResetProbe();
	static int64_t GetArrivalMs(const SteamNetworkingMessage_t* msg);	///< transport receive time in steady_clock ms
	ConnectionStats m_probeStats;

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

	int Recv(SteamNetworkingMessage_t** pMsg, int maxMessages = 255);

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
	float GetSustainedLossPercent(int windowMs);	///< The worse direction of the packet feedback loss from at least 3 reports, -1 if unknown.

	std::string GetStats();

	std::string GetConnectionType();

	void UpdateState(EConnectionState newState, NetworkMesh* pOwningMesh);
	void SetDisconnected(bool bWasError, NetworkMesh* pOwningMesh, bool bIsRetrying);
	
	int64_t m_userID = -1;
	EConnectionType m_ConnectionType = EConnectionType::Unknown;

	EConnectionState m_State = EConnectionState::NOT_CONNECTED;
	
	int m_SignallingAttempts = 0;

	// when signalling for this connection started, for connect-time logging
	int64_t m_connectStartedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	
	int GetLatency();
	PacketFeedback m_feedback;
	size_t ConsumeEnvelope(const uint8_t* data, size_t len);	///< envelope size, 0 if malformed
	int64_t m_lastPacketMs = 0;	///< steady_clock ms of the last enveloped packet, 0 before the first
	int SendEnveloped(ENetworkChannel channel, const uint8_t* payload, size_t len, int sendFlags);	///< returns an EResult
	bool GetOutgoingLoss(int windowMs, PacketFeedback::Summary& summary);	///< our packets to the peer, false without feedback
	bool GetIncomingLoss(int windowMs, PacketFeedback::Summary& summary);	///< the peer's packets to us

	void HandleKeyMessage(const uint8_t* data, size_t len);	///< A match key announcement arrived on this connection.

	int GetJitter();
	int ComputeConnectionRating();	///< 1..5 bars, -1 while it is not known yet; cached for 250 ms.
	int UpdateConnectionRating();
	int64_t m_nextRatingMs = 0;
	int m_ratingResult = -1;

	std::chrono::steady_clock::time_point m_connectedSinceTime = (std::chrono::steady_clock::time_point::min)();
	ConnectionRating::State m_rating;

	// Only set for Steam connections
	HSteamNetConnection m_hSteamConnection = k_HSteamNetConnection_Invalid;

	// Only set for MW connections
	std::string m_strMiddlewareID = std::string("NOT SET");

	void LiteUpdateForAC();

private:
	static constexpr size_t k_probeRingSize = 64;
	static constexpr uint16_t k_noProbeSeq = 0xFFFF;

	struct OutstandingProbe
	{
		uint16_t seq = k_noProbeSeq;	// k_noProbeSeq marks a free slot
		int64_t sendTimeMs = 0;
	};

	OutstandingProbe m_probeRing[k_probeRingSize];	// indexed by seq % k_probeRingSize
	uint16_t m_nextProbeSeq = 0;
	uint16_t m_lastPeerSeq = k_noProbeSeq;	// last probe received from the peer
	int64_t m_lastPeerSeqRecvMs = 0;
	int64_t m_nextProbeMs = 0;
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

	MatchKeys m_matchKeys;

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

	// The signing keys, see MatchKeys.h
	bool BeginLobbyKeys();	///< Makes this client's key, call when it joins or creates a lobby.
	void TickLobbyKeys(int64_t nowMs);	///< Announces the key to the peers that lack it and tells them which keys we hold.
	bool BeginMatchKeys();	///< Binds the keys to the match that is starting, false when a key of the roster is missing.
	void EndMatchKeys();	///< Unbinds from the match, the lobby keys stay.
	void OnKeyAnnouncement(int64_t connectionUserID, const uint8_t* data, size_t len);

	/// A lobby member the host has to wait for before it starts.
	struct LobbyStartBlocker
	{
		int64_t userID;
		bool notConnected;	///< Else the connection is up but the keys are not exchanged with everyone yet.
	};
	void GetLobbyStartBlockers(std::vector<LobbyStartBlocker>& blockers);
	const MatchKeys& GetMatchKeys() const { return m_matchKeys; }
	MatchKeys& GetMatchKeys() { return m_matchKeys; }

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

	struct KeySync
	{
		int64_t nextAnnounceMs = 0;
		int64_t nextStatusMs = 0;
		std::vector<uint8_t> lastStatus;
	};
	std::map<int64_t, KeySync> m_keySync;	///< Per peer, when the key and the held keys were last sent.

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
};
