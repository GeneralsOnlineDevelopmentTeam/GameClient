#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "../json.hpp"
#include "GameClient/MessageBox.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "realcrc.h"
#include "../../DownloadManager.h"
#include <ws2tcpip.h>
#include "GameClient/DisplayStringManager.h"
#include "../../NetworkInterface.h"
#include "Common/MultiplayerSettings.h"
#include "../../GameSpyOverlay.h"
#include "GameClient/Display.h"

extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}


NetworkMesh* NGMP_OnlineServicesManager::GetNetworkMesh()
{
	if (m_pOnlineServicesManager != nullptr)
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = GetInterface< NGMP_OnlineServices_LobbyInterface>();
		if (pLobbyInterface != nullptr)
		{
			return pLobbyInterface->GetNetworkMeshForLobby();
		}
	}

	return nullptr;
}

NGMP_OnlineServicesManager* NGMP_OnlineServicesManager::m_pOnlineServicesManager = nullptr;

enum class EVersionCheckResponseResult : int
{
	OK = 0,
	FAILED = 1,
	NEEDS_UPDATE = 2
};

struct VersionCheckResponse
{
	EVersionCheckResponseResult result;
	std::string patcher_name;
	std::string patcher_path;
	int64_t patcher_size;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(VersionCheckResponse, result, patcher_name, patcher_path, patcher_size)
};

GenOnlineSettings NGMP_OnlineServicesManager::Settings;

NGMP_OnlineServicesManager::NGMP_OnlineServicesManager()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Init");

	m_pOnlineServicesManager = this;

	InitSentry();
}

std::string NGMP_OnlineServicesManager::GetAPIEndpoint(const char* szEndpoint)
{
	if (g_Environment == EEnvironment::DEV)
	{
		return std::format("https://localhost:9000/env/dev/contract/1/{}", szEndpoint);
	}
	else if (g_Environment == EEnvironment::TEST)
	{
		return std::format("https://api.playgenerals.online/env/test/contract/1/{}", szEndpoint);
	}
	else // PROD
	{
		return std::format("https://api.playgenerals.online/env/prod/contract/1/{}", szEndpoint);
	}
}

void NGMP_OnlineServicesManager::Shutdown()
{
	if (m_pHTTPManager != nullptr)
	{
		m_pHTTPManager->Shutdown();
	}

	if (m_pWebSocket != nullptr)
	{
		m_pWebSocket->Shutdown();
	}

	ShutdownSentry();
}

void NGMP_OnlineServicesManager::StartVersionCheck(std::function<void(bool bSuccess, bool bNeedsUpdate)> fnCallback)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("VersionCheck");

	// NOTE: Generals 'CRCs' are not true CRC's, its a custom algorithm. This is fine for lobby comparisons, but its not good for patch comparisons.
	
	// exe crc
	Char filePath[_MAX_PATH];
	GetModuleFileName(NULL, filePath, sizeof(filePath));
	std::ifstream file(filePath, std::ios::binary | std::ios::ate);
	std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buffer(size);
	file.read((char*)buffer.data(), size);
	uint32_t realExeCRC = CRC_Memory((unsigned char*)buffer.data(), size);

	nlohmann::json j;
	j["execrc"] = realExeCRC;
	j["ver"] = GENERALS_ONLINE_VERSION;
	j["netver"] = GENERALS_ONLINE_NET_VERSION;
	j["servicesver"] = GENERALS_ONLINE_SERVICE_VERSION;
	std::string strPostData = j.dump();

	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Version Check: Response code was %d and body was %s", statusCode, strBody.c_str());
			try
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "VERSION CHECK: Up To Date");
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				VersionCheckResponse authResp = jsonObject.get<VersionCheckResponse>();

				if (authResp.result == EVersionCheckResponseResult::OK)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "VERSION CHECK: Up To Date");
					fnCallback(true, false);
				}
				else if (authResp.result == EVersionCheckResponseResult::NEEDS_UPDATE)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "VERSION CHECK: Needs Update");

					// cache the data
					m_patcher_name = authResp.patcher_name;
					m_patcher_path = authResp.patcher_path;
					m_patcher_size = authResp.patcher_size;

					fnCallback(true, true);
				}
				else
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "VERSION CHECK: Failed");
					fnCallback(false, false);
				}
			}
			catch (...)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "VERSION CHECK: Failed to parse response");
				fnCallback(false, false);
			}
		});
}

void NGMP_OnlineServicesManager::ContinueUpdate()
{
	if (m_vecFilesToDownload.size() > 0 && m_pHTTPManager != nullptr) // download next
	{
		std::string strDownloadPath = m_vecFilesToDownload.front();
		m_vecFilesToDownload.pop();

		uint32_t downloadSize = m_vecFilesSizes.front();
		m_vecFilesSizes.pop();

		TheDownloadManager->SetFileName(AsciiString(strDownloadPath.c_str()));
		TheDownloadManager->OnStatusUpdate(DOWNLOADSTATUS_DOWNLOADING);

		// this isnt a super nice way of doing this, lets make a download manager
		std::map<std::string, std::string> mapHeaders;
		m_pHTTPManager->SendGETRequest(strDownloadPath.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				// set done
				TheDownloadManager->OnProgressUpdate(downloadSize, downloadSize, 0, 0);

				m_vecFilesDownloaded.push_back(strDownloadPath);

				std::string strPatchDir = GetPatcherDirectoryPath();

				// Extract the filename with extension from strDownloadPath  
				std::string strFileName = strDownloadPath.substr(strDownloadPath.find_last_of('/') + 1);
				std::string strOutPath = std::format("{}/{}", strPatchDir, strFileName.c_str());

				std::vector<uint8_t> vecBuffer = pReq->GetBuffer();
				size_t bufSize = pReq->GetBufferSize();

				if (!std::filesystem::exists(strPatchDir))
				{
					std::filesystem::create_directory(strPatchDir);
				}

				FILE* pFile = fopen(strOutPath.c_str(), "wb");
				fwrite(vecBuffer.data(), sizeof(uint8_t), bufSize, pFile);
				fclose(pFile);

				// call continue update again, thisll check if we're done or have more work to do
				ContinueUpdate();

				NetworkLog(ELogVerbosity::LOG_RELEASE, "GOT FILE: %s", strDownloadPath.c_str());
			},
			[=](size_t bytesReceived)
			{
				//m_bytesReceivedSoFar += bytesReceived;

				TheDownloadManager->OnProgressUpdate(bytesReceived, downloadSize, -1, -1);
			}
			);
	}
	else if (m_vecFilesToDownload.size() == 0 && m_vecFilesDownloaded.size() > 0) // nothing left but we did download something
	{
		TheDownloadManager->SetFileName("Update is complete!");
		TheDownloadManager->OnStatusUpdate(DOWNLOADSTATUS_FINISHING);

		m_updateCompleteCallback();
	}
	
}

void NGMP_OnlineServicesManager::CancelUpdate()
{

}

void NGMP_OnlineServicesManager::LaunchPatcher()
{
	char GameDir[MAX_PATH + 1] = {};
	::GetCurrentDirectoryA(MAX_PATH + 1u, GameDir);

	// Extract the filename with extension from strDownloadPath
	std::string strPatcherDir = GetPatcherDirectoryPath();
	std::string strPatcherPath = std::format("{}/{}", strPatcherDir, m_patcher_name);

	SHELLEXECUTEINFOA shellexInfo = { sizeof(shellexInfo) };
	shellexInfo.lpVerb = "runas"; // admin
	shellexInfo.lpFile = strPatcherPath.c_str();
	shellexInfo.nShow = SW_SHOWNORMAL;
	shellexInfo.lpDirectory = GameDir;
	//shellexInfo.lpParameters = "/VERYSILENT";

	bool bPatcherExeExists = std::filesystem::exists(strPatcherPath) && std::filesystem::is_regular_file(strPatcherPath);
	bool bPatcherDirExists = std::filesystem::exists(strPatcherDir) && std::filesystem::is_directory(strPatcherDir);
	bool bInvalidSize = true;

	// TODO_NGMP: Replace with CRC ASAP

	// does the file size match?
	if (bPatcherExeExists && bPatcherDirExists)
	{
		std::uintmax_t file_size = std::filesystem::file_size(strPatcherPath);
		if (file_size == m_patcher_size)
		{
			bInvalidSize = false;
		}
	}

	if (!bInvalidSize && bPatcherExeExists && bPatcherDirExists && ShellExecuteExA(&shellexInfo))
	{
		// Exit the application  
		exit(0);
	}
	else
	{
		// show msg
		ClearGSMessageBoxes();
		MessageBoxOk(UnicodeString(L"Update Failed"), UnicodeString(L"Could not run the updater. Press below to exit."), []()
			{
				exit(0);
			});
		ShellExecuteA(NULL, "open", "https://www.playgenerals.online/updatefailed", NULL, NULL, SW_SHOWNORMAL);
	}
}

void NGMP_OnlineServicesManager::StartDownloadUpdate(std::function<void(void)> cb)
{
	TheDownloadManager->SetFileName("Connecting to update service...");
	TheDownloadManager->OnStatusUpdate(DOWNLOADSTATUS_CONNECTING);

	m_vecFilesToDownload = std::queue<std::string>();
	m_vecFilesDownloaded.clear();

	// patcher
	m_vecFilesToDownload.emplace(m_patcher_path);
	m_vecFilesSizes.emplace(m_patcher_size);
	
	m_updateCompleteCallback = cb;

	// cleanup current folder
	std::string strPatchDir = GetPatcherDirectoryPath();
	if (std::filesystem::exists(strPatchDir) && std::filesystem::is_directory(strPatchDir))
	{
		for (const auto& entry : std::filesystem::directory_iterator(strPatchDir))
		{
			std::filesystem::remove_all(entry.path());
		}
	}

	// start for real
	ContinueUpdate();


}

void NGMP_OnlineServicesManager::OnLogin(bool bSuccess, const char* szWSAddr)
{
	// TODO_NGMP: Support websocket reconnects here and on server
	// TODO_NGMP: disconnect websocket when leaving MP
	// TODO_NGMP: websocket keep alive
	if (bSuccess)
	{
		// connect to WS
		// TODO_NGMP: Handle WS conn failure
		m_pWebSocket = new WebSocket();

		m_pWebSocket->Connect(szWSAddr);

		// TODO_NGMP: This hangs forever if it fails to connect
	}
}

void NGMP_OnlineServicesManager::Init()
{
	// initialize child classes, these need the platform handle
	m_pAuthInterface = new NGMP_OnlineServices_AuthInterface();
	m_pLobbyInterface = new NGMP_OnlineServices_LobbyInterface();
	m_pRoomInterface = new NGMP_OnlineServices_RoomsInterface();
	m_pStatsInterface = new NGMP_OnlineServices_StatsInterface();

	m_pHTTPManager = new HTTPManager();
	m_pHTTPManager->Initialize();

	// TODO_NGMP: Better location
	// TODO_NGMP: Get all of this from the service
	int moneyVal = 100000;
	int maxMoneyVal = 1000000;

	while (moneyVal <= maxMoneyVal)
	{
		
		Money newMoneyVal;
		newMoneyVal.deposit(moneyVal, false);
		TheMultiplayerSettings->addStartingMoneyChoice(newMoneyVal, false);

		moneyVal += 50000;
	}

#if 0
	std::map<AsciiString, RGBColor> mapColors;
	mapColors["Dark Red"] = RGBColor{ 0.53f, 0.f, 0.08f };
	mapColors["Brown"] = RGBColor{ 0.46f, 0.26f, 0.26f };
	mapColors["Dark Green"] = RGBColor{ 0.09f, 0.24f, 0.04f };

	for (const auto& [colorName, rgbColor] : mapColors)
	{
		MultiplayerColorDefinition* newDef = TheMultiplayerSettings->newMultiplayerColorDefinition(colorName.str());
		newDef->setColor(rgbColor);
		newDef->setNightColor(rgbColor);
	}
#endif
}



void NGMP_OnlineServicesManager::Tick()
{
	m_qosMgr.Tick();

	if (m_pWebSocket != nullptr)
	{
		m_pWebSocket->Tick();
	}

	if (m_pHTTPManager != nullptr)
	{
		m_pHTTPManager->MainThreadTick();
	}

	if (m_pRoomInterface != nullptr)
	{
		m_pAuthInterface->Tick();
	}

	m_PortMapper.Tick();

	if (m_pRoomInterface != nullptr)
	{
		m_pRoomInterface->Tick();
	}

	if (m_pLobbyInterface != nullptr)
	{
		m_pLobbyInterface->Tick();
	}
}

void NGMP_OnlineServicesManager::InitSentry()
{
	std::string strDumpPath = std::format("{}/GeneralsOnlineCrashData/", TheGlobalData->getPath_UserData().str());
	if (!std::filesystem::exists(strDumpPath))
	{
		std::filesystem::create_directory(strDumpPath);
	}

	sentry_options_t* options = sentry_options_new();
	sentry_options_set_dsn(options, "{REPLACE_SENTRY_DSN}");
	sentry_options_set_database_path(options, strDumpPath.c_str());
	sentry_options_set_release(options, "generalsonline-client@0.1");

#if _DEBUG
	sentry_options_set_debug(options, 1);
	sentry_options_set_logger_level(options, SENTRY_LEVEL_DEBUG);

	sentry_options_set_logger(options,	[](sentry_level_t level, const char* message, va_list args, void* userdata)
	{
			char buffer[1024];
			va_start(args, message);
			vsnprintf(buffer, 1024, message, args);
			buffer[1024 - 1] = 0;
			va_end(args);

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[Sentry] %s", buffer);
	}, nullptr);
#endif

	int i = sentry_init(options);
	NetworkLog(ELogVerbosity::LOG_RELEASE, "Sentry init: %d", i);
}

void NGMP_OnlineServicesManager::ShutdownSentry()
{
	sentry_close();
}


std::string NGMP_OnlineServicesManager::GetPatcherDirectoryPath()
{
	std::string strPatcherDirPath = std::format("{}/GeneralsOnlineData/Update/", TheGlobalData->getPath_UserData().str());
	return strPatcherDirPath;
}

void WebSocket::Shutdown()
{
	Disconnect();
}

void WebSocket::SendData_ChangeName(UnicodeString& strNewName)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::PLAYER_NAME_CHANGE;
	j["name"] = to_utf8(strNewName.str());
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::SendData_LobbyChatMessage(UnicodeString& msg, bool bIsAction, bool bIsAnnouncement, bool bShowAnnouncementToHost)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_ROOM_CHAT_FROM_CLIENT;
	j["message"] = to_utf8(msg.str());
	j["action"] = bIsAction;
	j["announcement"] = bIsAnnouncement;
	j["show_announcement_to_host"] = bShowAnnouncementToHost;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::SendData_LeaveNetworkRoom()
{
	SendData_JoinNetworkRoom(-1);
}


void WebSocket::SendData_RequestSignalling(int64_t targetUserID)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] SEND REQUEST SIGNALING!");
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_CONNECTION_CLIENT_REQUEST_SIGNALLING;
	j["target_user_id"] = targetUserID;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}

void WebSocket::SendData_Signalling(int64_t targetUserID, std::vector<uint8_t> vecPayload)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] SEND SIGNAL!");
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_SIGNAL;
	j["target_user_id"] = targetUserID;
	j["payload"] = vecPayload;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}

void WebSocket::SendData_StartGame()
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::START_GAME;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}

void QoSManager::Tick()
{
	// are all probes done?
	bool bAllDone = true;
	for (QoSProbe& probe : m_lstQoSProbesInFlight)
	{
		bAllDone &= probe.bDone && probe.bSent;
	}

	if (!m_lstQoSProbesInFlight.empty() && bAllDone)
	{
		

		int qosDuration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() - m_timeStartQoS;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "QoS checks are done, took %d ms total", qosDuration);
		// put into an ordered map so we get latency high to low
		QoSProbe* pBestRegion = nullptr;

		NetworkLog(ELogVerbosity::LOG_RELEASE, "==== START QOS RESULTS ====");
		for (QoSProbe& probe : m_lstQoSProbesInFlight)
		{
			m_mapQoSData[probe.regionID] = probe.Latency;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "QoS reply from %s (%s) took %dms", probe.strEndpoint.c_str(), probe.strRegionName.c_str(), probe.Latency);

			if (probe.Latency > 0)
			{
				if (pBestRegion == nullptr || probe.Latency < pBestRegion->Latency)
				{
					pBestRegion = &probe;
				}
			}
		}

		if (pBestRegion != nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Best region is %s (%dms)", pBestRegion->strRegionName.c_str(), pBestRegion->Latency);
			m_PreferredRegionName = pBestRegion->strRegionName.c_str();
			m_PreferredRegionID = pBestRegion->regionID;
			m_PreferredRegionLatency = pBestRegion->Latency;
		}
		NetworkLog(ELogVerbosity::LOG_RELEASE, "==== END QOS RESULTS ====");

		// reset
		m_lstQoSProbesInFlight.clear();

		// invoke cb
		if (m_cbCompletion != nullptr)
		{
			m_cbCompletion();
		}
	}

	if (m_lstQoSProbesInFlight.empty() && bAllDone)
	{
		return;
	}

	static char szBuffer[1024];
	memset(szBuffer, 0, sizeof(szBuffer));

	// now wait
	sockaddr_in addr;
	memset(&addr, 0, sizeof(sockaddr_in));
	int iFromLen = sizeof(sockaddr_in);

	// does current probe need a timetout?
	for (QoSProbe& probe : m_lstQoSProbesInFlight)
	{
		if (!probe.bDone && probe.HasTimedOut())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[QoS] Probe for %s (%s) has timed out", probe.strRegionName.c_str(), probe.strEndpoint.c_str());
			// mark as failed
			probe.bDone = true;
			probe.Latency = -1;
		}
	}

	int iBytesRead = -1;
	while ((iBytesRead = recvfrom(m_Socket_QoSProbing, szBuffer, sizeof(szBuffer), 0, (sockaddr*)&addr, &iFromLen)) != -1)
	{
		char szIpAddress[MAX_SIZE_IP_ADDR] = { 0 };
		inet_ntop(AF_INET, &addr.sin_addr, szIpAddress, MAX_SIZE_IP_ADDR);
		unsigned short usPort = addr.sin_port;

		const int expectedLength = 6;
		if (iBytesRead == expectedLength)
		{
			CBitStream bitStream(expectedLength, szBuffer, iBytesRead);

			bitStream.ResetOffsetForLocalRead();

			// first two should be flipped, rest should be same
			BYTE b1 = bitStream.Read<BYTE>();
			BYTE b2 = bitStream.Read<BYTE>();
			BYTE b3 = bitStream.Read<BYTE>();
			BYTE b4 = bitStream.Read<BYTE>();
			BYTE b5 = bitStream.Read<BYTE>();
			BYTE b6 = bitStream.Read<BYTE>();

			if (b1 == 0x00 && b2 == 0x00
				&& b3 == 0x01
				&& b4 == 0x02
				&& b5 == 0x03
				&& b6 == 0x04)
			{
				// find the associated one
				bool bFound = false;
				for (QoSProbe& probe : m_lstQoSProbesInFlight)
				{
					//if (memcmp(&probe.addr, &addr, sizeof(addr) == 0))
					if (strcmp(szIpAddress, probe.strIPAddr.c_str()) == 0 && probe.Port == usPort)
					{
						int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
						int msTaken = currTime - probe.startTime;
						probe.Latency = msTaken;
						probe.bDone = true;

						bFound = true;
						NetworkLog(ELogVerbosity::LOG_RELEASE, "QoS reply from %s (%s) took %dms", probe.strEndpoint.c_str(), probe.strRegionName.c_str(), msTaken);
						break;
					}
				}

				// TODO_RELAY: add a timeout for the overall process too?
				if (!bFound)
				{
					// find which probe is in flight and mark it as bad
					for (QoSProbe& probe : m_lstQoSProbesInFlight)
					{
						if (!probe.bDone && probe.bSent)
						{
							probe.Latency = -1;
							probe.bDone = true;
							break;
						}
					}
				}
			}
			else
			{
				for (QoSProbe& probe : m_lstQoSProbesInFlight)
				{
					//if (memcmp(&probe.addr, &addr, sizeof(addr) == 0))
					if (strcmp(szIpAddress, probe.strIPAddr.c_str()) == 0 && probe.Port == usPort)
					{
						probe.Latency = -1;
						probe.bDone = true;
					}
				}
			}
		}
		else
		{
			for (QoSProbe& probe : m_lstQoSProbesInFlight)
			{
				//if (memcmp(&probe.addr, &addr, sizeof(addr) == 0))
				if (strcmp(szIpAddress, probe.strIPAddr.c_str()) == 0 && probe.Port == usPort)
				{
					probe.Latency = -1;
					probe.bDone = true;
				}
			}
		}
	}
}

void QoSManager::StartProbing(std::map<std::pair<std::string, EQoSRegions>, std::string>& endpoints, std::function<void(void)> cbOnComplete)
{
	m_cbCompletion = cbOnComplete;

	m_mapQoSEndpoints = endpoints;
	m_timeStartQoS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// create our socket
	m_Socket_QoSProbing = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	u_long sockopt = 1;
	ioctlsocket(m_Socket_QoSProbing, FIONBIO, &sockopt);

	// get ip from hostname
	for (const auto& qosEndpoint : m_mapQoSEndpoints)
	{
		// keep ticking while we're in this loop, so we dont artificially add latency
		Tick();

		QoSProbe newProbe;
		newProbe.strEndpoint = qosEndpoint.second;
		newProbe.strRegionName = qosEndpoint.first.first;
		newProbe.regionID = qosEndpoint.first.second;
		

		struct sockaddr_in probeAddr;

		hostent* pEnt = gethostbyname(newProbe.strEndpoint.c_str());
		if (pEnt != nullptr)
		{
			memcpy(&probeAddr.sin_addr, pEnt->h_addr_list[0], pEnt->h_length);
			probeAddr.sin_family = AF_INET;
			probeAddr.sin_port = htons(3075);

			CBitStream bsProbe(6);
			bsProbe.Write<BYTE>(0xFF);
			bsProbe.Write<BYTE>(0xFF);
			bsProbe.Write<BYTE>(0x01);
			bsProbe.Write<BYTE>(0x02);
			bsProbe.Write<BYTE>(0x03);
			bsProbe.Write<BYTE>(0x04);
			sendto(m_Socket_QoSProbing, (char*)bsProbe.GetRawBuffer(), (int)bsProbe.GetNumBytesUsed(), 0, (sockaddr*)&probeAddr, sizeof(sockaddr_in));

			// add to in flight list
			char szIpAddress[MAX_SIZE_IP_ADDR] = { 0 };
			inet_ntop(AF_INET, &probeAddr.sin_addr, szIpAddress, MAX_SIZE_IP_ADDR);

			newProbe.bSent = true;
			newProbe.Port = probeAddr.sin_port;
			newProbe.strIPAddr = std::string(szIpAddress);
			newProbe.startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

			// keep ticking while we're in this loop, so we dont artificially add latency
			Tick();
		}
		else
		{
			// mark as done and failed
			newProbe.bSent = true;
			newProbe.bDone = true;
			newProbe.Latency = -1;

			NetworkLog(ELogVerbosity::LOG_RELEASE, "QoS: Failed to resolve hostname %s", newProbe.strEndpoint.c_str());
		}

		m_lstQoSProbesInFlight.push_back(newProbe);
	}
}