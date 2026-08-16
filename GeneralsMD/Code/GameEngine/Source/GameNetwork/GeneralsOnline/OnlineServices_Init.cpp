#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "../json.hpp"
#include "GameClient/MessageBox.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "WWLib/realcrc.h"
#include "GameNetwork/DownloadManager.h"
#include <ws2tcpip.h>
#include "GameClient/DisplayStringManager.h"
#include "GameNetwork/NetworkInterface.h"
#include "Common/MultiplayerSettings.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameClient/Display.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/dx8wrapper.h"
#include <mutex>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "GameNetwork/GeneralsOnline/Vendor/stb_image/stb_image_write.h"
#include "GameNetwork/GeneralsOnline/Vendor/stb_image/stb_image_resize.h"
#include "GameClient/GameText.h"
#include <unordered_set>

extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

NGMP_OnlineServicesManager* NGMP_OnlineServicesManager::m_pOnlineServicesManager = nullptr;
std::recursive_mutex NGMP_OnlineServicesManager::m_singletonMutex;

std::thread::id NGMP_OnlineServicesManager::g_MainThreadID;
std::mutex NGMP_OnlineServicesManager::m_ScreenshotMutex;
std::vector<S3ScreenshotEntry> NGMP_OnlineServicesManager::m_vecGuardedSSData;


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
	
	// SECURITY FIX: Capture manager instance through GetInstance() to ensure thread-safety
	// Lambda will check if manager still exists before accessing members
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [cbOnDone](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				// SECURITY FIX: Re-acquire manager pointer inside lambda to check for shutdown
				NGMP_OnlineServicesManager* pMgr = NGMP_OnlineServicesManager::GetInstance();
				if (pMgr == nullptr)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Manager destroyed during service config request");
					return;
				}
				
				if (bSuccess && statusCode == 200)
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					pMgr->m_ServiceConfig = jsonObject.get<ServiceConfig>();
				}
				else
				{
					// It's OK to fail, we'll just use the sensible defaults
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to get service config, using defaults. Status code: %d", statusCode);
					pMgr->m_ServiceConfig = ServiceConfig();
				}
				
			}
			catch (...)
			{
				// It's OK to fail, we'll just use the sensible defaults
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to get service config, using defaults. Exception.");
				NGMP_OnlineServicesManager* pMgr = NGMP_OnlineServicesManager::GetInstance();
				if (pMgr != nullptr)
				{
					pMgr->m_ServiceConfig = ServiceConfig();
				}
			}

			if (cbOnDone != nullptr)
			{
				cbOnDone();
			}
		});
}

void NGMP_OnlineServicesManager::CaptureScreenshotForProbe(EScreenshotType screenshotType, std::string strURI)
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
				NGMP_OnlineServicesManager::GetInstance()->CaptureScreenshot(true, [strURI, screenshotType](std::vector<uint8_t> vecData)
					{
						CHECK_WORKER_THREAD;

						if (vecData.empty())
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "Screenshot capture failed, no data");
							return;
						}

                        // certain screenshots require caching for later upload when we have a valid match id, so we store them
						if (screenshotType == EScreenshotType::SCREENSHOT_TYPE_LOADSCREEN)
						{
							NGMP_OnlineServicesManager::GetInstance()->CacheScreenshotBytes_StartMatch(vecData);
						}
                        else if (screenshotType == EScreenshotType::SCREENSHOT_TYPE_SCORESCREEN)
                        {
                            NGMP_OnlineServicesManager::GetInstance()->CacheScreenshotBytes_EndMatch(vecData);
                        }
						else
						{
                            // send back to main thread for processing
							// NOTE: we don't lock in the above cases because the called functions lock
                            std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);

                            S3ScreenshotEntry newEntry;
                            newEntry.vecBytes = std::move(vecData);
                            newEntry.strSignedURI = strURI;
                            newEntry.screenshotType = screenshotType;
                            m_vecGuardedSSData.push_back(newEntry);
						}
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
		return std::format("https://api.playgenerals.online:2087/env/test/contract/1/{}", szEndpoint);
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

void NGMP_OnlineServicesManager::AttemptLoadSteam()
{
    // app id for ZH
    SetEnvironmentVariableA("SteamAppId", "2732960");

    // load steam 32bit dll from local dir if present
    HMODULE steamDll = LoadLibraryA("steam_api.dll");	
    if (!steamDll)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "steam_api.dll not found. Running without Steam.");
        return;
    }

	// Init func
    typedef bool (*SteamAPI_Init_t)();
    SteamAPI_Init_t SteamAPI_Init = (SteamAPI_Init_t)GetProcAddress(steamDll, "SteamAPI_InitSafe");

    if (!SteamAPI_Init)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "SteamAPI_Init not found in DLL.");
        FreeLibrary(steamDll);
        return;
    }

    // steam init
    if (SteamAPI_Init())
	{
        NetworkLog(ELogVerbosity::LOG_RELEASE, "SteamAPI_Init succeeded.");
    }
    else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "SteamAPI_Init failed.");
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

			// cache the data until we get an S3 URL from server
			NGMP_OnlineServicesManager::GetInstance()->CacheReplayBytes(replayData);
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

	AnticheatPlugInterface::UnloadPlugin();

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

	if (!file.is_open() || size <= 0)
	{
		fnCallback(false, false);
		return;
	}

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
					if (strPatchDir.empty())
						return;

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

		std::scoped_lock<std::recursive_mutex> lock(m_updateCallbackMutex);
		if (m_updateCompleteCallback != nullptr)
		{
			m_updateCompleteCallback();
		}
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

								// Copy pixel data into an owned buffer before spawning the thread so
								// the surface can be safely unlocked on the main thread immediately after.
								std::vector<uint8_t> pixelData(height * pitch);
								memcpy(pixelData.data(), pBits, height * pitch);

								// process on thread - track the thread so we can join it during shutdown
								std::thread* pNewThread = new std::thread([cbOnDataAvailable, width, height, pixelData = std::move(pixelData), pitch, bResizeForTransmit]()
									{
										CHECK_WORKER_THREAD;

										unsigned char* rgbData = new unsigned char[width * height * 3];

										std::vector<unsigned char> vecData;

										int finalWidth = width;
										int finalHeight = height;

										for (int y = 0; y < height; ++y) {
											const uint8_t* row = pixelData.data() + y * pitch;
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
											ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
											int new_width = serviceConf.screenshot_width;
											int new_height = serviceConf.screenshot_height;
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

										// invoke cb
										if (cbOnDataAvailable != nullptr)
										{
											cbOnDataAvailable(vecData);
										}
									}
								);

							// Store the thread so we can join it during shutdown
							// SECURITY FIX: Capture manager pointer before spawning thread to avoid TOCTOU race
							NGMP_OnlineServicesManager* pMgr = NGMP_OnlineServicesManager::GetInstance();
							if (pMgr != nullptr)
							{
								std::scoped_lock<std::mutex> lock(pMgr->m_mutexScreenshotThreads);
								pMgr->m_vecScreenshotThreads.push_back(pNewThread);
							}
							else
							{
								// Manager was destroyed, cannot store thread. Thread will leak but won't crash.
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[Screenshot] Manager destroyed before thread could be registered");
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

	if (pDXsurf != nullptr)
	{
		pDXsurf->Release();
		pDXsurf = nullptr;
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
	
	{
		std::scoped_lock<std::recursive_mutex> lock(m_updateCallbackMutex);
		m_updateCompleteCallback = cb;
	}

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

    std::string strPlugin = NGMP_OnlineServicesManager::Settings.GetAnticheatPlugin();
	std::string pluginPath = std::format("plugins/{}/{}.dll", strPlugin.c_str(), strPlugin.c_str());

#if _DEBUG
	AnticheatPlugInterface::LoadPlugin(pluginPath.c_str());
#else
	AnticheatPlugInterface::LoadPlugin(pluginPath.c_str());
#endif

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
	AnticheatPlugInterface::Tick();

	// screenshots
	{
		// send screenshot
		std::scoped_lock<std::mutex> ssLock(m_ScreenshotMutex);


		// screenshot types that already have a presigned URL
		for (S3ScreenshotEntry screenshotEntry : m_vecGuardedSSData)
		{
			// NOTE: Screenshot types start and end of match are captures and cached in memory until the server tells us where to upload them, so we need to wait for the upload URI

			if ((screenshotEntry.screenshotType == EScreenshotType::SCREENSHOT_TYPE_LOADSCREEN || screenshotEntry.screenshotType == EScreenshotType::SCREENSHOT_TYPE_SCORESCREEN) && screenshotEntry.strSignedURI.empty())
			{
				continue;
			}
			else // we have the data we need, send away
			{
                std::map<std::string, std::string> mapHeaders;
                mapHeaders["Content-Type"] = "image/jpeg";
                NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendS3PUTRequest(screenshotEntry.strSignedURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, screenshotEntry.vecBytes, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
                    {
#if _DEBUG
                        if (statusCode != 200)
                        {
                            __debugbreak();
                        }
#endif
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "Screenshot upload, result: %d", statusCode);
                    }, nullptr, HTTP_UPLOAD_TIMEOUT);
			}
            
		}

		m_vecGuardedSSData.clear();

		// screenshots and replays that are cached and awaiting S3 url, and now have said URL
		if (!m_vecCachedScreenshotBytes_MatchStart.empty()) // we have data waiting
		{
			if (!m_strCachedScreenshot_MatchStart_S3URI.empty()) // and we have a URL
			{
				// queue it
				S3ScreenshotEntry newEntry;
				newEntry.screenshotType = EScreenshotType::SCREENSHOT_TYPE_LOADSCREEN;
				newEntry.vecBytes = m_vecCachedScreenshotBytes_MatchStart;
				newEntry.strSignedURI = m_strCachedScreenshot_MatchStart_S3URI;
				m_vecGuardedSSData.push_back(newEntry);

				// clear data
                m_vecCachedScreenshotBytes_MatchStart = std::vector<uint8_t>();
				m_strCachedScreenshot_MatchStart_S3URI = std::string();
			}
		}

        if (!m_vecCachedScreenshotBytes_MatchEnd.empty()) // we have data waiting
        {
            if (!m_strCachedScreenshot_MatchEnd_S3URI.empty()) // and we have a URL
            {
                // queue it
                S3ScreenshotEntry newEntry;
                newEntry.screenshotType = EScreenshotType::SCREENSHOT_TYPE_SCORESCREEN;
                newEntry.vecBytes = m_vecCachedScreenshotBytes_MatchEnd;
                newEntry.strSignedURI = m_strCachedScreenshot_MatchEnd_S3URI;
				m_vecGuardedSSData.push_back(newEntry);

                // clear data
                m_vecCachedScreenshotBytes_MatchEnd = std::vector<uint8_t>();
                m_strCachedScreenshot_MatchEnd_S3URI = std::string();
            }
        }

        if (!m_vecCachedReplayBytes.empty()) // we have data waiting
        {
            if (!m_strCacheReplay_S3URI.empty()) // and we have a URL
            {
				// do the upload
                std::map<std::string, std::string> mapHeaders;
                mapHeaders["Content-Type"] = "application/octet-stream";
                NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendS3PUTRequest(m_strCacheReplay_S3URI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, m_vecCachedReplayBytes, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
                    {
#if _DEBUG
                        if (statusCode != 200)
                        {
                            __debugbreak();
                        }
#endif

                        NetworkLog(ELogVerbosity::LOG_RELEASE, "Replay upload, result: %d", statusCode);
                    }, nullptr, HTTP_UPLOAD_TIMEOUT);

                // clear data
                NGMP_OnlineServicesManager::GetInstance()->m_vecCachedReplayBytes.clear();
                NGMP_OnlineServicesManager::GetInstance()->m_strCacheReplay_S3URI = std::string();
            }
        }
	}

	if (m_pWebSocket != nullptr)
	{
		m_pWebSocket->Tick();
	}

	if (m_pHTTPManager != nullptr)
	{
		m_pHTTPManager->Tick();
	}

	if (m_pAuthInterface != nullptr)
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
	// Initialize libcurl global state here, before any plugins (e.g. EasyAntiCheat) are loaded.
	// This ensures libcurl's internal mutexes are fully initialized before the EAC plugin
	// attempts to use them, preventing an access violation in mtx_do_lock on null mutex state.
	curl_global_init(CURL_GLOBAL_DEFAULT);

#if !_DEBUG
	std::string strDumpPath = std::format("{}/GeneralsOnlineCrashData/", TheGlobalData->getPath_UserData().str());
	if (!std::filesystem::exists(strDumpPath))
	{
		std::filesystem::create_directory(strDumpPath);
	}

	sentry_options_t* options = sentry_options_new();

	sentry_options_set_dsn(options, "https://61750bebd112d279bcc286d617819269@o4509316925554688.ingest.us.sentry.io/4509316927586304");
	sentry_options_set_database_path(options, strDumpPath.c_str());

	std::string strVersionStr = std::format("generalsonline-client@{}", GENERALS_ONLINE_VERSION_STRING);
	sentry_options_set_release(options, strVersionStr.c_str());

#if defined(USE_TEST_ENV)
	sentry_options_set_environment(options, "test");
#else
	sentry_options_set_environment(options, "production");
#endif

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
	sentry_value_set_by_key(userinfoVal, "user_id", sentry_value_new_string(strUserID.c_str()));
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

	// Disable the crash handler backend to prevent it from attempting to
	// initialize Windows UI components (SystemNavigationManagerStatics::GetForCurrentView)
	// on a non-UI thread during sentry_init(), which causes an access violation.
	sentry_options_set_backend(options, nullptr);

	sentry_init(options);
#endif
}

void NGMP_OnlineServicesManager::ShutdownSentry()
{
	sentry_close();
}


std::string NGMP_OnlineServicesManager::GetPatcherDirectoryPath()
{
	if (!TheGlobalData)
		return {};
	std::string strPatcherDirPath = std::format("{}/GeneralsOnlineData/Update/", TheGlobalData->getPath_UserData().str());
	return strPatcherDirPath;
}

void WebSocket::Shutdown()
{
	// Return immediately if already shut down to prevent double-shutdown
	// (e.g., NGMP_OnlineServicesManager::Shutdown() calls this before releasing the shared_ptr,
	// and then the shared_ptr destructor also calls Shutdown() via ~WebSocket())
	if (m_bShuttingDown)
	{
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Shutdown initiated");
	
	// Signal that we're shutting down
	m_bShuttingDown = true;
	
	// Disconnect from the websocket (handles the connected case)
	Disconnect();

	// Clean up curl easy handle if still active (e.g., mid-connection, not yet fully connected)
	// Disconnect() returns early when m_bConnected is false, so m_pCurlWS may still be alive here
	if (m_pCurlWS != nullptr)
	{
		if (m_pMulti != nullptr)
		{
			curl_multi_remove_handle(m_pMulti, m_pCurlWS);
		}
		curl_easy_cleanup(m_pCurlWS);
		m_pCurlWS = nullptr;
	}

	// Clean up multi handle
	if (m_pMulti != nullptr)
	{
		curl_multi_cleanup(m_pMulti);
		m_pMulti = nullptr;
	}

	// Free headers (may already be freed by Disconnect, but check anyway)
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

// A client watching a lobby it is not a member of subscribes for the same pushes a member
// gets, so the read-only lobby view stays current and can queue its watch at match start.
void WebSocket::SendData_LobbyObserverSubscribe(int64_t lobbyID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_OBSERVER_SUBSCRIBE;
	j["lobby_id"] = lobbyID;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::SendData_LobbyObserverUnsubscribe(int64_t lobbyID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_OBSERVER_UNSUBSCRIBE;
	j["lobby_id"] = lobbyID;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

// A pre-game observer sending chat into a lobby it watches. Own lane from
// SendData_LobbyChatMessage because that path is gated on being a lobby member; the server
// re-broadcasts this as an ordinary LOBBY_CHAT_FROM_SERVER with its own formatting.
void WebSocket::SendData_LobbyObserverChat(int64_t lobbyID, UnicodeString& msg)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_OBSERVER_CHAT_FROM_CLIENT;
	j["lobby_id"] = lobbyID;
	j["message"] = to_utf8(msg.str());
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

// Host-only: ask GO to announce the current pre-game observer roster into the lobby chat.
// GO formats and broadcasts it; this client just renders the announcement like any other.
void WebSocket::SendData_LobbyObserverListRequest(int64_t lobbyID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::LOBBY_OBSERVER_LIST_REQUEST;
	j["lobby_id"] = lobbyID;
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


void WebSocket::SendData_ACMessage(int64_t targetUserID, std::vector<uint8_t> vecPayload)
{
    nlohmann::json j;
    j["msg_id"] = EWebSocketMessageID::ANTICHEAT_MESSAGE;
    j["target_user_id"] = targetUserID;
    j["payload"] = vecPayload;
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
