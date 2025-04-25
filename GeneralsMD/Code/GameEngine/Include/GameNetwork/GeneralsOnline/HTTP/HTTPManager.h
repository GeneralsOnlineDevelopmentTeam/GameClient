#pragma once

#include "HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/Vendor/libcurl/multi.h"
#include <vector>
#include <mutex>
#include <thread>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

enum class EHTTPVerb
{
	HTTP_VERB_GET,
	HTTP_VERB_POST,
	HTTP_VERB_PUT,
	HTTP_VERB_DELETE
};

enum class EIPProtocolVersion
{
	DONT_CARE = 0,
	FORCE_IPV4 = 4,
	FORCE_IPV6 = 6
};

class HTTPManager
{
public:
	HTTPManager() noexcept;
	~HTTPManager();

	void MainThreadTick();

	void SendGETRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr);
	void SendPOSTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szPostData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr);
	void SendPUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr);
	void SendDELETERequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr);

	void Shutdown();

	void BackgroundThreadRun();

	char* PlatformEscapeString(const char* szString, int len);

	bool IsProxyEnabled() const { return m_bProxyEnabled; }

	bool DeterminePlatformProxySettings()
	{
		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG pProxyConfig;
		WinHttpGetIEProxyConfigForCurrentUser(&pProxyConfig);

		if (pProxyConfig.lpszProxy != nullptr)
		{
			LPWSTR ws = pProxyConfig.lpszProxy;
			std::string strFullProxy;
			strFullProxy.reserve(wcslen(ws));
			for (; *ws; ws++)
				strFullProxy += (char)*ws;

			int ipStart = strFullProxy.find("=") + 1;
			int ipEnd = strFullProxy.find(":", ipStart);

			m_strProxyAddr = strFullProxy.substr(ipStart, ipEnd - ipStart);

			int portStart = ipEnd + 1;
			int portEnd = strFullProxy.find(";", portStart);
			std::string strPort = strFullProxy.substr(portStart, portEnd - portStart);

			m_proxyPort = (uint16_t)atoi(strPort.c_str());
		}

		m_bProxyEnabled = pProxyConfig.lpszProxy != nullptr;
		return m_bProxyEnabled;
	}

	std::string& GetProxyAddress() { return m_strProxyAddr; }
	uint16_t GetProxyPort() const { return m_proxyPort; }


	CURLM* GetMultiHandle() { return m_pCurl; }
private:
	HTTPRequest* PlatformCreateRequest(EHTTPVerb htpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback,
		std::function<void(size_t bytesReceived)> progressCallback = nullptr) noexcept;

	void PlatformThreadedTick_PreLock();
	void PlatformThreadedTick_Locked();

private:
	CURLM* m_pCurl = nullptr;

	bool m_bProxyEnabled = false;
	std::string m_strProxyAddr;
	uint16_t m_proxyPort;

	bool m_bExitRequested = false;

	std::thread* m_backgroundThread = nullptr;

	std::recursive_mutex m_mutex;
	std::vector<HTTPRequest*> m_vecRequestsPendingstart = std::vector<HTTPRequest*>();
	std::vector<HTTPRequest*> m_vecRequestsInFlight = std::vector<HTTPRequest*>();
};


