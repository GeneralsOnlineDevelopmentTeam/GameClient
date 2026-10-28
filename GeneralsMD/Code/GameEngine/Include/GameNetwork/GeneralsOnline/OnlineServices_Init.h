#pragma once

#include "GameNetwork/NetworkSettings.h"
#include "NGMP_include.h"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <memory>
#include <chrono>

class HTTPManager;

class NGMP_OnlineServices_AuthInterface;
class NGMP_OnlineServices_LobbyInterface;
class NGMP_OnlineServices_RoomsInterface;
class NGMP_OnlineServices_StatsInterface;
class NGMP_OnlineServices_MatchmakingInterface;
class NGMP_OnlineServices_SocialInterface;

class NetworkMesh;

enum class EScreenshotType : int
{
    SCREENSHOT_TYPE_LOADSCREEN = 0,
    SCREENSHOT_TYPE_GAMEPLAY = 1,
    SCREENSHOT_TYPE_SCORESCREEN = 2
};

struct S3ScreenshotEntry
{
    std::vector<uint8_t> vecBytes;
    std::string strSignedURI;
	EScreenshotType screenshotType;
	std::chrono::steady_clock::time_point notBefore; // upload is held back until this time

    bool operator==(const S3ScreenshotEntry& other) const
	{
		return (vecBytes == other.vecBytes && strSignedURI == other.strSignedURI && screenshotType == other.screenshotType);
    }
};

#include <mutex>
#include <atomic>

#include <curl/curl.h>
#include <sentry.h>
#include <chrono>
#include "GeneralsOnline_Settings.h"
#include "GameClient/DisplayStringManager.h"
#include "Common/GameEngine.h"

enum EWebSocketMessageID
{
	UNKNOWN = -1,
	NETWORK_ROOM_CHAT_FROM_CLIENT = 1,
	NETWORK_ROOM_CHAT_FROM_SERVER = 2,
	NETWORK_ROOM_CHANGE_ROOM = 3,
	NETWORK_ROOM_MEMBER_LIST_UPDATE = 4,
	NETWORK_ROOM_MARK_READY = 5,
	LOBBY_CURRENT_LOBBY_UPDATE = 6,
	NETWORK_ROOM_LOBBY_LIST_UPDATE = 7,
	ANTICHEAT_MESSAGE = 8,
	PLAYER_NAME_CHANGE = 9,
	LOBBY_ROOM_CHAT_FROM_CLIENT = 10,
	LOBBY_CHAT_FROM_SERVER = 11,
	NETWORK_SIGNAL = 12,
	START_GAME = 13,
	PING = 14,
	PONG = 15,
	PROBE = 16,
	NETWORK_CONNECTION_START_SIGNALLING = 17,
	NETWORK_CONNECTION_DISCONNECT_PLAYER = 18,
	NETWORK_CONNECTION_CLIENT_REQUEST_SIGNALLING = 19,
	MATCHMAKING_ACTION_JOIN_PREARRANGED_LOBBY = 20,
	MATCHMAKING_ACTION_START_GAME = 21,
	MATCHMAKING_MESSAGE = 22,
	START_GAME_COUNTDOWN_STARTED = 23,
	LOBBY_REMOVE_PASSWORD = 24,
	LOBBY_CHANGE_PASSWORD = 25,
	FULL_MESH_CONNECTIVITY_CHECK_HOST_REQUESTS_BEGIN = 26,
	FULL_MESH_CONNECTIVITY_CHECK_RESPONSE = 27,
	FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST = 28,
	SOCIAL_NEW_FRIEND_REQUEST = 29,
	SOCIAL_FRIEND_CHAT_MESSAGE_CLIENT_TO_SERVER = 30,
	SOCIAL_FRIEND_CHAT_MESSAGE_SERVER_TO_CLIENT = 31,
	SOCIAL_FRIEND_ONLINE_STATUS_CHANGED = 32,
	SOCIAL_SUBSCRIBE_REALTIME_UPDATES = 33,
	SOCIAL_UNSUBSCRIBE_REALTIME_UPDATES = 34,
	SOCIAL_FRIENDS_OVERALL_STATUS_UPDATE = 35,
	SOCIAL_FRIEND_FRIEND_REQUEST_ACCEPTED_BY_TARGET = 36,
	SOCIAL_FRIENDS_LIST_DIRTY = 37,
	SOCIAL_CANT_ADD_FRIEND_LIST_FULL = 38,
	PROBE_RESP = 39,
	AC_REGISTER_PLAYER = 40,
	AC_DEREGISTER_PLAYER = 41,
	WS_KEEPALIVE = 42,
	WS_KEEPALIVE_CLIENT = 43,
	MATCHMAKING_ACTION_REQUEUE = 44,
	MATCHMAKING_ACTION_SETUP_PROGRESS = 45,
	MODERATION_NOTICE = 46,
	MODERATION_COMMAND = 47,
	MODERATION_COMMAND_RESULT = 48
};

enum class EQoSRegions
{
	UNKNOWN = -1,
	WestUS = 0,
	CentralUS = 1,
	WestEurope = 2, 
	SouthCentralUS = 3,
	NorthEurope = 4,
	NorthCentralUS = 5,
	EastUS = 6,
	BrazilSouth = 7,
	AustraliaEast = 8,
	JapanWest = 9,
	AustraliaSoutheast = 10,
	EastAsia = 11,
	JapanEast = 12,
	SoutheastAsia = 13,
	SouthAfricaNorth = 14,
	UaeNorth = 15
};

enum class EGOTearDownReason
{
	UNKNOWN = -1,
	LOST_CONNECTION = 0,
	USER_LOGOUT = 1,
	USER_REQUESTED_SILENT = 2,
	AUTH_FAILED = 3,
	MODERATION_BAN = 4,
	MODERATION_KICK = 5,
	MIDDLEWARE_LOGIN_FAILED = 6
};

constexpr bool IsModerationTeardownReason(EGOTearDownReason reason) noexcept
{
	return reason == EGOTearDownReason::MODERATION_BAN || reason == EGOTearDownReason::MODERATION_KICK;
}

class WebSocket
{
public:
	WebSocket();
	~WebSocket();
	void Connect(const char* url, bool bIsReconnect, std::function<void(void)> fnWebsocketConnectedCallback);
	void Disconnect();

	bool IsConnected()
	{
		return m_bConnected;
	}

	std::vector<char> m_vecWSPartialBuffer;

	std::function<void(void)> m_fnWebsocketConnectedCallback = nullptr;

	void Shutdown();

	void SendData_ChangeName(UnicodeString& strNewName);
	void SendData_RoomChatMessage(UnicodeString& msg, bool bIsAction);
	void SendData_FriendMessage(UnicodeString& msg, int64_t target_user_id);
	void SendData_LobbyChatMessage(UnicodeString& msg, bool bIsAction, bool bIsAnnouncement, bool bShowAnnouncementToHost);
	void SendData_JoinNetworkRoom(int roomID, uint64_t requestID = 0);
	void SendData_LeaveNetworkRoom();
	void SendData_MarkReady(bool bReady);

	void SendData_RequestSignalling(int64_t targetUserID);
	void SendData_Signalling(int64_t targetUserID, std::vector<uint8_t> vecPayload);
	void SendData_StartGame();

	void SendData_ACMessage(int64_t targetUserID, std::vector<uint8_t> vecPayload);

	void SendData_ChangeLobbyPassword(UnicodeString& strNewPassword);
	void SendData_RemoveLobbyPassword();

	void SendData_SubscribeRealtimeUpdates();
	void SendData_UnsubscribeRealtimeUpdates();


	void SendData_CountdownStarted();

	// params: fully connected, missing links, reason ("" on success, else e.g. "missing_connections",
	// "timeout", "member_left", "check_superseded")
	std::function<void(bool, std::list<std::pair<int64_t, int64_t>>, std::string)> m_cbOnConnectivityCheckComplete = nullptr;

	// lobby ID the check was started for; a stale reply (different lobby) is ignored
	int64_t m_connectivityCheckLobbyID = -1;

	void SendData_StartFullMeshConnectivityCheck(std::function<void(bool, std::list<std::pair<int64_t, int64_t>>, std::string)> cbOnConnectivityCheckComplete);

	void ClearConnectivityCheckCallback()
	{
		m_cbOnConnectivityCheckComplete = nullptr;
		m_connectivityCheckLobbyID = -1;
	}

	void Tick();

	int Ping();

	// Queues the message; Tick() flushes it on the main thread. Thread-safe.
	void Send(const char* message);

	// Thread-safe queue of inbound P2P signal payloads; drained by CSignalingClient::Poll().
	void PushPendingSignal(std::vector<uint8_t> payload)
	{
		std::scoped_lock<std::mutex> lock(m_pendingSignalsMutex);

		static constexpr size_t kMaxPendingSignals = 256;
		if (m_pendingSignals.size() >= kMaxPendingSignals)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Pending signal queue full (%zu), discarding oldest signal", m_pendingSignals.size());
			m_pendingSignals.pop();
		}

		m_pendingSignals.push(std::move(payload));
	}

	std::queue<std::vector<uint8_t>> DrainPendingSignals()
	{
		std::scoped_lock<std::mutex> lock(m_pendingSignalsMutex);
		std::queue<std::vector<uint8_t>> drained;
		drained.swap(m_pendingSignals);
		return drained;
	}

private:
	CURL* m_pCurlWS = nullptr;
    CURLM* m_pMulti = nullptr;
	struct curl_slist* m_pHeaders = nullptr;

	bool m_bConnected = false;

	// Menu window should not exceed the service's abandoned-session hold, else reconnects get 205.
	const int64_t m_reconnectMenuWindow = 30000;
	const int64_t m_reconnectBackoffBase = 2000;
	const int64_t m_reconnectBackoffCap = 10000;
	const int64_t m_reconnectAttemptTimeout = 15000;

	bool m_bReconnecting = false;
	int m_numReconnectAttempts = 0;
	int64_t m_reconnectStartTime = -1;
	int64_t m_nextReconnectAttempt = -1;
	int64_t m_reconnectAttemptStarted = -1; // -1 = none in flight
	bool m_bFreshSession = false; // 205: reconnect as a new session

	void BeginReconnect();
	void EndReconnect();
	void UpdateReconnect();
	int64_t ComputeReconnectDelay() const;

	std::string m_strWebsocketAddr;

	int64_t m_lastPong = -1;
	int64_t m_lastPing = -1;
	const int64_t m_timeBetweenUserPings = 1000;
	const int64_t m_timeForWSTimeout = 20000;

	std::atomic<bool> m_bShuttingDown = false;

	// Outbound messages queued for Tick() to flush.
	std::mutex m_outboundQueueMutex;
	std::vector<std::string> m_vecQueuedOutboungMsgs;

	std::mutex m_pendingSignalsMutex;
	std::queue<std::vector<uint8_t>> m_pendingSignals;
};

enum class ERoomFlags : int
{
	ROOM_FLAGS_DEFAULT = 0,
	ROOM_FLAGS_SHOW_ALL_MATCHES = 1
};

class NetworkRoom
{
public:
	NetworkRoom(int roomID, std::string strRoomName, ERoomFlags roomFlags, int parentRoomID = -1)
	{
		m_RoomID = roomID;
		m_strRoomDisplayName.translate(AsciiString(strRoomName.c_str()));
		m_RoomFlags = roomFlags;
		m_ParentRoomID = parentRoomID;
	}

	~NetworkRoom()
	{

	}

	int GetRoomID() const { return m_RoomID; }
	const UnicodeString& GetRoomDisplayName() const { return m_strRoomDisplayName; }
	ERoomFlags GetRoomFlags() const { return m_RoomFlags; }
	int GetParentRoomID() const { return m_ParentRoomID; }

private:
	int m_RoomID;
	UnicodeString m_strRoomDisplayName;
	ERoomFlags m_RoomFlags = ERoomFlags::ROOM_FLAGS_DEFAULT;
	int m_ParentRoomID = -1;
};

struct RegionResponse
{
	std::string countryCode;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RegionResponse, countryCode)
};

struct ServiceConfig
{
	// Peer connections: signalling, NAT traversal, latency probes, and the per-connection ping history
	struct Connection
	{
		bool relay_only = false;
		int ice_engine = 2;             // ICE library: 0 = library default, 1 = native, 2 = WebRTC; the key also takes "default", "native" or "webrtc"
		bool use_mapped_port = true;
		bool retry_signalling = true;
		bool anticheat_transport = false; // game traffic through the anticheat plugin's transport
		int probe_lobby_interval_ms = 250; // time between latency probes sent to each peer while not in a match
		int probe_match_interval_ms = 1000; // the same in a match, the command acks already measure the round trip
		int latency_history_ms = 20000;
	} connection;

	// Not read from the service: the live values
	int network_latency_logic_model = 2;
	bool ibra_ra_tweaks = false;
	float ibra_minslack_default = 0.25f;
	float ibra_maxslack_default = 1.f;
    float ibra_minslack_greaterthan300ms = 0.35f;
    float ibra_maxslack_greaterthan300ms = 1.f;
    float ibra_minslack_greaterthan200ms = 0.3f;
    float ibra_maxslack_greaterthan200ms = 1.f;
	using Send = NetworkSettings::Send;
	using InputDelay = NetworkSettings::InputDelay;
	Send send;
	InputDelay input_delay;

	struct Lobby
	{
		bool host_migration = false;
	} lobby;

	struct Telemetry
	{
		bool screenshot_probes = true;
		bool replay_upload = true;
		int screenshot_width = 960;
		int screenshot_height = 600;
		int screenshot_upload_bytes_per_sec = 131072; // 128 KB/s
		int screenshot_slot_offset_ms = 2000;
	} telemetry;
};

// Reads key into value when present; a missing key keeps the default
template <typename T>
inline void ReadServiceConfigValue(const nlohmann::json& j, const char* key, T& value)
{
	if (!j.is_object())
	{
		throw std::invalid_argument("Service config sections must be objects");
	}
	const auto it = j.find(key);
	if (it != j.end() && !it->is_null())
	{
		value = it->get<T>();
	}
}

// The ICE library by name or by number
inline void ReadIceEngine(const nlohmann::json& j, int& value)
{
	if (!j.is_object())
	{
		throw std::invalid_argument("Service config sections must be objects");
	}
	const auto it = j.find("ice_engine");
	if (it == j.end() || it->is_null())
	{
		return;
	}
	if (it->is_string())
	{
		const std::string name = it->get<std::string>();
		if (name == "default")
		{
			value = 0;
		}
		else if (name == "native")
		{
			value = 1;
		}
		else if (name == "webrtc")
		{
			value = 2;
		}
		return;
	}
	value = it->get<int>();
}

// A key the service sets replaces the compiled default, a missing key keeps it
inline void ReadServiceConfig(const nlohmann::json& j, ServiceConfig& c)
{
	if (j.contains("connection"))
	{
		const nlohmann::json& s = j["connection"];
		ReadServiceConfigValue(s, "relay_only", c.connection.relay_only);
		ReadIceEngine(s, c.connection.ice_engine);
		ReadServiceConfigValue(s, "use_mapped_port", c.connection.use_mapped_port);
		ReadServiceConfigValue(s, "retry_signalling", c.connection.retry_signalling);
		ReadServiceConfigValue(s, "anticheat_transport", c.connection.anticheat_transport);
		ReadServiceConfigValue(s, "probe_lobby_interval_ms", c.connection.probe_lobby_interval_ms);
		ReadServiceConfigValue(s, "probe_match_interval_ms", c.connection.probe_match_interval_ms);
		ReadServiceConfigValue(s, "latency_history_ms", c.connection.latency_history_ms);
	}
	if (j.contains("send"))
	{
		const nlohmann::json& s = j["send"];
		ReadServiceConfigValue(s, "mode", c.send.mode);
		ReadServiceConfigValue(s, "frames_per_packet", c.send.frames_per_packet);
		ReadServiceConfigValue(s, "frames_per_packet_lossy", c.send.frames_per_packet_lossy);
		ReadServiceConfigValue(s, "flush_every_frame", c.send.flush_every_frame);
		ReadServiceConfigValue(s, "redundancy", c.send.redundancy);
		ReadServiceConfigValue(s, "redundancy_window_ms", c.send.redundancy_window_ms);
		ReadServiceConfigValue(s, "redundancy_burst_ms", c.send.redundancy_burst_ms);
		ReadServiceConfigValue(s, "fast_resend", c.send.fast_resend);
	}
	if (j.contains("input_delay"))
	{
		const nlohmann::json& s = j["input_delay"];
		ReadServiceConfigValue(s, "min_frames", c.input_delay.min_frames);
		ReadServiceConfigValue(s, "decision_interval_ms", c.input_delay.decision_interval_ms);
		ReadServiceConfigValue(s, "fps_history_samples", c.input_delay.fps_history_samples);
		ReadServiceConfigValue(s, "slack_percent", c.input_delay.slack_percent);
		if (s.contains("formula"))
		{
			const nlohmann::json& f = s["formula"];
			ReadServiceConfigValue(f, "latency_history_samples", c.input_delay.formula.latency_history_samples);
		}
	}
	if (j.contains("lobby"))
	{
		ReadServiceConfigValue(j["lobby"], "host_migration", c.lobby.host_migration);
	}
	if (j.contains("telemetry"))
	{
		const nlohmann::json& s = j["telemetry"];
		ReadServiceConfigValue(s, "screenshot_probes", c.telemetry.screenshot_probes);
		ReadServiceConfigValue(s, "replay_upload", c.telemetry.replay_upload);
		ReadServiceConfigValue(s, "screenshot_width", c.telemetry.screenshot_width);
		ReadServiceConfigValue(s, "screenshot_height", c.telemetry.screenshot_height);
		ReadServiceConfigValue(s, "screenshot_upload_bytes_per_sec", c.telemetry.screenshot_upload_bytes_per_sec);
		ReadServiceConfigValue(s, "screenshot_slot_offset_ms", c.telemetry.screenshot_slot_offset_ms);
	}
}

// Keeps values from the service inside the ranges the game can run with
inline void ClampServiceConfig(ServiceConfig& c)
{
	c.connection.latency_history_ms = (std::max)(c.connection.latency_history_ms, 1000);
	c.connection.probe_lobby_interval_ms = std::clamp(c.connection.probe_lobby_interval_ms, 50, 5000);
	c.connection.probe_match_interval_ms = std::clamp(c.connection.probe_match_interval_ms, 50, 10000);
	c.send.redundancy_window_ms = std::clamp(c.send.redundancy_window_ms, 0, 5000);
	c.send.redundancy_burst_ms = std::clamp(c.send.redundancy_burst_ms, 0, 1000);
	c.send.frames_per_packet_lossy = std::clamp(c.send.frames_per_packet_lossy, 0, 8);

	c.input_delay.min_frames = std::clamp(c.input_delay.min_frames, 1, 64);
	c.input_delay.slack_percent = std::clamp(c.input_delay.slack_percent, 0, 200);
	c.input_delay.decision_interval_ms = std::clamp(c.input_delay.decision_interval_ms, 100, 60000);
	c.input_delay.fps_history_samples = std::clamp(c.input_delay.fps_history_samples, 1, 128);
	c.input_delay.formula.latency_history_samples = std::clamp(c.input_delay.formula.latency_history_samples, 128, 3600); // FrameMetrics drops pending acks when the window is smaller than the frames in flight
	c.send.frames_per_packet = std::clamp(c.send.frames_per_packet, 1, 16);

	c.telemetry.screenshot_width = std::clamp(c.telemetry.screenshot_width, 16, 4096);
	c.telemetry.screenshot_height = std::clamp(c.telemetry.screenshot_height, 16, 4096);
}

inline void from_json(const nlohmann::json& j, ServiceConfig& c)
{
	if (!j.is_object())
	{
		throw std::invalid_argument("Service config must be an object");
	}
	ReadServiceConfig(j, c);
	ClampServiceConfig(c);
}

class NGMP_OnlineServicesManager
{
private:
	static NGMP_OnlineServicesManager* m_pOnlineServicesManager;

public:

	static GenOnlineSettings Settings;

	

	NGMP_OnlineServicesManager();
	
	enum EEnvironment
	{
		DEV,
		TEST,
		PROD
	};

#if defined(USE_TEST_ENV)
	const static EEnvironment g_Environment = EEnvironment::TEST;
	#pragma message ("Building for TEST environment")
#elif defined(USE_DEBUG_ON_LIVE_SERVER)
	const static EEnvironment g_Environment = EEnvironment::PROD;
#pragma message ("Building for PROD environment (Debug Client)")
#else
	#if defined(_DEBUG)
		const static EEnvironment g_Environment = EEnvironment::DEV;
		#pragma message ("Building for DEV environment")
	#else
		const static EEnvironment g_Environment = EEnvironment::PROD;
		#pragma message ("Building for PROD environment")
	#endif
#endif
	static std::string GetAPIEndpoint(const char* szEndpoint);

	static void CreateInstance()
	{
		std::scoped_lock<std::recursive_mutex> lock(m_singletonMutex);
		if (m_pOnlineServicesManager == nullptr)
		{
			m_pOnlineServicesManager = new NGMP_OnlineServicesManager();
		}
	}

	static void DestroyInstance()
	{
		std::scoped_lock<std::recursive_mutex> lock(m_singletonMutex);
		if (m_pOnlineServicesManager != nullptr)
		{
			m_pOnlineServicesManager->Shutdown();

			delete m_pOnlineServicesManager;
			m_pOnlineServicesManager = nullptr;
		}
	}

	static void AttemptLoadSteam();

	void CommitReplay(AsciiString absoluteReplayPath);

	static std::recursive_mutex m_singletonMutex;
	
	static NGMP_OnlineServicesManager* GetInstance()
	{
		std::scoped_lock<std::recursive_mutex> lock(m_singletonMutex);
		return m_pOnlineServicesManager;
	}

	static std::shared_ptr<WebSocket> GetWebSocket()
	{
		if (m_pOnlineServicesManager != nullptr)
		{
			return m_pOnlineServicesManager->Internal_GetWebSocket();
		}

		return nullptr;
	}

	template<typename T>
	static T* GetInterface()
	{
		// need the root mgr first
		if (m_pOnlineServicesManager != nullptr)
		{
			if constexpr (std::is_same<T, NGMP_OnlineServices_AuthInterface>::value)
			{
				return m_pOnlineServicesManager->m_pAuthInterface;
			}
			else if constexpr (std::is_same<T, NGMP_OnlineServices_LobbyInterface>::value)
			{
				return m_pOnlineServicesManager->m_pLobbyInterface;
			}
			else if constexpr (std::is_same<T, NGMP_OnlineServices_RoomsInterface>::value)
			{
				return m_pOnlineServicesManager->m_pRoomInterface;
			}
			else if constexpr (std::is_same<T, NGMP_OnlineServices_StatsInterface>::value)
			{
				return m_pOnlineServicesManager->m_pStatsInterface;
			}
			else if constexpr (std::is_same<T, NGMP_OnlineServices_MatchmakingInterface>::value)
			{
				return m_pOnlineServicesManager->m_pMatchmakingInterface;
			}
			else if constexpr (std::is_same<T, NGMP_OnlineServices_SocialInterface>::value)
			{
				return m_pOnlineServicesManager->m_pSocialInterface;
			}
		}

		return nullptr;
	}

	static std::thread::id g_MainThreadID;

	static NetworkMesh* GetNetworkMesh();

	void Shutdown();

	void WaitForScreenshotThreads();

	void GetAndParseServiceConfig(std::function<void(void)> cbOnDone);

	~NGMP_OnlineServicesManager()
	{
		if (m_pAuthInterface != nullptr)
		{
			delete m_pAuthInterface;
			m_pAuthInterface = nullptr;
		}

		if (m_pStatsInterface != nullptr)
		{
			delete m_pStatsInterface;
			m_pStatsInterface = nullptr;
		}

		if (m_pLobbyInterface != nullptr)
		{
			delete m_pLobbyInterface;
			m_pLobbyInterface = nullptr;
		}

		if (m_pRoomInterface != nullptr)
		{
			delete m_pRoomInterface;
			m_pRoomInterface = nullptr;
		}

		if (m_pSocialInterface != nullptr)
		{
			delete m_pSocialInterface;
			m_pSocialInterface = nullptr;
		}

		if (m_pHTTPManager != nullptr)
		{
			delete m_pHTTPManager;
			m_pHTTPManager = nullptr;
		}

		// Reset shared_ptr, which will delete WebSocket only when all references are released
		m_pWebSocket.reset();
	}

	void StartVersionCheck(std::function<void(bool bSuccess, bool bNeedsUpdate)> fnCallback);

	std::shared_ptr<WebSocket> Internal_GetWebSocket() const { return m_pWebSocket; }
	HTTPManager* GetHTTPManager() const { return m_pHTTPManager; }

	void CancelUpdate();
	void LaunchPatcher();
	void StartDownloadUpdate(std::function<void(void)> cb);
	void ContinueUpdate();

	static void CaptureScreenshot(bool bResizeForTransmit, std::function<void(std::vector<unsigned char>)> cbOnDataAvailable);
	static void CaptureScreenshotForProbe(EScreenshotType screenshotType, std::string strURI);

	// the network overlay: 0 off, 1 player view (F10), 2 dev view (Shift+F10)
	static int g_networkOverlayMode;
	static void ToggleNetworkOverlay(int mode) { g_networkOverlayMode = (g_networkOverlayMode == mode) ? 0 : mode; }
	static int GetNetworkOverlayMode() { return g_networkOverlayMode; }

	void OnLogin(ELoginResult loginResult, const char* szWSAddr, std::function<void(void)> fnWebsocketConnectedCallback);
	
	void Init();

	void Tick();

	void ProcessMOTD(const char* szMOTD)
	{
		m_strMOTD = std::string(szMOTD);
	}

	std::string& GetMOTD() { return m_strMOTD; }

	void SetPendingFullTeardown(EGOTearDownReason reason)
	{
		if (IsModerationTeardownReason(m_teardownReason) && !IsModerationTeardownReason(reason))
		{
			return;
		}

		m_bPendingFullTeardown = true;
		m_teardownReason = reason;
	}
	bool IsPendingFullTeardown() const { return m_bPendingFullTeardown; }
	EGOTearDownReason GetTeardownReason() const { return m_teardownReason; }
	void ConsumePendingFullTeardown() { m_bPendingFullTeardown = false; }

	void ResetPendingFullTeardownReason() { m_teardownReason = EGOTearDownReason::UNKNOWN; }

	static void InitSentry();
	static void ShutdownSentry();
private:
		
		

		std::string GetPatcherDirectoryPath();

public:
	NGMP_OnlineServices_AuthInterface* m_pAuthInterface = nullptr;
	NGMP_OnlineServices_LobbyInterface* m_pLobbyInterface = nullptr;
	NGMP_OnlineServices_RoomsInterface* m_pRoomInterface = nullptr;
	NGMP_OnlineServices_StatsInterface* m_pStatsInterface = nullptr;
	NGMP_OnlineServices_MatchmakingInterface* m_pMatchmakingInterface = nullptr;
	NGMP_OnlineServices_SocialInterface* m_pSocialInterface = nullptr;

	ServiceConfig& GetServiceConfig() { return m_ServiceConfig; }

public:
	void CacheScreenshotBytes_StartMatch(std::vector<uint8_t> vecData)
	{
		std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
        m_vecCachedScreenshotBytes_MatchStart = std::move(vecData);
	}

	void CacheScreenshotBytes_EndMatch(uint64_t matchID, std::vector<uint8_t> data);
	void CacheReplayBytes(uint64_t matchID, std::vector<uint8_t> data);
    void SetScreenshotS3URI_StartMatch(const char* szURI)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
		m_strCachedScreenshot_MatchStart_S3URI = std::string(szURI);
    }

	void SetScreenshotS3URI_EndMatch(uint64_t matchID, std::string uri);
	void SetScreenshotS3URI_Replay(uint64_t matchID, std::string uri);

private:
	// NOTE: Accessed from multiple threads, dont access directly, use helpers above to lock
    std::string m_strCachedScreenshot_MatchStart_S3URI;

    // screenshots / replays that require caching
    std::vector<uint8_t> m_vecCachedScreenshotBytes_MatchStart;

	struct CachedMatchUpload
	{
		uint64_t dataMatchID = 0;
		uint64_t uriMatchID = 0;
		std::vector<uint8_t> bytes;
		std::string signedURI;
	};

	void CacheMatchUploadBytes(CachedMatchUpload& upload, uint64_t matchID, std::vector<uint8_t> data);
	void CacheMatchUploadURI(CachedMatchUpload& upload, uint64_t matchID, std::string uri);

	// Data and URLs may arrive independently. Keeping their shared match ID here
	// prevents a late response from being paired with media from another match.
	CachedMatchUpload m_cachedMatchEndUpload;
	CachedMatchUpload m_cachedReplayUpload;

	// main thread SS Upload
	static std::mutex m_ScreenshotMutex;

	// normal screenshots
	static std::vector<S3ScreenshotEntry> m_vecGuardedSSData;

	// Screenshot thread management
	std::vector<std::thread*> m_vecScreenshotThreads;
	std::mutex m_mutexScreenshotThreads;

	ServiceConfig m_ServiceConfig;

	HTTPManager* m_pHTTPManager = nullptr;

	std::shared_ptr<WebSocket> m_pWebSocket;

	std::string m_strMOTD;

	EGOTearDownReason m_teardownReason = EGOTearDownReason::UNKNOWN;
	bool m_bPendingFullTeardown = false;

	std::queue<std::string> m_vecFilesToDownload;
	std::queue<int64_t> m_vecFilesSizes;
	std::vector<std::string> m_vecFilesDownloaded;
	std::function<void(void)> m_updateCompleteCallback = nullptr;
	mutable std::recursive_mutex m_updateCallbackMutex;

	std::string m_patcher_name;
	std::string m_patcher_path;
	int64_t m_patcher_size;
};
