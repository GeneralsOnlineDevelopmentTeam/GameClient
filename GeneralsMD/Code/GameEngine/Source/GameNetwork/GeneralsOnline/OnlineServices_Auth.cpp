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

#pragma comment(lib, "Crypt32.lib")

#if defined(USE_TEST_ENV)
#define CREDENTIALS_FILENAME "credentials_env_test.json"
#elif !defined(DEBUG)
#define CREDENTIALS_FILENAME "credentials.json"
#endif

#include "GameNetwork/GeneralsOnline/vendor/libcurl/curl.h"
#include "GameClient/ClientInstance.h"

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
	std::string session_token;
	std::string refresh_token;
	int64_t user_id = -1;
	std::string display_name = "";
	std::string ws_uri = "";

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(AuthResponse, result, session_token, refresh_token, user_id, display_name, ws_uri)
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
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("MOTD");
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
	std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginWithToken");

	std::string strRefreshToken;
	bool bValidCreds = GetCredentials(strRefreshToken);
	if (bValidCreds)
	{
		// login
		std::map<std::string, std::string> mapHeaders;

		nlohmann::json j;
		j["client_id"] = GENERALS_ONLINE_CLIENT_ID;
		std::string strPostData = j.dump();

		// attach refresh token
		mapHeaders["Authorization"] = "Bearer " + strRefreshToken;

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

						SaveCredentials(authResp.refresh_token.c_str());

						// store data locally
						m_strToken = authResp.session_token;
						m_userID = authResp.user_id;
						m_strDisplayName = authResp.display_name;

						// trigger callback
						OnLoginComplete(true, authResp.ws_uri.c_str());
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

#if _DEBUG
		m_strCode = "ILOVECODE";
#else
		ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
			

			
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
			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("CheckLogin");
			std::map<std::string, std::string> mapHeaders;

			nlohmann::json j;
			j["code"] = m_strCode.c_str();
			j["client_id"] = GENERALS_ONLINE_CLIENT_ID;
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

							SaveCredentials(authResp.refresh_token.c_str());

							// store data locally
							m_strToken = authResp.session_token;
							m_userID = authResp.user_id;
							m_strDisplayName = authResp.display_name;

							// trigger callback
							OnLoginComplete(true, authResp.ws_uri.c_str());
						}
						else if (authResp.result == EAuthResponseResult::FAILED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed");
							m_bWaitingLogin = false;

							// trigger callback
							OnLoginComplete(false, "");
						}
					}
					catch (...)
					{

					}

				}, nullptr);
		}
	}
}

void NGMP_OnlineServices_AuthInterface::OnLoginComplete(bool bSuccess, const char* szWSAddr)
{
	if (bSuccess)
	{
		NGMP_OnlineServicesManager::GetInstance()->OnLogin(bSuccess, szWSAddr);

		// move on to network capabilities section
		ClearGSMessageBoxes();
		GoToDetermineNetworkCaps();
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
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("User"), m_userID);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", nullptr);

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

void NGMP_OnlineServices_AuthInterface::SaveCredentials(const char* szRefreshToken)
{
	// store in data dir
	nlohmann::json root = { {"refresh_token", szRefreshToken} };

	std::string strData = root.dump(1);

	FILE* file = fopen(GetCredentialsFilePath().c_str(), "wb");
	if (file)
	{
#if defined(GENERALS_ONLINE_ENCRYPT_CREDENTIALS)
		DATA_BLOB inputBlob;
		DATA_BLOB outputBlob;

		inputBlob.pbData = (BYTE*)strData.c_str();
		inputBlob.cbData = static_cast<DWORD>(strData.size());

		if (CryptProtectData(&inputBlob, L"GO Credentials", nullptr, nullptr, nullptr, 0, &outputBlob))
		{
			fwrite(outputBlob.pbData, 1, outputBlob.cbData, file);
		}
		else
		{
			// TODO_JWT: Handle failure case
		}
#else
		fwrite(strData.data(), 1, strData.size(), file);
#endif

		fclose(file);
	}
}

bool NGMP_OnlineServices_AuthInterface::GetCredentials(std::string& strRefreshToken)
{
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
		// needs decrypt first
#if defined(GENERALS_ONLINE_ENCRYPT_CREDENTIALS)
		DATA_BLOB encryptedBlob;
		encryptedBlob.pbData = const_cast<BYTE*>(vecBytes.data());
		encryptedBlob.cbData = static_cast<DWORD>(vecBytes.size());
		std::string strJSON;

		DATA_BLOB decryptedBlob = { 0 };
		if (CryptUnprotectData(&encryptedBlob, nullptr, nullptr, nullptr, nullptr, 0, &decryptedBlob))
		{
			strJSON = std::string((char*)decryptedBlob.pbData, decryptedBlob.cbData);
			LocalFree(decryptedBlob.pbData); // Free memory allocated by CryptUnprotectData
		}
		else
		{
			// TODO_JWT: Handle failure
		}
#else
		std::string strJSON = std::string((char*)vecBytes.data(), vecBytes.size());
#endif

		
		nlohmann::json jsonCredentials = nullptr;

		try
		{
			jsonCredentials = nlohmann::json::parse(strJSON);

			if (jsonCredentials != nullptr)
			{
				if (jsonCredentials.contains("refresh_token"))
				{
					strRefreshToken = jsonCredentials["refresh_token"];

					if (strRefreshToken.empty())
					{
						return false;
					}

					return true;
				}
			}

		}
		catch (...)
		{
			return false;
		}
	}

	return false;
}

std::string NGMP_OnlineServices_AuthInterface::GetCredentialsFilePath()
{
	// debug supports multi inst, so needs seperate tokens
#if _DEBUG
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/credentials_dev_env_{}.json", TheGlobalData->getPath_UserData().str(), rts::ClientInstance::getInstanceIndex());
#else
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), CREDENTIALS_FILENAME);
#endif
	return strCredsPath;
}
