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

class NetworkMesh;

enum class EScreenshotType : int
{
	SCREENSHOT_TYPE_LOADSCREEN = 0,
	SCREENSHOT_TYPE_GAMEPLAY = 1,
	SCREENSHOT_TYPE_SCORESCREEN = 2
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
	UNUSED_PLACEHOLDER = 8, // this was relay upgrade, was removed. We can re-use it later, but service needs this placeholder
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
	FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST = 28
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
	USER_REQUESTED_SILENT = 2
};

class QoSManager
{
public:
	void Tick();
	void StartProbing(std::map<std::pair<std::string, EQoSRegions>, std::string>& endpoints, std::function<void(void)> cbOnComplete);

	std::string& GetPreferredRegionName() { return m_PreferredRegionName; }
	EQoSRegions GetPreferredRegionID() { return m_PreferredRegionID; }
	int GetPreferredRegionLatency() { return m_PreferredRegionLatency; }
	std::map<EQoSRegions, int>& GetQoSData() { return m_mapQoSData; }

private:
	std::function<void(void)> m_cbCompletion = nullptr;
	std::string m_PreferredRegionName = "Unknown";
	EQoSRegions m_PreferredRegionID = EQoSRegions::UNKNOWN;
	int m_PreferredRegionLatency = -1;

	std::map<std::pair<std::string, EQoSRegions>, std::string> m_mapQoSEndpoints;
	SOCKET m_Socket_QoSProbing = -1;
	int64_t m_timeStartQoS = -1;

	std::map<EQoSRegions, int> m_mapQoSData;

	class QoSProbe
	{
	public:
		EQoSRegions regionID;
		std::string strRegionName;
		std::string strEndpoint;

		int64_t startTime = -1;

		unsigned short Port = -1;
		std::string strIPAddr;

		bool bSent = false;
		bool bDone = false;
		int Latency = -1;

		bool HasTimedOut()
		{
			if (startTime == -1)
			{
				return false;
			}

			const int timeoutMS = 1000;
			int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
			return (currTime - startTime) >= timeoutMS;
		}
	};
	std::vector<QoSProbe> m_lstQoSProbesInFlight;

	const static int MAX_SIZE_IP_ADDR = 16;

};

class WebSocket
{
public:
	WebSocket();
	~WebSocket();
	void Connect(const char* url);
	void Disconnect();

	bool IsConnected()
	{
		return m_bConnected;
	}

	std::vector<char> m_vecWSPartialBuffer;

	void Shutdown();

	void SendData_ChangeName(UnicodeString& strNewName);
	void SendData_RoomChatMessage(UnicodeString& msg, bool bIsAction);
	void SendData_LobbyChatMessage(UnicodeString& msg, bool bIsAction, bool bIsAnnouncement, bool bShowAnnouncementToHost);
	void SendData_JoinNetworkRoom(int roomID);
	void SendData_LeaveNetworkRoom();
	void SendData_MarkReady(bool bReady);

	void SendData_RequestSignalling(int64_t targetUserID);
	void SendData_Signalling(int64_t targetUserID, std::vector<uint8_t> vecPayload);
	void SendData_StartGame();

	void SendData_ChangeLobbyPassword(UnicodeString& strNewPassword);
	void SendData_RemoveLobbyPassword();


	void SendData_CountdownStarted();

	std::function<void(bool)> m_cbOnConnectivityCheckComplete = nullptr;
	void SendData_StartFullMeshConnectivityCheck(std::function<void(bool)> cbOnConnectivityCheckComplete);

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
	CURL* m_pCurl = nullptr;
	bool m_bConnected = false;

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
	
	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ServiceConfig, retry_signalling, use_mapped_port, min_run_ahead_frames, ra_update_frequency_frames, relay_all_traffic,
		ra_slack_percent, frame_grouping_frames, enable_host_migration, network_do_immediate_flush_per_frame, network_send_flags, network_latency_logic_model,
		use_default_config, ra_slack_override_percent_in_default, do_probes, do_replay_upload)
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
		if (m_pOnlineServicesManager == nullptr)
		{
			m_pOnlineServicesManager = new NGMP_OnlineServicesManager();
		}
	}

	static void DestroyInstance()
	{
		if (m_pOnlineServicesManager != nullptr)
		{
			m_pOnlineServicesManager->Shutdown();

			delete m_pOnlineServicesManager;
			m_pOnlineServicesManager = nullptr;
		}
	}

	void CommitReplay(AsciiString absoluteReplayPath);

	static NGMP_OnlineServicesManager* GetInstance()
	{
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
		// IMPORTANT: Delete HTTPManager BEFORE deleting interface objects
		// HTTPManager::Shutdown() will invoke completion callbacks, which may reference interface objects
		// We must ensure interfaces are still valid when callbacks execute
		if (m_pHTTPManager != nullptr)
		{
			delete m_pHTTPManager;
			m_pHTTPManager = nullptr;
		}

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
	static void CaptureScreenshotToDisk();
	static void CaptureScreenshotForProbe(EScreenshotType screenshotType);

	static bool g_bAdvancedNetworkStats;
	static void ToggleAdvancedNetworkStats() { g_bAdvancedNetworkStats = !g_bAdvancedNetworkStats; }
	static bool IsAdvancedNetworkStatsEnabled() { return g_bAdvancedNetworkStats; }

	/*
	NGMP_OnlineServices_AuthInterface* GetAuthInterface() const { return m_pAuthInterface; }
	NGMP_OnlineServices_LobbyInterface* GetLobbyInterface() const { return m_pLobbyInterface; }
	NGMP_OnlineServices_RoomsInterface* GetRoomsInterface() const { return m_pRoomInterface; }
	NGMP_OnlineServices_StatsInterface* GetStatsInterface() const { return m_pStatsInterface; }
	NGMP_OnlineServices_MatchmakingInterface* GetMatchmakingInterface() const { return m_pMatchmakingInterface; }
	*/
	QoSManager& GetQoSManager() { return m_qosMgr; }
	QoSManager m_qosMgr;

	void OnLogin(bool bSuccess, const char* szWSAddr);
	
	void Init();

	void Tick();

	void ProcessMOTD(const char* szMOTD)
	{
		m_strMOTD = std::string(szMOTD);
	}

	std::string& GetMOTD() { return m_strMOTD; }

	void SetPendingFullTeardown(EGOTearDownReason reason) { m_bPendingFullTeardown = true; m_teardownReason = reason; }
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

	ServiceConfig& GetServiceConfig() { return m_ServiceConfig; }

private:
	// main thread SS Upload
	static std::mutex m_ScreenshotMutex;
	static std::vector<std::string> m_vecGuardedSSData;

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

	std::string m_patcher_name;
	std::string m_patcher_path;
	int64_t m_patcher_size;
};
