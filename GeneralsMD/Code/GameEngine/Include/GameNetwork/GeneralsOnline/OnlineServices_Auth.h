#pragma once
#include "NGMP_types.h"

class NGMP_OnlineServices_AuthInterface
{
public:

	std::string GetDisplayName()
	{
		return m_strDisplayName;
	}

	std::wstring GetDisplayNameW()
	{
		return from_utf8(m_strDisplayName);
	}

	int64_t GetUserID() const { return m_userID; }

	void GoToDetermineNetworkCaps();

	void SendMiddlewareToken(std::string strMWToken);

	void RefreshToken();
	void BeginLogin();
	void BeginLoginOnSelectedRoute();
	void DoFullLoginFlow();

	void Tick();

	void OnLoginComplete(ELoginResult loginResult, const char* szWSAddr);

	void RegisterForLoginCallback(std::function<void(ELoginResult)> callback)
	{
		m_cb_LoginPendingCallback = callback;
	}

	void DeregisterForLoginCallback()
	{
		m_cb_LoginPendingCallback = nullptr;
	}

	std::string& GetAuthToken() { return m_strToken; }

	bool IsLoggedIn() const
	{
		return m_userID != -1 && !m_strToken.empty();
	}

	void LogoutOfMyAccount();

	static std::string GetCredentialsFilePath();

private:
	void LoginAsSecondaryDevAccount();

	void SaveCredentials(const char* szRefreshToken);
	bool GetCredentials();

	void OnRefreshTokenFailed(const char* szReason, const std::string& strBody = std::string());

	static std::string CredentialServerKey();

private:
	bool m_bWaitingLogin = false;
	std::string m_strCode;
	std::int64_t m_lastCheckCode = -1;

	std::string m_strToken = std::string();
	int64_t m_userID = -1;
	std::string m_strDisplayName = "NO_USER";

	std::function<void(ELoginResult)> m_cb_LoginPendingCallback = nullptr;

	std::string m_strRefreshToken = std::string();
	int64_t m_tokenCreationTime = -1;
	const int m_minutesUntilTokenRefresh = 10;

	// if a refresh fails we still have ~5m before the token actually expires, so retry quickly rather than waiting for the next scheduled refresh
	int m_currentRefreshAttempt = 0;
	int64_t m_nextRefreshRetryTime = -1;
	const int m_maxRefreshAttempts = 2;
	const int m_secondsUntilRefreshRetry = 30;
};
