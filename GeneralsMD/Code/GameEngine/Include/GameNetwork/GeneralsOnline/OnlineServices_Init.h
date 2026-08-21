#pragma once

#include "NGMP_include.h"

#include <thread>
#include <memory>

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

    bool operator==(const S3ScreenshotEntry& other) const
	{
		return (vecBytes == other.vecBytes && strSignedURI == other.strSignedURI && screenshotType == other.screenshotType);
    }
};

#include <mutex>
#include <atomic>

#pragma comment(lib, "libcurl/libcurl.lib")
#pragma comment(lib, "sentry/sentry.lib")


#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/sentry/sentry.h"
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
    MODERATION_ACTION = 46
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
	MODERATION_KICK = 5
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

	std::vector<std::string> m_vecQueuedOutboungMsgs;

	std::function<void(void)> m_fnWebsocketConnectedCallback = nullptr;

	void Shutdown();

	void SendData_ChangeName(UnicodeString& strNewName);
	void SendData_RoomChatMessage(UnicodeString& msg, bool bIsAction);
	void SendData_FriendMessage(UnicodeString& msg, int64_t target_user_id);
	void SendData_LobbyChatMessage(UnicodeString& msg, bool bIsAction, bool bIsAnnouncement, bool bShowAnnouncementToHost);
	void SendData_JoinNetworkRoom(int roomID);
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

	std::function<void(bool, std::list<std::pair<int64_t, int64_t>>)> m_cbOnConnectivityCheckComplete = nullptr;
	void SendData_StartFullMeshConnectivityCheck(std::function<void(bool, std::list<std::pair<int64_t, int64_t>>)> cbOnConnectivityCheckComplete);

	void Tick();

	int Ping();

	void Send(const char* message);

	// TODO_STEAM: clear this on connect
	std::queue<std::vector<uint8_t>> m_pendingSignals;

	bool AcquireLock()
	{
		return m_mutex.try_lock_for(std::chrono::milliseconds(1));
	}

	void ReleaseLock()
	{
		m_mutex.unlock();
	}

private:
	CURL* m_pCurlWS = nullptr;
    CURLM* m_pMulti = nullptr;
	struct curl_slist* m_pHeaders = nullptr;

	bool m_bConnected = false;

    const int maxReconnectAttempts_Frontend = 15;
	const int timeBetweenReconnectAttempts_Frontend = 1000;

	const int maxReconnectAttempts_Ingame = 240;
	const int timeBetweenReconnectAttempts_Ingame = 2500;
	bool m_bReconnecting = false;
    int m_numReconnectAttempts = 0;
    int64_t m_lastReconnectAttempt = -1;

	std::string m_strWebsocketAddr;

	int64_t m_lastPong = -1;
	int64_t m_lastPing = -1;
	const int64_t m_timeBetweenUserPings = 1000;
	const int64_t m_timeForWSTimeout = 20000;

	std::atomic<bool> m_bShuttingDown = false;

	std::recursive_timed_mutex m_mutex;
};

enum class ERoomFlags : int
{
	ROOM_FLAGS_DEFAULT = 0,
	ROOM_FLAGS_SHOW_ALL_MATCHES = 1
};

class NetworkRoom
{
public:
	NetworkRoom(int roomID, std::string strRoomName, ERoomFlags roomFlags)
	{
		m_RoomID = roomID;
		m_strRoomDisplayName.translate(AsciiString(strRoomName.c_str()));
		m_RoomFlags = roomFlags;
	}

	~NetworkRoom()
	{

	}

	int GetRoomID() const { return m_RoomID; }
	UnicodeString GetRoomDisplayName() const { return m_strRoomDisplayName; }
	ERoomFlags GetRoomFlags() const { return m_RoomFlags; }

private:
	int m_RoomID;
	UnicodeString m_strRoomDisplayName;
	ERoomFlags m_RoomFlags = ERoomFlags::ROOM_FLAGS_DEFAULT;
};

struct RegionResponse
{
	std::string countryCode;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(RegionResponse, countryCode)
};

struct ServiceConfig
{
	bool retry_signalling = false;
	bool use_mapped_port = true;
	int min_run_ahead_frames = 4;
	int ra_update_frequency_frames = 10;
	bool relay_all_traffic = false;
	int ra_slack_percent = 20;
	int frame_grouping_frames = 2;
	bool enable_host_migration = true;

	bool network_do_immediate_flush_per_frame = true;
	int network_send_flags = -1;

	int network_latency_logic_model = 0;

	bool use_default_config = false;
	int ra_slack_override_percent_in_default = 10;
	bool do_probes = true;
	bool do_replay_upload = true;

	int network_mesh_histogram_duration = 20000;

	bool ibra_ra_tweaks = false;
	float ibra_minslack_default = 0.25f;
	float ibra_maxslack_default = 1.f;

    float ibra_minslack_greaterthan300ms = 0.35f;
    float ibra_maxslack_greaterthan300ms = 1.f;

    float ibra_minslack_greaterthan200ms = 0.3f;
    float ibra_maxslack_greaterthan200ms = 1.f;

	int screenshot_width = 557;
	int screenshot_height = 333;

	
	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ServiceConfig, retry_signalling, use_mapped_port, min_run_ahead_frames, ra_update_frequency_frames, relay_all_traffic,
		ra_slack_percent, frame_grouping_frames, enable_host_migration, network_do_immediate_flush_per_frame, network_send_flags, network_latency_logic_model,
		use_default_config, ra_slack_override_percent_in_default, do_probes, do_replay_upload, network_mesh_histogram_duration,
		ibra_ra_tweaks, ibra_minslack_default, ibra_maxslack_default, ibra_minslack_greaterthan300ms, ibra_maxslack_greaterthan300ms, ibra_minslack_greaterthan200ms, ibra_maxslack_greaterthan200ms,
		screenshot_width, screenshot_height)
};

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

	static bool g_bAdvancedNetworkStats;
	static void ToggleAdvancedNetworkStats() { g_bAdvancedNetworkStats = !g_bAdvancedNetworkStats; }
	static bool IsAdvancedNetworkStatsEnabled() { return g_bAdvancedNetworkStats; }

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
	void CacheScreenshotBytes_StartMatch(std::vector<uint8_t>& vecData)
	{
		std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
        m_vecCachedScreenshotBytes_MatchStart = vecData;
	}

    void CacheScreenshotBytes_EndMatch(std::vector<uint8_t>& vecData)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
        m_vecCachedScreenshotBytes_MatchEnd = vecData;
    }

    void CacheReplayBytes(std::vector<uint8_t>& vecData)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
		m_vecCachedReplayBytes = vecData;
    }

    void SetScreenshotS3URI_StartMatch(const char* szURI)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
		m_strCachedScreenshot_MatchStart_S3URI = std::string(szURI);
    }

    void SetScreenshotS3URI_EndMatch(const char* szURI)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
        m_strCachedScreenshot_MatchEnd_S3URI = std::string(szURI);
    }

    void SetScreenshotS3URI_Replay(const char* szURI)
    {
        std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
		m_strCacheReplay_S3URI = std::string(szURI);
    }

private:
	// NOTE: Accessed from multiple threads, dont access directly, use helpers above to lock
    std::string m_strCachedScreenshot_MatchStart_S3URI;
    std::string m_strCachedScreenshot_MatchEnd_S3URI;
    std::string m_strCacheReplay_S3URI;

    // screenshots / replays that require caching
    std::vector<uint8_t> m_vecCachedScreenshotBytes_MatchStart;
    std::vector<uint8_t> m_vecCachedScreenshotBytes_MatchEnd;
    std::vector<uint8_t> m_vecCachedReplayBytes;

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
