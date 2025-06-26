#pragma once

class NGMP_OnlineServices_AuthInterface
{
public:

	AsciiString GetDisplayName()
	{
		return AsciiString(m_strDisplayName.c_str());
	}

	int64_t GetUserID() const { return m_userID; }

	void GoToDetermineNetworkCaps();

	void BeginLogin();

	void Tick();

	void OnLoginComplete(bool bSuccess, const char* szWSAddr, const char* szWSToken);

	void RegisterForLoginCallback(std::function<void(bool)> callback)
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

private:
	void LoginAsSecondaryDevAccount();

	void SaveCredentials(const char* szToken);
	bool DoCredentialsExist();
	std::string GetCredentials();

	std::string GetCredentialsFilePath();

private:
	bool m_bWaitingLogin = false;
	std::string m_strCode;
	std::int64_t m_lastCheckCode = -1;

	std::string m_strToken = std::string();
	int64_t m_userID = -1;
	std::string m_strDisplayName = "NO_USER";

	std::function<void(bool)> m_cb_LoginPendingCallback = nullptr;
};