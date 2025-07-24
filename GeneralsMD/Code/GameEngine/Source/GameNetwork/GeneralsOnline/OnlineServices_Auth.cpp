#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <random>
#include <windows.h>
#include <wincred.h>
#include "GameNetwork/GameSpyOverlay.h"
#include "../json.hpp"

#if defined(USE_TEST_ENV)
#define CREDENTIALS_FILENAME "credentials_env_test.json"
#else
#define CREDENTIALS_FILENAME "credentials.json"
#endif

#include "GameNetwork/GeneralsOnline/vendor/libcurl/curl.h"
#include "libsodium/sodium/crypto_aead_aes256gcm.h"

enum class EAuthResponseResult : int
{
	CODE_INVALID = -1,
	WAITING_USER_ACTION = 0,
	SUCCEEDED = 1,
	FAILED = 2
};

struct AuthResponse
{
	EAuthResponseResult result;
	std::string ss_token;
	std::string al_token;
	int64_t user_id = -1;
	std::string display_name = "";
	std::string ws_uri = "";
	std::string ws_token = "";

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(AuthResponse, result, ss_token, al_token, user_id, display_name, ws_uri, ws_token)
};

struct MOTDResponse
{
	std::string MOTD;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(MOTDResponse, MOTD)
};

std::string GenerateGamecode()
{
	std::string result;
	const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	const size_t max_index = sizeof(charset) - 1;

	auto seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::mt19937 generator(seed);
	std::uniform_int_distribution<> distribution(0, max_index - 1);

	for (int i = 0; i < 32; ++i) {
		result += charset[distribution(generator)];
	}

	return result;
}

void NGMP_OnlineServices_AuthInterface::GoToDetermineNetworkCaps()
{
	// move on to network capabilities section
	// this is done in the background, but we'll update the MOTD when done to show the latest status
	NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().DetermineLocalNetworkCapabilities();

	// GET MOTD
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("MOTD", true);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				MOTDResponse motdResp = jsonObject.get<MOTDResponse>();

				NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD(motdResp.MOTD.c_str());

				bool bResult = true;

				// WS should be connected by this point
				bool bWSConnected = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->IsConnected();
				if (!bWSConnected)
				{
					bResult = bWSConnected;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(bResult);
				}


			}
			catch (...)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "MOTD: Failed to parse response");

				// if MOTD was bad, still proceed, its a soft error
				NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD("Error retrieving MOTD");

				bool bResult = true;

				// WS should be connected by this point
				bool bWSConnected = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->IsConnected();
				if (!bWSConnected)
				{
					bResult = bWSConnected;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(bResult);
				}
			}
		});
}

void NGMP_OnlineServices_AuthInterface::BeginLogin()
{
	std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginWithToken", false);

#if defined(DEBUG)
	static HANDLE MPMutex = NULL;
	MPMutex = CreateMutex(NULL, FALSE, "685EAFF2-3216-4265-FFFF-251C5F4B82F3");

	if (NGMP_OnlineServicesManager::g_Environment == NGMP_OnlineServicesManager::EEnvironment::DEV || NGMP_OnlineServicesManager::g_Environment == NGMP_OnlineServicesManager::EEnvironment::TEST)
	{
		// use dev account
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Secondary instance detected... using dev account for testing purposes");
		// login
		std::string strToken = "ILOVECODE";
		std::map<std::string, std::string> mapHeaders;

		nlohmann::json j;
		j["token"] = strToken.c_str();
		PrepareChallenge(j);
		std::string strPostData = j.dump();

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					AuthResponse authResp = jsonObject.get<AuthResponse>();

					if (authResp.result == EAuthResponseResult::SUCCEEDED)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Logged in");
						m_bWaitingLogin = false;

						//SaveCredentials(DecryptServiceToken(authResp.al_token).c_str());

						// store data locally
						m_strToken = DecryptServiceToken(authResp.ss_token);
						m_userID = authResp.user_id;
						m_strDisplayName = authResp.display_name;

						// trigger callback
						OnLoginComplete(true, DecryptServiceToken(authResp.ws_uri).c_str(), DecryptServiceToken(authResp.ws_token).c_str());
					}
					else if (authResp.result == EAuthResponseResult::FAILED)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed, dev account cannot reauth");

						m_bWaitingLogin = false;

						// trigger callback
						OnLoginComplete(false, "", "");
					}
				}
				catch (...)
				{

				}

			}, nullptr);
	}
	else
#endif
	{
			if (DoCredentialsExist())
			{
				std::string strToken = GetCredentials();

				// login
				std::map<std::string, std::string> mapHeaders;

				nlohmann::json j;
				j["token"] = strToken.c_str();
				PrepareChallenge(j);
				std::string strPostData = j.dump();

				NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
					{
						try
						{
							nlohmann::json jsonObject = nlohmann::json::parse(strBody, nullptr, false, true);
							AuthResponse authResp = jsonObject.get<AuthResponse>();

							if (authResp.result == EAuthResponseResult::SUCCEEDED)
							{
								ClearGSMessageBoxes();
								GSMessageBoxNoButtons(UnicodeString(L"Logging In"), UnicodeString(L"Logged in!"), true);

								NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Logged in");
								m_bWaitingLogin = false;

								SaveCredentials(DecryptServiceToken(authResp.al_token).c_str());

								// store data locally
								m_strToken = DecryptServiceToken(authResp.ss_token);
								m_userID = authResp.user_id;
								m_strDisplayName = authResp.display_name;

								// trigger callback
								OnLoginComplete(true, DecryptServiceToken(authResp.ws_uri).c_str(), DecryptServiceToken(authResp.ws_token).c_str());
							}
							else if (authResp.result == EAuthResponseResult::FAILED)
							{
								ClearGSMessageBoxes();
								GSMessageBoxNoButtons(UnicodeString(L"Logging In"), UnicodeString(L"Please continue in your web browser"), true);

								NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed, trying to re-auth");

								// do normal login flow, token is bad or expired etc
								m_bWaitingLogin = true;
								m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
								m_strCode = GenerateGamecode();

#if defined(USE_TEST_ENV)
								std::string strURI = std::format("http://www.playgenerals.online/login/?gamecode={}&env=test", m_strCode.c_str());
#else
								std::string strURI = std::format("http://www.playgenerals.online/login/?gamecode={}", m_strCode.c_str());
#endif

								ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
							}
						}
						catch (...)
						{

						}

					}, nullptr);
			}
			else
			{
				m_bWaitingLogin = true;
				m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
				m_strCode = GenerateGamecode();

#if defined(USE_TEST_ENV)
				std::string strURI = std::format("http://www.playgenerals.online/login/?gamecode={}&env=test", m_strCode.c_str());
#else
				std::string strURI = std::format("http://www.playgenerals.online/login/?gamecode={}", m_strCode.c_str());
#endif

				ClearGSMessageBoxes();
				GSMessageBoxNoButtons(UnicodeString(L"Logging In"), UnicodeString(L"Please continue in your web browser"), true);

				ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
			}
	}
}

void NGMP_OnlineServices_AuthInterface::Tick()
{
	if (m_bWaitingLogin)
	{
		const int64_t timeBetweenChecks = 1000;
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

		if (currTime - m_lastCheckCode >= timeBetweenChecks)
		{
			m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

			// check again
			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("CheckLogin", false);
			std::map<std::string, std::string> mapHeaders;

			nlohmann::json j;
			j["code"] = m_strCode.c_str();
			PrepareChallenge(j);
			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					AuthResponse authResp = jsonObject.get<AuthResponse>();

					NetworkLog(ELogVerbosity::LOG_RELEASE, "PageBody: %s", strBody.c_str());
					if (authResp.result == EAuthResponseResult::CODE_INVALID)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Code didnt exist, trying again soon");
					}
					else if (authResp.result == EAuthResponseResult::WAITING_USER_ACTION)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Waiting for user action");
					}
					else if (authResp.result == EAuthResponseResult::SUCCEEDED)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Logged in");
						m_bWaitingLogin = false;

						SaveCredentials(DecryptServiceToken(authResp.al_token).c_str());

						// store data locally
						m_strToken = DecryptServiceToken(authResp.ss_token);
						m_userID = authResp.user_id;
						m_strDisplayName = authResp.display_name;

						// trigger callback
						OnLoginComplete(true, DecryptServiceToken(authResp.ws_uri).c_str(), DecryptServiceToken(authResp.ws_token).c_str());
					}
					else if (authResp.result == EAuthResponseResult::FAILED)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed");
						m_bWaitingLogin = false;

						// trigger callback
						OnLoginComplete(false, "", "");
					}
				}
				catch (...)
				{

				}
				
			}, nullptr);
		}
	}
}

void NGMP_OnlineServices_AuthInterface::OnLoginComplete(bool bSuccess, const char* szWSAddr, const char* szWSToken)
{
	if (bSuccess)
	{
		NGMP_OnlineServicesManager::GetInstance()->OnLogin(bSuccess, szWSAddr, szWSToken);

		// move on to network capabilities section
		ClearGSMessageBoxes();

#if defined(ENABLE_QOS)
		GSMessageBoxNoButtons(UnicodeString(L"Network"), UnicodeString(L"Determining best server region... this could take a few seconds"), true);

		// Get QoS endpoints
		std::string strQoSURI = NGMP_OnlineServicesManager::GetAPIEndpoint("QOS", true);
		std::map<std::string, std::string> mapHeaders;
		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strQoSURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					std::map<std::pair<std::string, EQoSRegions>, std::string> mapQoSEndpoints;

					for (const auto& qosEntry : jsonObject["Servers"])
					{
						std::string strServerURL;
						std::string strRegion;
						EQoSRegions regionID;

						qosEntry["ServerURL"].get_to(strServerURL);
						qosEntry["Region"].get_to(regionID);
						qosEntry["RegionName"].get_to(strRegion);

						mapQoSEndpoints.emplace(std::make_pair<>(strRegion, regionID), strServerURL);
					}

					// TODO_RELAY: The network caps stuff should wait on this finishing, they can run in parallel but should never go forward iwthout determining region
					NGMP_OnlineServicesManager::GetInstance()->GetQoSManager().StartProbing(mapQoSEndpoints, [this]()
						{
							std::map<EQoSRegions, int>& qosData = NGMP_OnlineServicesManager::GetInstance()->GetQoSManager().GetQoSData();

							// inform service of outcome
							//
							nlohmann::json j;
							j["qos_data"] = qosData;
							std::string strPostData = j.dump();

							std::string strQoSURI = NGMP_OnlineServicesManager::GetAPIEndpoint("UserRegion", true);
							std::map<std::string, std::string> mapHeaders;
							NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strQoSURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
								{
									// dont care about the response
								});
							// move on
							GoToDetermineNetworkCaps();
						});
				}
				catch (...)
				{
					// TODO_RELAY: Handle this service side, someone might not have a preferred server set
					GoToDetermineNetworkCaps();
					// NOTE: This is a soft error, if we couldnt get QoS for some reason, we'll pick a relay still, it just wont be the best one
				}
			});
#else
		// move on
		GoToDetermineNetworkCaps();
#endif
	}
	else
	{
		if (m_cb_LoginPendingCallback != nullptr)
		{
			m_cb_LoginPendingCallback(false);
		}

		TheShell->pop();
	}
}

void NGMP_OnlineServices_AuthInterface::LogoutOfMyAccount()
{
	// delete session on service
	nlohmann::json j;
	j["token"] = GetCredentials();
	std::string strBody = j.dump();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("User", true), m_userID);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strBody.c_str(), nullptr);

	// delete local credentials cache
	std::string strCredentialsCachePath = GetCredentialsFilePath();

	if (std::filesystem::exists(strCredentialsCachePath))
	{
		std::filesystem::remove(strCredentialsCachePath);
	}
}

void NGMP_OnlineServices_AuthInterface::LoginAsSecondaryDevAccount()
{

}

void NGMP_OnlineServices_AuthInterface::SaveCredentials(const char* szToken)
{
	// store in data dir
#if !defined(GENERALS_ONLINE_DONT_SAVE_CREDENTIALS)
	nlohmann::json root = {{"token", szToken}};

	std::string strData = root.dump(1);
	FILE* file = fopen(GetCredentialsFilePath().c_str(), "wb");
	if (file)
	{
		fwrite(strData.data(), 1, strData.size(), file);
		fclose(file);
	}
#endif
}

bool NGMP_OnlineServices_AuthInterface::DoCredentialsExist()
{
	std::string strToken = GetCredentials();
	return !strToken.empty();
}

std::string NGMP_OnlineServices_AuthInterface::GetCredentials()
{
	// NGMP_NOTE: Prior to 6/23, tokens were stored in the Windows Credential Manager, this code migrates them to the new json file system
	PCREDENTIAL credential;
	if (CredRead("GeneralsOnline", CRED_TYPE_GENERIC, 0, &credential))
	{
		std::string token = std::string((char*)credential->CredentialBlob, credential->CredentialBlobSize);
		CredFree(credential);

		if (token.length() == 32)
		{
			SaveCredentials(token.c_str());
			CredDelete("GeneralsOnline", CRED_TYPE_GENERIC, 0);
			return token;
		}
	}

	// New system, load from json
	std::vector<uint8_t> vecBytes;
	FILE* file = fopen(GetCredentialsFilePath().c_str(), "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		long fileSize = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (fileSize > 0)
		{
			vecBytes.resize(fileSize);
			fread(vecBytes.data(), 1, fileSize, file);
		}
		fclose(file);
	}


	if (!vecBytes.empty())
	{
		std::string strJSON = std::string((char*)vecBytes.data(), vecBytes.size());
		nlohmann::json jsonCredentials = nullptr;

		try
		{
			jsonCredentials = nlohmann::json::parse(strJSON);

			if (jsonCredentials != nullptr)
			{
				if (jsonCredentials.contains("token"))
				{
					std::string strToken = jsonCredentials["token"];
					return strToken;
				}
			}

		}
		catch (...)
		{
			return std::string();
		}
	}

	return std::string();
}

std::string NGMP_OnlineServices_AuthInterface::GetCredentialsFilePath()
{
	std::string strPatcherDirPath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), CREDENTIALS_FILENAME);
	return strPatcherDirPath;
}
