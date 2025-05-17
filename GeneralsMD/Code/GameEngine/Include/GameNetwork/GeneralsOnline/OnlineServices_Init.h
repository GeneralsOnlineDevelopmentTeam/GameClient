#pragma once

#include "NGMP_include.h"
#include "PortMapper.h"

class HTTPManager;

class NGMP_OnlineServices_AuthInterface;
class NGMP_OnlineServices_LobbyInterface;
class NGMP_OnlineServices_RoomsInterface;
class NGMP_OnlineServices_StatsInterface;

#pragma comment(lib, "libcurl/libcurl.lib")
#pragma comment(lib, "sentry/sentry.lib")


#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include "GameNetwork/GeneralsOnline/Vendor/sentry/sentry.h"

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

	void Shutdown();

	void SendData_RoomChatMessage(const char* szMessage);
	void SendData_JoinNetworkRoom(int roomID);
	void SendData_LeaveNetworkRoom();
	void SendData_MarkReady(bool bReady);

	void Tick();

	int Ping();

	void Send(const char* message);

private:
	CURL* m_pCurl = nullptr;
	bool m_bConnected = false;

	int64_t m_lastPing = -1;
	int64_t m_timeBetweenUserPings = 5000;
};

class NetworkRoom
{
public:
	NetworkRoom(int roomID, std::string strRoomName)
	{
		m_RoomID = roomID;
		m_strRoomDisplayName.translate(AsciiString(strRoomName.c_str()));
	}

	~NetworkRoom()
	{

	}

	int GetRoomID() const { return m_RoomID; }
	UnicodeString GetRoomDisplayName() const { return m_strRoomDisplayName; }

private:
	int m_RoomID;
	UnicodeString m_strRoomDisplayName;
};

enum NGMP_ENATType : uint8_t
{
	NAT_TYPE_UNDETERMINED,
	NAT_TYPE_OPEN,
	NAT_TYPE_MODERATE,
	NAT_TYPE_STRICT
};

class NGMP_OnlineServicesManager
{
private:
	static NGMP_OnlineServicesManager* m_pOnlineServicesManager;

public:

	NGMP_OnlineServicesManager();
	
	enum EEnvironment
	{
		DEV,
		PROD
	};

#if defined(_DEBUG)
	const static EEnvironment g_Environment = EEnvironment::DEV;
	#pragma message ("Building for DEV environment")
#else
	const static EEnvironment g_Environment = EEnvironment::PROD;
	#pragma message ("Building for PROD environment")
#endif
	static std::string GetAPIEndpoint(const char* szEndpoint, bool bAttachToken);

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

	void OnLogin(bool bSuccess, const char* szWSAddr, const char* szWSToken);
	
	void Init();

	void Tick();

	PortMapper& GetPortMapper() { return m_PortMapper; }

	void ProcessMOTD(const char* szMOTD)
	{
		m_strMOTD = std::string(szMOTD);
	}

	std::function<void(NGMP_ENATType previousNATType, NGMP_ENATType newNATType)> m_cbNATTypeChanged = nullptr;
	void RegisterForNATTypeChanges(std::function<void(NGMP_ENATType previousNATType, NGMP_ENATType newNATType)> cbNATTypeChanged) { m_cbNATTypeChanged = cbNATTypeChanged; }

	void CacheNATType(NGMP_ENATType natType)
	{
		NGMP_ENATType oldNATType = m_NATType;
		
		m_NATType = natType;

		if (m_cbNATTypeChanged != nullptr)
		{
			m_cbNATTypeChanged(oldNATType, m_NATType);
		}
	}
	NGMP_ENATType GetNATType() const { return m_NATType; }
	AsciiString GetNATTypeString() const
	{
		switch (m_NATType)
		{
		default:
		case NGMP_ENATType::NAT_TYPE_UNDETERMINED:
			return AsciiString("Undetermined");

		case NGMP_ENATType::NAT_TYPE_OPEN:
			return AsciiString("Open");

		case NGMP_ENATType::NAT_TYPE_MODERATE:
			return AsciiString("Moderate");

		case NGMP_ENATType::NAT_TYPE_STRICT:
			return AsciiString("Strict");
		}

		return AsciiString("Undetermined");
	}

	std::string& GetMOTD() { return m_strMOTD; }

	void SetPendingFullTeardown() { m_bPendingFullTeardown = true; }
	bool IsPendingFullTeardown() const { return m_bPendingFullTeardown; }
	void ConsumePendingFullTeardown() { m_bPendingFullTeardown = false; }

	private:
		void InitSentry();
		void ShutdownSentry();

private:
	NGMP_ENATType m_NATType = NGMP_ENATType::NAT_TYPE_UNDETERMINED;


	NGMP_OnlineServices_AuthInterface* m_pAuthInterface = nullptr;
	NGMP_OnlineServices_LobbyInterface* m_pLobbyInterface = nullptr;
	NGMP_OnlineServices_RoomsInterface* m_pRoomInterface = nullptr;
	NGMP_OnlineServices_StatsInterface* m_pStatsInterface = nullptr;
	PortMapper m_PortMapper;

	HTTPManager* m_pHTTPManager = nullptr;

	int64_t m_lastUserPut = -1;
	int64_t m_timeBetweenUserPuts = 60000;

	WebSocket* m_pWebSocket = nullptr;

	std::string m_strMOTD;

	bool m_bPendingFullTeardown = false;

	std::queue<std::string> m_vecFilesToDownload;
	std::queue<int64_t> m_vecFilesSizes;
	std::vector<std::string> m_vecFilesDownloaded;
	std::function<void(void)> m_updateCompleteCallback = nullptr;

	const std::string strPatchDir = "GeneralsOnlinePatch";

	std::string m_patcher_name;
	std::string m_patcher_path;
	std::string m_patchfile_path;
	int64_t m_patcher_size;
	int64_t m_patchfile_size;
};