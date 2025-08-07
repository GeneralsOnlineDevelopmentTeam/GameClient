#pragma once

#include "NGMP_include.h"
#include "PortMapper.h"

class HTTPManager;

class NGMP_OnlineServices_AuthInterface;
class NGMP_OnlineServices_LobbyInterface;
class NGMP_OnlineServices_RoomsInterface;
class NGMP_OnlineServices_StatsInterface;
class NGMP_OnlineServices_MatchmakingInterface;

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
	PLAYER_CONNECTION_RELAY_UPGRADE = 8,
	PLAYER_NAME_CHANGE = 9,
	LOBBY_ROOM_CHAT_FROM_CLIENT = 10,
	LOBBY_CHAT_FROM_SERVER = 11,
	NETWORK_SIGNAL = 12,
	START_GAME = 13
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

	std::string strBuf;

	void Shutdown();

	void SendData_ChangeName(UnicodeString& strNewName);
	void SendData_RoomChatMessage(UnicodeString& msg, bool bIsAction);
	void SendData_LobbyChatMessage(UnicodeString& msg, bool bIsAction, bool bIsAnnouncement, bool bShowAnnouncementToHost);
	void SendData_JoinNetworkRoom(int roomID);
	void SendData_LeaveNetworkRoom();
	void SendData_MarkReady(bool bReady);
	void SendData_ConnectionRelayUpgrade(int64_t userID);

	void SendData_Signalling(const std::string& s);
	void SendData_StartGame();

	void Tick();

	int Ping();

	void Send(const char* message);

	// TODO_STEAM: clear this on connect
	std::queue<std::string> m_pendingSignals;

private:
	CURL* m_pCurl = nullptr;
	bool m_bConnected = false;

	int64_t m_lastPing = -1;
	int64_t m_timeBetweenUserPings = 5000;
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

	static NGMP_OnlineServicesManager* GetInstance()
	{
		return m_pOnlineServicesManager;
	}

	void Shutdown();

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

		if (m_pHTTPManager != nullptr)
		{
			delete m_pHTTPManager;
			m_pHTTPManager = nullptr;
		}

		if (m_pWebSocket != nullptr)
		{
			delete m_pWebSocket;
			m_pWebSocket = nullptr;
		}
	}

	void StartVersionCheck(std::function<void(bool bSuccess, bool bNeedsUpdate)> fnCallback);

	WebSocket* GetWebSocket() const { return m_pWebSocket; }
	HTTPManager* GetHTTPManager() const { return m_pHTTPManager; }

	void CancelUpdate();
	void LaunchPatcher();
	void StartDownloadUpdate(std::function<void(void)> cb);
	void ContinueUpdate();

	NGMP_OnlineServices_AuthInterface* GetAuthInterface() const { return m_pAuthInterface; }
	NGMP_OnlineServices_LobbyInterface* GetLobbyInterface() const { return m_pLobbyInterface; }
	NGMP_OnlineServices_RoomsInterface* GetRoomsInterface() const { return m_pRoomInterface; }
	NGMP_OnlineServices_StatsInterface* GetStatsInterface() const { return m_pStatsInterface; }
	NGMP_OnlineServices_MatchmakingInterface* GetMatchmakingInterface() const { return m_pMatchmakingInterface; }
	QoSManager& GetQoSManager() { return m_qosMgr; }
	QoSManager m_qosMgr;

	void OnLogin(bool bSuccess, const char* szWSAddr);
	
	void Init();

	void Tick();

	PortMapper& GetPortMapper() { return m_PortMapper; }

	void ProcessMOTD(const char* szMOTD)
	{
		m_strMOTD = std::string(szMOTD);
	}

	std::function<void()> m_cbPortMapperCallback = nullptr;
	void RegisterForPortMapperChanges(std::function<void()> cbPortMapper) { m_cbPortMapperCallback = cbPortMapper; }
	void DeregisterForPortMapperChanges() { m_cbPortMapperCallback = nullptr; }

	std::string& GetMOTD() { return m_strMOTD; }

	void SetPendingFullTeardown(EGOTearDownReason reason) { m_bPendingFullTeardown = true; m_teardownReason = reason; }
	bool IsPendingFullTeardown() const { return m_bPendingFullTeardown; }
	EGOTearDownReason GetTeardownReason() const { return m_teardownReason; }
	void ConsumePendingFullTeardown() { m_bPendingFullTeardown = false; }

	void ResetPendingFullTeardownReason() { m_teardownReason = EGOTearDownReason::UNKNOWN; }

private:
		void InitSentry();
		void ShutdownSentry();

		std::string GetPatcherDirectoryPath();

private:
	NGMP_OnlineServices_AuthInterface* m_pAuthInterface = nullptr;
	NGMP_OnlineServices_LobbyInterface* m_pLobbyInterface = nullptr;
	NGMP_OnlineServices_RoomsInterface* m_pRoomInterface = nullptr;
	NGMP_OnlineServices_StatsInterface* m_pStatsInterface = nullptr;
	NGMP_OnlineServices_MatchmakingInterface* m_pMatchmakingInterface = nullptr;
	PortMapper m_PortMapper;

	HTTPManager* m_pHTTPManager = nullptr;

	WebSocket* m_pWebSocket = nullptr;

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