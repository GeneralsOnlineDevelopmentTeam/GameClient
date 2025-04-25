#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "../json.hpp"
#include "GameClient/MessageBox.h"

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
	std::string patchfile_path;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(VersionCheckResponse, result, patcher_name, patcher_path, patchfile_path)
};

NGMP_OnlineServicesManager::NGMP_OnlineServicesManager()
{
	NetworkLog("[NGMP] Init");

	m_pOnlineServicesManager = this;
}

std::string NGMP_OnlineServicesManager::GetAPIEndpoint(const char* szEndpoint, bool bAttachToken)
{
	if (bAttachToken)
	{
		std::string strToken = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken();

		if (g_Environment == EEnvironment::DEV)
		{
			return std::format("http://localhost:9000/cloud/env:dev:token:{}/{}", strToken, szEndpoint);
		}
		else // PROD
		{
			return std::format("https://cloud.playgenerals.online:9000/cloud/env:dev:token:{}/{}", strToken, szEndpoint);
		}

	}
	else
	{
		if (g_Environment == EEnvironment::DEV)
		{
			return std::format("http://localhost:9000/cloud/env:dev/{}", szEndpoint);
		}
		else // PROD
		{
			return std::format("https://cloud.playgenerals.online:9000/cloud/env:dev/{}", szEndpoint);
		}
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
}

void NGMP_OnlineServicesManager::StartVersionCheck(UnsignedInt exeCRC, UnsignedInt iniCRC, std::function<void(bool bSuccess, bool bNeedsUpdate)> fnCallback)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("VersionCheck", false);

	nlohmann::json j;
	j["execrc"] = exeCRC;
	j["inicrc"] = iniCRC;
	j["ver"] = GENERALS_ONLINE_VERSION;
	j["netver"] = GENERALS_ONLINE_NET_VERSION;
	j["servicesver"] = GENERALS_ONLINE_SERVICE_VERSION;
	std::string strPostData = j.dump();

	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				VersionCheckResponse authResp = jsonObject.get<VersionCheckResponse>();

				if (authResp.result == EVersionCheckResponseResult::OK)
				{
					NetworkLog("VERSION CHECK: Up To Date");
					fnCallback(true, false);
				}
				else if (authResp.result == EVersionCheckResponseResult::NEEDS_UPDATE)
				{
					NetworkLog("VERSION CHECK: Needs Update");

					// cache the data
					m_patcher_name = authResp.patcher_name;
					m_patcher_path = authResp.patcher_path;
					m_patchfile_path = authResp.patchfile_path;

					fnCallback(true, true);
				}
				else
				{
					NetworkLog("VERSION CHECK: Failed");
					fnCallback(false, false);
				}
			}
			catch (...)
			{
				NetworkLog("VERSION CHECK: Failed to parse response");
				fnCallback(false, false);
			}
		});
}

void NGMP_OnlineServicesManager::ContinueUpdate()
{
	if (m_vecFilesToDownload.size() > 0) // download next
	{
		std::string strDownloadPath = m_vecFilesToDownload.front();
		m_vecFilesToDownload.pop();

		// this isnt a super nice way of doing this, lets make a download manager
		std::map<std::string, std::string> mapHeaders;
		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strDownloadPath.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				m_vecFilesDownloaded.push_back(strDownloadPath);

				char CurDir[MAX_PATH + 1] = {};
				::GetCurrentDirectoryA(MAX_PATH + 1u, CurDir);

				// Extract the filename with extension from strDownloadPath  
				std::string strFileName = strDownloadPath.substr(strDownloadPath.find_last_of('/') + 1);
				std::string strOutPath = std::format("{}/{}/{}", CurDir, strPatchDir, strFileName.c_str());

				uint8_t* pBuffer = pReq->GetBuffer();
				size_t bufSize = pReq->GetBufferSize();

				if (!std::filesystem::exists(strPatchDir))
				{
					std::filesystem::create_directory(strPatchDir);
				}

				FILE* pFile = fopen(strOutPath.c_str(), "wb");
				fwrite(pBuffer, sizeof(uint8_t), bufSize, pFile);
				fclose(pFile);

				// call continue update again, thisll check if we're done or have more work to do
				ContinueUpdate();

				NetworkLog("GOT FILE: %s", strDownloadPath.c_str());
			});
	}
	else if (m_vecFilesToDownload.size() == 0 && m_vecFilesDownloaded.size() > 0) // nothing left but we did download something
	{
		m_updateCompleteCallback();
	}
	
}

void NGMP_OnlineServicesManager::LaunchPatcher()
{
	STARTUPINFOA si = { sizeof(STARTUPINFOA) };
	PROCESS_INFORMATION pi = {};

	char CurDir[MAX_PATH + 1] = {};
	::GetCurrentDirectoryA(MAX_PATH + 1u, CurDir);

	// Extract the filename with extension from strDownloadPath  
	std::string strPatcherDir = std::format("{}/{}", CurDir, strPatchDir);
	std::string strPatcherPath = std::format("{}/{}", strPatcherDir, m_patcher_name);

	if (CreateProcessA(strPatcherPath.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, strPatcherDir.c_str(), &si, &pi))
	{
		// Successfully launched the process, close handles  
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);

		// Exit the application  
		exit(0);
	}
	else
	{
		// show msg
		MessageBoxOk(UnicodeString(L"Update Failed"), UnicodeString(L"Could not run the updater. Please visit www.playgenerals.online and install the latest version manually."), nullptr);
	}
}

void NGMP_OnlineServicesManager::StartDownloadUpdate(std::function<void(void)> cb)
{
	m_vecFilesToDownload = std::queue<std::string>();
	m_vecFilesDownloaded.clear();
	m_vecFilesToDownload.emplace(m_patcher_path); // patcher
	m_vecFilesToDownload.emplace(m_patchfile_path); // patch
	
	m_updateCompleteCallback = cb;

	// cleanup current folder
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

void NGMP_OnlineServicesManager::OnLogin(bool bSuccess, const char* szWSAddr, const char* szWSToken)
{
	// TODO_NGMP: Support websocket reconnects here and on server
	// TODO_NGMP: disconnect websocket when leaving MP
	// TODO_NGMP: websocket keep alive
	if (bSuccess)
	{
		// connect to WS
		// TODO_NGMP: Handle WS conn failure
		m_pWebSocket = new WebSocket();

		m_pWebSocket->Connect(std::format("{}/{}", szWSAddr, szWSToken).c_str());

		// TODO_NGMP: This hangs forever if it fails to connect
	}
}

void NGMP_OnlineServicesManager::Init()
{
	/*
	// Init EOS SDK
	EOS_InitializeOptions SDKOptions = {};
	SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
	SDKOptions.AllocateMemoryFunction = nullptr;
	SDKOptions.ReallocateMemoryFunction = nullptr;
	SDKOptions.ReleaseMemoryFunction = nullptr;

	char szBuffer[MAX_PATH] = { 0 };
	strcpy(szBuffer, "Generals");
	SDKOptions.ProductName = szBuffer;
	SDKOptions.ProductVersion = "1.0";
	SDKOptions.Reserved = nullptr;
	SDKOptions.SystemInitializeOptions = nullptr;
	SDKOptions.OverrideThreadAffinity = nullptr;

	EOS_EResult InitResult = EOS_Initialize(&SDKOptions);

	// TODO_LAUNCH: Have a define to turn off all debug logging, even EOS below, should be no logging on retail builds
	if (InitResult == EOS_EResult::EOS_Success)
	{
		// LOGGING
		EOS_EResult SetLogCallbackResult = EOS_Logging_SetCallback([](const EOS_LogMessage* Message)
			{
				NetworkLog("[NGMP][EOS] %s", Message->Message);
			});
		if (SetLogCallbackResult != EOS_EResult::EOS_Success)
		{
			NetworkLog("[NGMP] Failed to set EOS log callback");
		}
		else
		{

#if _DEBUG
			EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Info);
#else
			EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Info);
#endif
		}

		char CurDir[MAX_PATH + 1] = {};
		::GetCurrentDirectoryA(MAX_PATH + 1u, CurDir);


		// cache dir
		char szTempDir[MAX_PATH + 1] = { 0 };
		sprintf(szTempDir, "%s\\eos_cache", CurDir);

		// PLATFORM OPTIONS
		EOS_Platform_Options PlatformOptions = {};
		PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
		PlatformOptions.bIsServer = EOS_FALSE;
		PlatformOptions.OverrideCountryCode = nullptr;
		PlatformOptions.OverrideLocaleCode = nullptr;
		PlatformOptions.Flags = EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D9 | EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D10;
		PlatformOptions.CacheDirectory = szTempDir;

		PlatformOptions.ProductId = NGMP_EOS_PRODUCT_ID;
		PlatformOptions.SandboxId = NGMP_EOS_SANDBOX_ID;
		PlatformOptions.EncryptionKey = NGMP_EOS_ENCRYPTION_KEY;
		PlatformOptions.DeploymentId = NGMP_EOS_DEPLOYMENT_ID;
		PlatformOptions.ClientCredentials.ClientId = NGMP_EOS_CLIENT_ID;
		PlatformOptions.ClientCredentials.ClientSecret = NGMP_EOS_CLIENT_SECRET;

		double timeout = 5000.f;
		PlatformOptions.TaskNetworkTimeoutSeconds = &timeout;

		EOS_Platform_RTCOptions RtcOptions = { 0 };
		RtcOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;

		// Get absolute path for xaudio2_9redist.dll file
		char szXAudioDir[MAX_PATH + 1] = { 0 };
		sprintf(szXAudioDir, "%s\\xaudio2_9redist.dll", CurDir);

		// does the DLL exist on disk?
		std::fstream fileStream;
		fileStream.open(szXAudioDir, std::fstream::in | std::fstream::binary);
		if (!fileStream.good())
		{
			NetworkLog("[NGMP] FATAL ERROR: Failed to locate XAudio DLL");
			exit(1);
		}
		else
		{
			NetworkLog("[NGMP] XAudio DLL located successfully");
		}


		EOS_Windows_RTCOptions WindowsRtcOptions = { 0 };
		WindowsRtcOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
		WindowsRtcOptions.XAudio29DllPath = szXAudioDir;
		RtcOptions.PlatformSpecificOptions = &WindowsRtcOptions;
		
		PlatformOptions.RTCOptions = &RtcOptions;

#if ALLOW_RESERVED_PLATFORM_OPTIONS
		SetReservedPlatformOptions(PlatformOptions);
#else
		PlatformOptions.Reserved = NULL;
#endif // ALLOW_RESERVED_PLATFORM_OPTIONS

		// platform integration settings
		// Create the generic container.
		const EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainerOptions CreateOptions =
		{
			EOS_INTEGRATEDPLATFORM_CREATEINTEGRATEDPLATFORMOPTIONSCONTAINER_API_LATEST
		};

		const EOS_EResult Result = EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainer(&CreateOptions, &PlatformOptions.IntegratedPlatformOptionsContainerHandle);
		// TODO_NGMP: Handle error
		if (Result != EOS_EResult::EOS_Success)
		{
			NetworkLog("[NGMP] EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainer returned an error\n");
		}

		// Configure platform-specific options.
		const EOS_IntegratedPlatform_Steam_Options PlatformSpecificOptions =
		{
				EOS_INTEGRATEDPLATFORM_STEAM_OPTIONS_API_LATEST,
				nullptr,
				1,
				59
		};

		// Add the configuration to the SDK initialization options.
		const EOS_IntegratedPlatform_Options Options =
		{
			EOS_INTEGRATEDPLATFORM_OPTIONS_API_LATEST,
			EOS_IPT_Steam,
			EOS_EIntegratedPlatformManagementFlags::EOS_IPMF_LibraryManagedByApplication | EOS_EIntegratedPlatformManagementFlags::EOS_IPMF_DisableSDKManagedSessions | EOS_EIntegratedPlatformManagementFlags::EOS_IPMF_PreferIntegratedIdentity,
			&PlatformSpecificOptions
		};

		const EOS_IntegratedPlatformOptionsContainer_AddOptions AddOptions =
		{
			EOS_INTEGRATEDPLATFORMOPTIONSCONTAINER_ADD_API_LATEST,
			&Options
		};
		//EOS_IntegratedPlatformOptionsContainer_Add(PlatformOptions.IntegratedPlatformOptionsContainerHandle, &AddOptions);

		// end platform integration settings

		// TODO_NGMP: We dont EOS_Platform_Release or shutdown the below
		m_EOSPlatformHandle = EOS_Platform_Create(&PlatformOptions);
		// TODO_NGMP: Don't process any input if steam or eos overlays are showing
	}
	*/

	// initialize child classes, these need the platform handle
	m_pAuthInterface = new NGMP_OnlineServices_AuthInterface();
	m_pLobbyInterface = new NGMP_OnlineServices_LobbyInterface();
	m_pRoomInterface = new NGMP_OnlineServices_RoomsInterface();
	m_pStatsInterface = new NGMP_OnlineServices_StatsInterface();

	m_pHTTPManager = new HTTPManager();
	
}



void NGMP_OnlineServicesManager::Tick()
{
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

	/*
	if (m_EOSPlatformHandle != nullptr)
	{
		EOS_Platform_Tick(m_EOSPlatformHandle);
	}
	*/

	if (m_pRoomInterface != nullptr)
	{
		m_pRoomInterface->Tick();
	}

	if (m_pLobbyInterface != nullptr)
	{
		m_pLobbyInterface->Tick();
	}

	int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	if ((currTime - m_lastUserPut) > m_timeBetweenUserPuts)
	{
		m_lastUserPut = currTime;

		if (m_pAuthInterface != nullptr && m_pAuthInterface->IsLoggedIn())
		{
			std::map<std::string, std::string> mapHeaders;
			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(std::format("https://playgenerals.online/cloud/env:dev:{}/User", NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken()).c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					// TODO_NGMP: Handle 404 (session terminated)
				});
		}
	};
}

void WebSocket::Shutdown()
{
	Disconnect();
}

void WebSocket::SendData_LeaveNetworkRoom()
{
	SendData_JoinNetworkRoom(-1);
}
