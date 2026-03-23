#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "../json.hpp"
#include "GameClient/MessageBox.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "realcrc.h"
#include "GameNetwork/DownloadManager.h"
#include <ws2tcpip.h>
#include "GameClient/DisplayStringManager.h"
#include "GameNetwork/NetworkInterface.h"
#include "Common/MultiplayerSettings.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/Display.h"
#include "surfaceclass.h"
#include "dx8wrapper.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "GameNetwork/GeneralsOnline/Vendor/stb_image/stb_image_write.h"
#include "GameNetwork/GeneralsOnline/Vendor/stb_image/stb_image_resize.h"
#include "GameClient/GameText.h"

extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

NGMP_OnlineServicesManager* NGMP_OnlineServicesManager::m_pOnlineServicesManager = nullptr;


std::thread::id NGMP_OnlineServicesManager::g_MainThreadID;
std::mutex NGMP_OnlineServicesManager::m_ScreenshotMutex;
std::vector<std::string> NGMP_OnlineServicesManager::m_vecGuardedSSData;


bool NGMP_OnlineServicesManager::g_bAdvancedNetworkStats;

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


void NGMP_OnlineServicesManager::GetAndParseServiceConfig(std::function<void(void)> cbOnDone)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ServiceConfig");
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				if (bSuccess && statusCode == 200)
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					m_ServiceConfig = jsonObject.get<ServiceConfig>();
				}
				else
				{
					// It's OK to fail, we'll just use the sensible defaults
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to get service config, using defaults. Status code: %d", statusCode);
					m_ServiceConfig = ServiceConfig();
				}
				
			}
			catch (...)
			{
				// It's OK to fail, we'll just use the sensible defaults
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to get service config, using defaults. Exception.");
				m_ServiceConfig = ServiceConfig();
			}

			if (cbOnDone != nullptr)
			{
				cbOnDone();
			}
		});
}


void NGMP_OnlineServicesManager::CaptureScreenshotToDisk()
{
	// create dirs
	std::string strScreenshotsDir = std::format("{}\\GeneralsOnlineScreenshots\\", TheGlobalData->getPath_UserData().str());

	if (!std::filesystem::exists(strScreenshotsDir))
	{
		std::filesystem::create_directory(strScreenshotsDir);
	}

	// calculate path
	auto now = std::chrono::system_clock::now();
	auto in_time_t = std::chrono::system_clock::to_time_t(now);
	std::stringstream ss;
	ss << std::put_time(std::localtime(&in_time_t), "GeneralsOnline_Screenshot_%Y-%m-%d-%H-%M-%S.jpg");

	std::string strFilePath = std::format("{}\\{}", strScreenshotsDir.c_str(), ss.str().c_str());

	// do UI output immediately on mainthread (if ingame)
	if (TheInGameUI != nullptr)
	{
		UnicodeString ufileName;
		ufileName.translate(AsciiString(strFilePath.c_str()));
		TheInGameUI->message(TheGameText->fetch("GUI:ScreenCapture"), ufileName.str());
	}

	NGMP_OnlineServicesManager::CaptureScreenshot(false, [strFilePath](std::vector<unsigned char> vecBuffer)
		{
			if (!vecBuffer.empty())
			{
				// write to disk
				FILE* pFile = fopen(strFilePath.c_str(), "wb");
				if (pFile != nullptr) {
					fwrite(vecBuffer.data(), sizeof(uint8_t), vecBuffer.size(), pFile);
					fclose(pFile);
				}
			}
		});
}


void NGMP_OnlineServicesManager::CaptureScreenshotForProbe(EScreenshotType screenshotType)
{
	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr != nullptr)
	{
		ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

		if (serviceConf.do_probes)
		{
			CHECK_MAIN_THREAD;

			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
			if (pLobbyInterface != nullptr)
			{
				uint64_t currentMatchID = pLobbyInterface->GetCurrentMatchID();

				NGMP_OnlineServicesManager::GetInstance()->CaptureScreenshot(true, [currentMatchID, screenshotType](std::vector<unsigned char> vecData)
					{
						CHECK_WORKER_THREAD;

						if (vecData.empty())
						{
							NetworkLog(ELogVerbosity::LOG_DEBUG, "Screenshot capture failed, no data");
							return;
						}

						nlohmann::json j;
						j["img"] = nullptr;
						j["imgtype"] = (int)screenshotType;
						j["match_id"] = currentMatchID;

						// encode body
						j["img"] = Base64Encode(vecData);

						std::string strPostData = j.dump();

						std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);
						m_vecGuardedSSData.push_back(strPostData);
					});
			}
		}
	}
}

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
		if (NGMP_OnlineServicesManager::Settings.Network_UseAlternativeEndpoint())
		{
			return std::format("https://api-ru.playgenerals.online/env/prod/contract/1/{}", szEndpoint);
		}
		else
		{
			return std::format("https://api.playgenerals.online/env/prod/contract/1/{}", szEndpoint);
		}
	}
}

void NGMP_OnlineServicesManager::CommitReplay(AsciiString absoluteReplayPath)
{
	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr != nullptr)
	{
		ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

		if (serviceConf.do_replay_upload)
		{
			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
			if (pLobbyInterface == nullptr)
			{
				return;
			}

			uint64_t currentMatchID = pLobbyInterface->GetCurrentMatchID();

			FILE* pFile = fopen(absoluteReplayPath.str(), "rb");

			std::vector<unsigned char> replayData;
			if (pFile)
			{
				fseek(pFile, 0, SEEK_END);
				long fileSize = ftell(pFile);
				fseek(pFile, 0, SEEK_SET);
				if (fileSize > 0)
				{
					replayData.resize(fileSize);
					fread(replayData.data(), 1, fileSize, pFile);
				}
				fclose(pFile);
			}

			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("MatchReplay");
			std::map<std::string, std::string> mapHeaders;

			nlohmann::json j;
			j["replaydata"] = Base64Encode(replayData);
			j["match_id"] = currentMatchID;

			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{

				}, nullptr, HTTP_UPLOAD_TIMEOUT);
		}
	}
}

void NGMP_OnlineServicesManager::WaitForScreenshotThreads()
{
	std::scoped_lock<std::mutex> lock(m_mutexScreenshotThreads);
	
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Waiting for %d screenshot threads to complete...", (int)m_vecScreenshotThreads.size());
	
	for (std::thread* pThread : m_vecScreenshotThreads)
	{
		if (pThread != nullptr && pThread->joinable())
		{
			pThread->join();
			delete pThread;
		}
	}
	
	m_vecScreenshotThreads.clear();
	
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] All screenshot threads completed");
}

void NGMP_OnlineServicesManager::Shutdown()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] OnlineServicesManager shutdown initiated");
	
	// First, wait for all screenshot threads to complete
	// This prevents race conditions where threads might still be using resources
	WaitForScreenshotThreads();
	
	// Shutdown and completely destroy WebSocket BEFORE cleaning up HTTPManager
	// This is critical because WebSocket has curl handles that must be freed
	// before curl_global_cleanup() is called by HTTPManager
	if (m_pWebSocket)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Shutting down WebSocket...");
		m_pWebSocket->Shutdown();
		
		// Reset shared_ptr to fully destroy WebSocket and free all its curl resources
		// This must happen before HTTPManager shutdown to avoid accessing freed curl state
		m_pWebSocket.reset();
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] WebSocket shutdown complete");
	}

	// Now safe to shutdown HTTP manager which calls curl_global_cleanup()
	if (m_pHTTPManager != nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Shutting down HTTPManager...");
		m_pHTTPManager->Shutdown();
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] HTTPManager shutdown complete");
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] OnlineServicesManager shutdown complete");
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
		}, nullptr, -1);
}

void NGMP_OnlineServicesManager::ContinueUpdate()
{
	if (m_vecFilesToDownload.size() > 0 && m_pHTTPManager != nullptr) // download next
	{
		std::string strDownloadPath = m_vecFilesToDownload.front();
		m_vecFilesToDownload.pop();

		uint32_t downloadSize = m_vecFilesSizes.front();
		m_vecFilesSizes.pop();

		if (TheDownloadManager != nullptr)
		{
			TheDownloadManager->SetFileName(AsciiString(strDownloadPath.c_str()));
			TheDownloadManager->OnStatusUpdate(DOWNLOADSTATUS_DOWNLOADING);
		}

		// this isnt a super nice way of doing this, lets make a download manager
		std::map<std::string, std::string> mapHeaders;
		m_pHTTPManager->SendGETRequest(strDownloadPath.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				if (statusCode != 200)
				{
					// show msg
					ClearGSMessageBoxes();
					MessageBoxOk(UnicodeString(L"Update Failed"), UnicodeString(L"Could not download the updater. Press below to exit."), []()
						{
							TheGameEngine->setQuitting(TRUE);
						});
					ShellExecuteA(NULL, "open", "https://www.playgenerals.online/updatefailed", NULL, NULL, SW_SHOWNORMAL);
				}
				else
				{
					// set done
					if (TheDownloadManager != nullptr)
					{
						TheDownloadManager->OnProgressUpdate(downloadSize, downloadSize, 0, 0);
					}

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
					if (pFile != nullptr) {
						fwrite(vecBuffer.data(), sizeof(uint8_t), bufSize, pFile);
						fclose(pFile);
					}

					// call continue update again, thisll check if we're done or have more work to do
					ContinueUpdate();

					NetworkLog(ELogVerbosity::LOG_RELEASE, "GOT FILE: %s", strDownloadPath.c_str());
				}
			},
			[=](size_t bytesReceived)
			{
				//m_bytesReceivedSoFar += bytesReceived;

				if (TheDownloadManager != nullptr)
				{
					TheDownloadManager->OnProgressUpdate(bytesReceived, downloadSize, -1, -1);
				}
			}
			);
	}
	else if (m_vecFilesToDownload.size() == 0 && m_vecFilesDownloaded.size() > 0) // nothing left but we did download something
	{
		if (TheDownloadManager != nullptr)
		{
			TheDownloadManager->SetFileName("Update is complete!");
			TheDownloadManager->OnStatusUpdate(DOWNLOADSTATUS_FINISHING);
		}

		m_updateCompleteCallback();
	}
	
}


void NGMP_OnlineServicesManager::CaptureScreenshot(bool bResizeForTransmit, std::function<void(std::vector<unsigned char>)> cbOnDataAvailable)
{
	CHECK_MAIN_THREAD;

	bool bSucceeded = false;

	// no callback, nothing to do, early out
	if (cbOnDataAvailable == nullptr)
	{
		return;
	}

	SurfaceClass* surface = DX8Wrapper::_Get_DX8_Back_Buffer();
	LPDIRECT3DSURFACE8 surf = nullptr;
	SurfaceClass* surfaceCopy = nullptr;
	void* pBits = nullptr;
	IDirect3DSurface8* pDXsurf = nullptr;

	if (surface != nullptr)
	{
		SurfaceClass::SurfaceDescription surfaceDesc;
		surface->Get_Description(surfaceDesc);
		
		pDXsurf = DX8Wrapper::_Create_DX8_Surface(surfaceDesc.Width, surfaceDesc.Height, surfaceDesc.Format);
		
		if (pDXsurf != nullptr)
		{
			surfaceCopy = NEW_REF(SurfaceClass, (pDXsurf));

			if (surfaceCopy != nullptr)
			{
				DX8Wrapper::_Copy_DX8_Rects(surface->Peek_D3D_Surface(), NULL, 0, surfaceCopy->Peek_D3D_Surface(), NULL);

				HRESULT hr;

				D3DDISPLAYMODE mode;
				if (SUCCEEDED(hr = DX8Wrapper::_Get_D3D_Device8()->GetDisplayMode(&mode)))
				{
					if (SUCCEEDED(hr = DX8Wrapper::_Get_D3D_Device8()->CreateImageSurface(mode.Width, mode.Height,
						D3DFMT_A8R8G8B8, &surf)))
					{
						if (SUCCEEDED(hr = DX8Wrapper::_Get_D3D_Device8()->GetFrontBuffer(surf)))
						{
							// gather all our data
							int pitch = 0;
							pBits = surfaceCopy->Lock(&pitch);

							if (pBits != nullptr)
							{
								int width = surfaceDesc.Width;
								int height = surfaceDesc.Height;

								// process on thread - track the thread so we can join it during shutdown
								std::thread* pNewThread = new std::thread([cbOnDataAvailable, width, height, pBits, pDXsurf, pitch, bResizeForTransmit]()
									{
										CHECK_WORKER_THREAD;

										unsigned char* rgbData = new unsigned char[width * height * 3];

										std::vector<unsigned char> vecData;

										int finalWidth = width;
										int finalHeight = height;

										for (int y = 0; y < height; ++y) {
											uint8_t* row = static_cast<uint8_t*>(pBits) + y * pitch;
											int rowOffset = y * width * 3;
											int srcOffset = 0;
											for (int x = 0; x < width; ++x, srcOffset += 4)
											{
												int dstIndex = rowOffset + x * 3;
												rgbData[dstIndex + 0] = row[srcOffset + 2]; // R
												rgbData[dstIndex + 1] = row[srcOffset + 1]; // G
												rgbData[dstIndex + 2] = row[srcOffset + 0]; // B
											}
										}

										// resize
										unsigned char* pBufferToWrite = rgbData;
										if (bResizeForTransmit)
										{
											int new_width = 557;
											int new_height = 333;
											int channels = 3;
											unsigned char* resized = new unsigned char[new_width * new_height * channels];

											stbir_resize_uint8(rgbData, width, height, 0,
												resized, new_width, new_height, 0,
												channels
											);

											// update data
											finalWidth = new_width;
											finalHeight = new_height;
											pBufferToWrite = resized;
										}
										// end resize

										stbi_write_jpg_to_func([](void* context, void* data, int size)
											{
												std::vector<unsigned char>* buffer = static_cast<std::vector<unsigned char>*>(context);
												buffer->insert(buffer->end(), (unsigned char*)data, (unsigned char*)data + size);
											}, &vecData, finalWidth, finalHeight, 3, pBufferToWrite, bResizeForTransmit ? 0 : 90);

										// cleanup
										if (bResizeForTransmit)
										{
											delete[] pBufferToWrite; // This is 'resized'
											pBufferToWrite = nullptr;
										}

										delete[] rgbData;
										rgbData = nullptr;

										if (pDXsurf != nullptr)
										{
											pDXsurf->Release();
										}

										// invoke cb
										if (cbOnDataAvailable != nullptr)
										{
											cbOnDataAvailable(vecData);
										}
									}
								);

								// Store the thread so we can join it during shutdown
								if (m_pOnlineServicesManager != nullptr)
								{
									std::scoped_lock<std::mutex> lock(m_pOnlineServicesManager->m_mutexScreenshotThreads);
									m_pOnlineServicesManager->m_vecScreenshotThreads.push_back(pNewThread);
								}

								bSucceeded = true;
							}
						}
					}
				}
			}
		}
	}

	// clean everything up, whether we succeeded or not

	// release the image surface
	if (surf != nullptr)
	{
		surf->Release();
		//delete surf;
		surf = nullptr;
	}

	// unlock
	if (surface != nullptr)
	{
		surface->Unlock();
		surface->Release_Ref();
 		surface = nullptr;
	}

	if (surfaceCopy != nullptr)
	{
		surfaceCopy->Unlock();
		surfaceCopy->Release_Ref();
 		surfaceCopy = nullptr;
	}

	if (!bSucceeded) // if success, thread uses this and then destroys it
	{
		if (pDXsurf != nullptr)
		{
			pDXsurf->Release();
		}
	}

	// callback if failed
	if (!bSucceeded)
	{
		cbOnDataAvailable(std::vector<unsigned char>());
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
		TheGameEngine->setQuitting(TRUE);
	}
	else
	{
		// show msg
		ClearGSMessageBoxes();
		MessageBoxOk(UnicodeString(L"Update Failed"), UnicodeString(L"Could not run the updater. Press below to exit."), []()
			{
				TheGameEngine->setQuitting(TRUE);
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

void NGMP_OnlineServicesManager::OnLogin(ELoginResult loginResult, const char* szWSAddr, std::function<void(void)> fnWebsocketConnectedCallback)
{
	if (loginResult == ELoginResult::Success)
	{
		// connect to WS
		m_pWebSocket = std::make_shared<WebSocket>();

		// TODO_NGMP: This should come from the service, if the service was russia-aware
		std::string strWebsocketAddr = NGMP_OnlineServicesManager::Settings.Network_UseAlternativeEndpoint() ? "wss://api-ru.playgenerals.online/ws" : std::string(szWSAddr);

        m_pWebSocket->Connect(strWebsocketAddr.c_str(), false, [=]()
            {
                // Get friends list and blocked list
                // we need to wait until the websocket is connected so we have a session
                NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
                if (pSocialInterface == nullptr)
                {
                    return;
                }

                pSocialInterface->GetFriendsList(false, nullptr);
                pSocialInterface->GetBlockList(nullptr);

                // and invoke callback
                fnWebsocketConnectedCallback();
            });
	}
}

void NGMP_OnlineServicesManager::Init()
{
	g_MainThreadID = std::this_thread::get_id();

	// initialize child classes, these need the platform handle
	m_pAuthInterface = new NGMP_OnlineServices_AuthInterface();
	m_pLobbyInterface = new NGMP_OnlineServices_LobbyInterface();
	m_pRoomInterface = new NGMP_OnlineServices_RoomsInterface();
	m_pStatsInterface = new NGMP_OnlineServices_StatsInterface();
	m_pMatchmakingInterface = new NGMP_OnlineServices_MatchmakingInterface();
	m_pSocialInterface = new NGMP_OnlineServices_SocialInterface();

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
	// screenshots
	{
		// send screenshot
		std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("MatchUpdate");
		std::map<std::string, std::string> mapHeaders;

		std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);

		for (std::string& b64SSData : m_vecGuardedSSData)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, b64SSData.c_str(),
				[=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{

				}, nullptr, HTTP_UPLOAD_TIMEOUT);
		}
		m_vecGuardedSSData.clear();
	}

	if (m_pWebSocket != nullptr)
	{
		m_pWebSocket->Tick();
	}

	if (m_pHTTPManager != nullptr)
	{
		m_pHTTPManager->Tick();
	}

	if (m_pRoomInterface != nullptr)
	{
		m_pAuthInterface->Tick();
	}

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
#if !_DEBUG
	std::string strDumpPath = std::format("{}/GeneralsOnlineCrashData/", TheGlobalData->getPath_UserData().str());
	if (!std::filesystem::exists(strDumpPath))
	{
		std::filesystem::create_directory(strDumpPath);
	}

	sentry_options_t* options = sentry_options_new();

	sentry_options_set_dsn(options, "https://61750bebd112d279bcc286d617819269@o4509316925554688.ingest.us.sentry.io/4509316927586304");
	sentry_options_set_database_path(options, strDumpPath.c_str());
	sentry_options_set_release(options, "generalsonline-client@021326_QFE2");

	// local player info
	int64_t userID = -1;
	std::string strDisplayname = "Unknown";
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface != nullptr)
	{
		userID = pAuthInterface->GetUserID();
		strDisplayname = pAuthInterface->GetDisplayName();
	}
	std::string strUserID = std::format("{}", userID);


	sentry_value_t userinfoVal = sentry_value_new_object();
	sentry_value_set_by_key(userinfoVal, "user_id", sentry_value_new_int32(userID));
	sentry_value_set_by_key(userinfoVal, "user_displayname", sentry_value_new_string(strDisplayname.c_str()));
	sentry_set_context("user_info", userinfoVal);

	sentry_set_tag("user_id", strUserID.c_str());
	sentry_set_tag("user_displayname", strDisplayname.c_str());

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

	sentry_init(options);
#endif
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
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Shutdown initiated");
	
	// Signal that we're shutting down
	m_bShuttingDown = true;
	
	// Disconnect from the websocket
	Disconnect();

    // Free headers


	if (m_pHeaders != nullptr)
	{
		curl_slist_free_all(m_pHeaders);
		m_pHeaders = nullptr;
	}
	
	// Give CURL time to process the disconnect and cease operations
	// This ensures any background I/O threads have completed before we return
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Shutdown complete");
}

void WebSocket::SendData_ChangeLobbyPassword(UnicodeString& strNewPassword)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_CHANGE_PASSWORD;
	j["new_password"] = to_utf8(strNewPassword.str());
	std::string strBody = j.dump();

	Send(strBody.c_str());
}


void WebSocket::SendData_RemoveLobbyPassword()
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_REMOVE_PASSWORD;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::SendData_ChangeName(UnicodeString& strNewName)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::PLAYER_NAME_CHANGE;
	j["name"] = to_utf8(strNewName.str());
	std::string strBody = j.dump();

	Send(strBody.c_str());
}


void WebSocket::SendData_FriendMessage(UnicodeString& msg, int64_t target_user_id)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::SOCIAL_FRIEND_CHAT_MESSAGE_CLIENT_TO_SERVER;
	j["target_user_id"] = target_user_id;
	j["message"] = to_utf8(msg.str());
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


void WebSocket::SendData_SubscribeRealtimeUpdates()
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::SOCIAL_SUBSCRIBE_REALTIME_UPDATES;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}


void WebSocket::SendData_UnsubscribeRealtimeUpdates()
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::SOCIAL_UNSUBSCRIBE_REALTIME_UPDATES;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}

void WebSocket::SendData_CountdownStarted()
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::START_GAME_COUNTDOWN_STARTED;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}


void WebSocket::SendData_StartFullMeshConnectivityCheck(std::function<void(bool, std::list<std::pair<int64_t, int64_t>>)> cbOnConnectivityCheckComplete)
{
	m_cbOnConnectivityCheckComplete = cbOnConnectivityCheckComplete;

	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_HOST_REQUESTS_BEGIN;
	std::string strBody = j.dump();
	Send(strBody.c_str());
}
