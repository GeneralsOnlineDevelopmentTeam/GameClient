#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Moderation.h"
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
#elif !defined(DEBUG) || defined(USE_DEBUG_ON_LIVE_SERVER)
#define CREDENTIALS_FILENAME "credentials.json"
#endif

#include <curl/curl.h>
#include "GameClient/ClientInstance.h"

enum class EAuthResponseResult : int
{
	CODE_INVALID = -1,
	WAITING_USER_ACTION = 0,
	SUCCEEDED = 1,
	FAILED = 2
};

struct GetLoginCodeResponse
{
	bool success = false;
    std::string login_code = "";
    
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(GetLoginCodeResponse, success, login_code)
};

struct AuthResponse
{
	EAuthResponseResult result = EAuthResponseResult::FAILED;
	std::string session_token = "";
	std::string refresh_token = "";
	int64_t user_id = -1;
	std::string display_name = "";
	std::string ws_uri = "";

	// NOTE: _WITH_DEFAULT so endpoints that only return a subset of these fields (e.g. refresh, which doesn't resend profile data) don't throw during parsing
	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AuthResponse, result, session_token, refresh_token, user_id, display_name, ws_uri)
};

namespace
{
	std::string GetBanReason(const std::string& responseBody)
	{
		nlohmann::json jsonObject = nlohmann::json::parse(responseBody, nullptr, false, true);
		if (jsonObject.is_object())
		{
			const auto banReason = jsonObject.find("ban_reason");
			if (banReason != jsonObject.end() && banReason->is_string())
			{
				return banReason->get_ref<const std::string&>();
			}
		}

		return std::string();
	}
}

struct MOTDResponse
{
	std::string MOTD;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(MOTDResponse, MOTD)
};

void NGMP_OnlineServices_AuthInterface::GoToDetermineNetworkCaps()
{
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

				ELoginResult loginResult = ELoginResult::Success;

				// WS should be connected by this point
				std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
				bool bWSConnected = pWS == nullptr ? false : pWS->IsConnected();
				if (!bWSConnected)
				{
					loginResult = ELoginResult::Failed;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(loginResult);
				}


			}
			catch (...)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "MOTD: Failed to parse response");

				// if MOTD was bad, still proceed, its a soft error
				NGMP_OnlineServicesManager::GetInstance()->ProcessMOTD("Error retrieving MOTD");

				ELoginResult loginResult = ELoginResult::Success;

				// WS should be connected by this point
				std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
				bool bWSConnected = pWS == nullptr ? false : pWS->IsConnected();
				if (!bWSConnected)
				{
					loginResult = ELoginResult::Failed;
				}

				// NOTE: Don't need to get stats here, PopulatePlayerInfoWindows is called as part of going to MP...
				// cache our local stats 
				// 
				// go to next screen
				ClearGSMessageBoxes();

				if (m_cb_LoginPendingCallback != nullptr)
				{
					m_cb_LoginPendingCallback(loginResult);
				}
			}
		});
}

void NGMP_OnlineServices_AuthInterface::SendMiddlewareToken(std::string strMWToken)
{
    std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ProvideMWToken");

    // login
    std::map<std::string, std::string> mapHeaders;

    nlohmann::json j;
	j["mw_token"] = strMWToken;
    std::string strPostData = j.dump();

    NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
        {
            if (!bSuccess || statusCode < 200 || statusCode >= 300)
            {
				NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::MIDDLEWARE_LOGIN_FAILED);
            }
            else
            {
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] MW LOGIN: Logged in");
            }

        }, nullptr);
}

void NGMP_OnlineServices_AuthInterface::OnRefreshTokenFailed(const char* szReason, const std::string& strBody)
{
	// the body can hold a rotated refresh token, so only its size is logged unless verbose
	if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Refresh response body: %s", strBody.c_str());
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Refresh response body was %zu bytes", strBody.size());
	}

	if (m_currentRefreshAttempt < m_maxRefreshAttempts)
	{
		// the token itself is still valid for a few more minutes, so try again shortly instead of waiting for the next scheduled refresh
		m_nextRefreshRetryTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() + (m_secondsUntilRefreshRetry * 1000);

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Token refresh attempt %d of %d failed (%s), retrying in %ds", m_currentRefreshAttempt, m_maxRefreshAttempts, szReason, m_secondsUntilRefreshRetry);
		return;
	}

	// TODO_JWT: More graceful handling
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Token refresh failed after %d attempts (%s), tearing down", m_currentRefreshAttempt, szReason);

	// we couldnt renew our token, so things are about to go really bad
	m_nextRefreshRetryTime = -1;
	m_currentRefreshAttempt = 0;
	NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::AUTH_FAILED);
}

void NGMP_OnlineServices_AuthInterface::RefreshToken()
{
	++m_currentRefreshAttempt;

	// clear any pending retry, this attempt supersedes it
	m_nextRefreshRetryTime = -1;

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Starting token refresh (attempt %d of %d)", m_currentRefreshAttempt, m_maxRefreshAttempts);

	// so we dont keep retrying while the request is pending
	m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

    std::string strRefreshURI = NGMP_OnlineServicesManager::GetAPIEndpoint("RefreshToken");

    if (!m_strRefreshToken.empty())
    {
        std::map<std::string, std::string> mapHeaders;

        // attach refresh token. NOTE: service auth is disabled for this request, otherwise the session token would overwrite this header
        mapHeaders["Authorization"] = "Bearer " + m_strRefreshToken;

        NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strRefreshURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
            {
                if (statusCode >= 400 && statusCode < 500)
                {
					if (statusCode == 423)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Account ban detected during token refresh, tearing down");
						m_tokenCreationTime = -1;
						m_nextRefreshRetryTime = -1;
						m_currentRefreshAttempt = 0;
						HandleModerationDisconnect(EOnlineModerationAction::BAN, GetBanReason(strBody));
						return;
					}

					OnRefreshTokenFailed(std::format("HTTP {}", statusCode).c_str(), strBody);
                }
                else
                {
                    try
                    {
                        nlohmann::json jsonObject = nlohmann::json::parse(strBody, nullptr, false, true);
                        AuthResponse authResp = jsonObject.get<AuthResponse>();

                        if (authResp.result == EAuthResponseResult::SUCCEEDED && !authResp.session_token.empty())
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH]: Token refreshed successfully!");

                            m_currentRefreshAttempt = 0;
                            m_nextRefreshRetryTime = -1;

                            // the refresh endpoint may not rotate the refresh token, only save it if we actually got a new one, otherwise we'd wipe the stored credentials
                            if (!authResp.refresh_token.empty())
                            {
                                SaveCredentials(authResp.refresh_token.c_str());
                            }
                            else
                            {
                                m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
                            }

                            // store data locally
                            m_strToken = authResp.session_token;

                            // refresh responses don't necessarily resend profile data, don't clobber what we already have
                            if (authResp.user_id != -1)
                            {
                                m_userID = authResp.user_id;
                            }

                            if (!authResp.display_name.empty())
                            {
                                m_strDisplayName = authResp.display_name;
                            }
                        }
                        else if (authResp.result == EAuthResponseResult::SUCCEEDED)
                        {
                            OnRefreshTokenFailed("succeeded but no session_token", strBody);
                        }
                        else
                        {
                            OnRefreshTokenFailed(std::format("auth result {}", (int)authResp.result).c_str(), strBody);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        OnRefreshTokenFailed(std::format("malformed response ({})", e.what()).c_str(), strBody);
                    }
                    catch (...)
                    {
                        OnRefreshTokenFailed("malformed response", strBody);
                    }
                }

            }, nullptr, -1, true /* bDisableServiceAuth, we're authenticating with the refresh token here, not the session token */);
    }
    else
    {
		// nothing to refresh with, we're going through the full login flow instead
		DoFullLoginFlow();
    }
}

void NGMP_OnlineServices_AuthInterface::BeginLogin()
{
	auto* manager = NGMP_OnlineServicesManager::GetInstance();
	if (!manager)
	{
		return;
	}
	manager->EnsureServiceRoute([](bool success)
	{
		auto* auth = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		if (!auth)
		{
			return;
		}
		if (success)
		{
			auth->BeginLoginOnSelectedRoute();
		}
		else
		{
			ClearGSMessageBoxes();
			GSMessageBoxOk(UnicodeString(L"Connection failed"), UnicodeString(L"No trusted service route could be reached. Check your connection and try again."), []() { TheShell->pop(); });
		}
	});
}

void NGMP_OnlineServices_AuthInterface::BeginLoginOnSelectedRoute()
{
	m_tokenCreationTime = -1;

	std::string strLoginURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginWithToken");

	std::string strRefreshToken;
	bool bValidCreds = GetCredentials();
	if (bValidCreds && !m_strRefreshToken.empty())
	{
		// login
		std::map<std::string, std::string> mapHeaders;

		nlohmann::json j;
		j["machine_guid"] = GetMachineGuid();
		j["mac_addr"] = GetPrimaryMacAddress();
		j["vol_serial"] = GetVolumeSerial();
		j["exe_crc"] = getGameExeCRC();
		j["ini_crc"] = TheGlobalData->m_iniCRC;
		std::string strPostData = j.dump();

		// attach refresh token. NOTE: service auth is disabled for this request, otherwise a stale session token would overwrite this header
		mapHeaders["Authorization"] = "Bearer " + m_strRefreshToken;


		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strLoginURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				// if 4XX, just log in again
				if (statusCode >= 400 && statusCode < 500)
				{
					if (statusCode == 423)
					{
						ShowLoginBanDialog(GetBanReason(strBody));
						return;
					}
					else
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed due to 4XX code, trying to re-auth");
						DoFullLoginFlow();
					}
				}
				else
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
							OnLoginComplete(ELoginResult::Success, authResp.ws_uri.c_str());
						}
						else if (authResp.result == EAuthResponseResult::FAILED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed, trying to re-auth");
							DoFullLoginFlow();
						}
					}
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Resp parse failed, trying to re-auth");
						DoFullLoginFlow();
					}
				}

			}, nullptr, -1, true /* bDisableServiceAuth, we're authenticating with the refresh token here, not the session token */);
	}
	else
	{
		DoFullLoginFlow();
	}
}

void NGMP_OnlineServices_AuthInterface::DoFullLoginFlow()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: DoFullLoginFlow");

    // get a game login code from server
    std::string strGetLoginCodeURI = NGMP_OnlineServicesManager::GetAPIEndpoint("LoginCode");
    std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strGetLoginCodeURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			std::function<void(void)> fnGetLoginCodeFailed = [this]()
				{
					// stop checking
					m_bWaitingLogin = false;
					m_strCode = std::string();
					m_lastCheckCode = -1;


					ClearGSMessageBoxes();
					GSMessageBoxOk(UnicodeString(L"Login Failed"), UnicodeString(L"Failed to retrieve login code"), []()
						{
							TheShell->pop();
						});
				};

			if (!bSuccess || (statusCode >= 400 && statusCode < 500))
			{
				fnGetLoginCodeFailed();
			}
			else
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody, nullptr, false, true);
					GetLoginCodeResponse authResp = jsonObject.get<GetLoginCodeResponse>();

					if (authResp.success)
					{
						m_currentRefreshAttempt = 0;

						m_bWaitingLogin = true;
						m_lastCheckCode = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

						m_strCode = authResp.login_code;
						if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
						{
							NetworkLog(ELogVerbosity::LOG_DEBUG, "Login Code is %s", m_strCode.c_str());
						}

#if defined(USE_TEST_ENV)
                        std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}&env=test", m_strCode.c_str());
#else
                        std::string strURI = std::format("https://www.playgenerals.online/login/?gamecode={}", m_strCode.c_str());
#endif

                        ClearGSMessageBoxes();
                        GSMessageBoxCancel(UnicodeString(L"Logging In"), UnicodeString(L"Please continue in your web browser"), []()
                            {
                                if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
                                {
                                    NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::USER_REQUESTED_SILENT);
                                }

                                NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
                                if (pAuthInterface != nullptr)
                                {
                                    pAuthInterface->OnLoginComplete(ELoginResult::UserCancelled, "");
                                }
                            });

#if !defined(_DEBUG) || defined(USE_TEST_ENV) || defined(USE_DEBUG_ON_LIVE_SERVER)
                        ShellExecuteA(NULL, "open", strURI.c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
                    }
                    else
                    {
						fnGetLoginCodeFailed();
                    }
                }
                catch (const std::exception& /*e*/)
                {
					fnGetLoginCodeFailed();
                }
                catch (...)
                {
					fnGetLoginCodeFailed();
                }
            }
        });
}

void NGMP_OnlineServices_AuthInterface::Tick()
{
    // Do we need to refresh our token?
    if (IsLoggedIn())
    {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

        if (m_nextRefreshRetryTime != -1)
        {
            // a refresh failed, retry it before the token actually expires
            if (now >= m_nextRefreshRetryTime)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Retrying token refresh...");
                RefreshToken();
            }
        }
        else if (m_tokenCreationTime != -1 && now - m_tokenCreationTime >= m_minutesUntilTokenRefresh * 60 * 1000) // refresh every 10m, tokens last 15m
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AUTH] Token is about to expire, refreshing...");
            m_currentRefreshAttempt = 0;
            RefreshToken();
        }
    }

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
            j["machine_guid"] = GetMachineGuid();
            j["mac_addr"] = GetPrimaryMacAddress();
            j["vol_serial"] = GetVolumeSerial();
            j["exe_crc"] = getGameExeCRC();
            j["ini_crc"] = TheGlobalData->m_iniCRC;
			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					try
					{
						if (statusCode == 423)
						{
							m_bWaitingLogin = false;
							ShowLoginBanDialog(GetBanReason(strBody));
							return;
						}

						nlohmann::json jsonObject = nlohmann::json::parse(strBody);
						AuthResponse authResp = jsonObject.get<AuthResponse>();

						if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "PageBody: %s", strBody.c_str());
						}
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
							OnLoginComplete(ELoginResult::Success, authResp.ws_uri.c_str());
						}
						else if (authResp.result == EAuthResponseResult::FAILED)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "LOGIN: Login failed");
							m_bWaitingLogin = false;

							// trigger callback
							OnLoginComplete(ELoginResult::Failed, "");
						}
					}
					catch (...)
					{

					}

				}, nullptr);
		}
	}
}

void NGMP_OnlineServices_AuthInterface::OnLoginComplete(ELoginResult loginResult, const char* szWSAddr)
{
	if (loginResult == ELoginResult::Success)
	{
		// TODO_AC: Consider chaining this
		// login to AC
		AnticheatPlugInterface::Authenticate();

		NGMP_OnlineServicesManager::GetInstance()->OnLogin(loginResult, szWSAddr, [=]() // wait for WS to connect
			{
                // move on to network capabilities section
                ClearGSMessageBoxes();
                GoToDetermineNetworkCaps();
			});
	}
	else
	{
		if (m_cb_LoginPendingCallback != nullptr)
		{
			m_cb_LoginPendingCallback(loginResult);
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

	std::error_code ec;
	if (std::filesystem::exists(strCredentialsCachePath, ec))
	{
		std::filesystem::remove(strCredentialsCachePath, ec);
	}
}

void NGMP_OnlineServices_AuthInterface::LoginAsSecondaryDevAccount()
{

}

void NGMP_OnlineServices_AuthInterface::SaveCredentials(const char* szRefreshToken)
{
	m_strRefreshToken = std::string(szRefreshToken); // store the new refresh token, we'll need it for the next refresh
	m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// store in data dir
	nlohmann::json root = { {"refresh_token", szRefreshToken} };

	std::string strData = root.dump(1);

	// encrypt before touching the file so a failure doesn't truncate the credentials already stored
	std::string strFileData;
#if defined(GENERALS_ONLINE_ENCRYPT_CREDENTIALS)
	DATA_BLOB inputBlob;
	DATA_BLOB outputBlob;

	inputBlob.pbData = (BYTE*)strData.c_str();
	inputBlob.cbData = static_cast<DWORD>(strData.size());

	if (CryptProtectData(&inputBlob, L"GO Credentials", nullptr, nullptr, nullptr, 0, &outputBlob))
	{
		strFileData.assign((const char*)outputBlob.pbData, outputBlob.cbData);
		LocalFree(outputBlob.pbData);
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to encrypt credentials, keeping the stored ones");
		return;
	}
#else
	strFileData = strData;
#endif

	// write to a temp file and swap it in so a failed write never leaves truncated credentials
	std::string strCredentialsPath = GetCredentialsFilePath();
	std::string strTempPath = strCredentialsPath + ".tmp";
	bool bSaved = false;
	FILE* file = fopen(strTempPath.c_str(), "wb");
	if (file)
	{
		bSaved = fwrite(strFileData.data(), 1, strFileData.size(), file) == strFileData.size();
		if (fclose(file) != 0)
			bSaved = false;
	}

	if (bSaved)
		bSaved = MoveFileExA(strTempPath.c_str(), strCredentialsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;

	if (!bSaved)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to save credentials");
		std::error_code ec;
		std::filesystem::remove(strTempPath, ec);
	}
}

bool NGMP_OnlineServices_AuthInterface::GetCredentials()
{
#if defined(_DEBUG) && !defined(USE_TEST_ENV) && !defined(USE_DEBUG_ON_LIVE_SERVER)
	return false;
#endif
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
					m_strRefreshToken = jsonCredentials["refresh_token"];

					if (m_strRefreshToken.empty())
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
#if defined(_DEBUG) && !defined(USE_TEST_ENV) && !defined(USE_DEBUG_ON_LIVE_SERVER)
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/credentials_dev_env_{}.json", TheGlobalData->getPath_UserData().str(), rts::ClientInstance::getInstanceIndex());
#else
	std::string strCredsPath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), CREDENTIALS_FILENAME);
#endif
	return strCredsPath;
}
