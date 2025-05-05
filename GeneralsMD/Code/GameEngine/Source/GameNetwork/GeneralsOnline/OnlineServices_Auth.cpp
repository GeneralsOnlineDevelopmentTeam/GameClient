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

void NGMP_OnlineServices_AuthInterface::BeginLogin()
{
	std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginWithToken", false);

#if defined(DEBUG)
	static HANDLE MPMutex = NULL;
	MPMutex = CreateMutex(NULL, FALSE, "685EAFF2-3216-4265-FFFF-251C5F4B82F3");

	if (NGMP_OnlineServicesManager::g_Environment == NGMP_OnlineServicesManager::EEnvironment::DEV)
	{
		// use dev account
		NetworkLog("[NGMP] Secondary instance detected... using dev account for testing purposes");
		// login
		std::string strToken = "ILOVECODE";
		std::map<std::string, std::string> mapHeaders;

		nlohmann::json j;
		j["token"] = strToken.c_str();
		j["challenge"] = PrepareChallenge();
		std::string strPostData = j.dump();

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					AuthResponse authResp = jsonObject.get<AuthResponse>();

					if (authResp.result == EAuthResponseResult::SUCCEEDED)
					{
						NetworkLog("LOGIN: Logged in");
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
						NetworkLog("LOGIN: Login failed, dev account cannot reauth");

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
				j["challenge"] = PrepareChallenge();
				std::string strPostData = j.dump();

				NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
					{
						try
						{
							nlohmann::json jsonObject = nlohmann::json::parse(strBody, nullptr, false, true);
							AuthResponse authResp = jsonObject.get<AuthResponse>();

							if (authResp.result == EAuthResponseResult::SUCCEEDED)
							{
								NetworkLog("LOGIN: Logged in");
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
								NetworkLog("LOGIN: Login failed, trying to re-auth");

								// do normal login flow, token is bad or expired etc
								m_bWaitingLogin = true;
								m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
								m_strCode = GenerateGamecode();

								std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}", m_strCode.c_str());

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

				std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}", m_strCode.c_str());

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
			j["challenge"] = PrepareChallenge();
			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					AuthResponse authResp = jsonObject.get<AuthResponse>();

					NetworkLog("PageBody: %s", strBody.c_str());
					if (authResp.result == EAuthResponseResult::CODE_INVALID)
					{
						NetworkLog("LOGIN: Code didnt exist, trying again soon");
					}
					else if (authResp.result == EAuthResponseResult::WAITING_USER_ACTION)
					{
						NetworkLog("LOGIN: Waiting for user action");
					}
					else if (authResp.result == EAuthResponseResult::SUCCEEDED)
					{
						NetworkLog("LOGIN: Logged in");
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
						NetworkLog("LOGIN: Login failed");
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

struct MOTDResponse
{
	std::string MOTD;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(MOTDResponse, MOTD)
};

void NGMP_OnlineServices_AuthInterface::OnLoginComplete(bool bSuccess, const char* szWSAddr, const char* szWSToken)
{
	if (bSuccess)
	{
		NGMP_OnlineServicesManager::GetInstance()->OnLogin(bSuccess, szWSAddr, szWSToken);

		// move on to network capabilities section
		ClearGSMessageBoxes();
		GSMessageBoxNoButtons(UnicodeString(L"Network"), UnicodeString(L"Determining local network capabilities... this may take a few seconds"), true);

		// NOTE: This is partially blocking and partially async...
		NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().DetermineLocalNetworkCapabilities([this]()
			{
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

							for (auto cb : m_vecLogin_PendingCallbacks)
							{
								// TODO_NGMP: Support failure
								cb(bResult);
							}
							m_vecLogin_PendingCallbacks.clear();
							

						}
						catch (...)
						{
							NetworkLog("LOGIN: Failed to parse response");

							// if MOTD was bad, still proceed, its a soft error
							NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD("Error retrieving MOTD");

							// go to next screen
							ClearGSMessageBoxes();

							for (auto cb : m_vecLogin_PendingCallbacks)
							{
								// TODO_NGMP: Support failure
								cb(false);
							}
							m_vecLogin_PendingCallbacks.clear();

							TheShell->pop();
						}
					});
			});
	}
	else
	{
		for (auto cb : m_vecLogin_PendingCallbacks)
		{
			// TODO_NGMP: Support failure
			cb(false);
		}
		m_vecLogin_PendingCallbacks.clear();

		TheShell->pop();
	}
}

void NGMP_OnlineServices_AuthInterface::DeleteMyAccount()
{
	// delete on service
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("User", true), m_userID);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", nullptr);
}

void NGMP_OnlineServices_AuthInterface::LoginAsSecondaryDevAccount()
{

}

void NGMP_OnlineServices_AuthInterface::SaveCredentials(const char* szToken)
{
	// store in credmgr
#if !defined(GENERALS_ONLINE_DONT_SAVE_CREDENTIALS)
	DWORD blobsize = strlen(szToken);

	CREDENTIALA cred = { 0 };
	cred.Flags = 0;
	cred.Type = CRED_TYPE_GENERIC;
	cred.TargetName = (char*)"GeneralsOnline";
	cred.CredentialBlobSize = blobsize;
	cred.CredentialBlob = (LPBYTE)szToken;
	cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
	cred.UserName = (char*)"";

	if (!CredWrite(&cred, 0))
	{
		NetworkLog("ERROR STORING CREDENTIALS: %d\n", GetLastError());
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
	PCREDENTIAL credential;
	if (CredRead("GeneralsOnline", CRED_TYPE_GENERIC, 0, &credential))
	{
		std::string token = std::string((char*)credential->CredentialBlob, credential->CredentialBlobSize);
		CredFree(credential);

		if (token.length() == 32)
		{
			return token;
		}
	}

	return std::string();
}