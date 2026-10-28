#pragma once

#include "HTTPRequest.h"
#include <curl/multi.h>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <winhttp.h>
#include "../NGMP_include.h"

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

	void Initialize();

	void Tick();


    void SetProtocolInUse(EIPProtocolVersion proto)
    {
		m_sProtocolInUse.store(proto);
    }

	EIPProtocolVersion GetProtocolInUse()
    {
        return m_sProtocolInUse.load();
    }

	bool AddHandleToMulti(CURL* pNewHandle);
	void RemoveHandleFromMulti(CURL* pHandleToRemove);

	void SendGETRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1);
	// NOTE: set bDisableServiceAuth for endpoints that must be called with something other than the session token (e.g. the refresh endpoint, which needs the refresh token), otherwise the session token overwrites any Authorization header passed in inHeaders
	void SendPOSTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szPostData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1, bool bDisableServiceAuth = false);
	void SendPUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1);
	void SendS3PUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::vector<uint8_t> vecBuffer, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1, int maxSendBytesPerSec = 0);
	void SendDELETERequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1);

	void Shutdown();


private:
	HTTPRequest* PlatformCreateRequest(EHTTPVerb htpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback,
		std::function<void(size_t bytesReceived)> progressCallback = nullptr, int timeoutMS = -1) noexcept;

private:
	CURLM* m_pCurl = nullptr;

	std::atomic<EIPProtocolVersion> m_sProtocolInUse = EIPProtocolVersion::DONT_CARE;


	std::atomic<bool> m_bShuttingDown = false;

	std::vector<HTTPRequest*> m_vecRequestsPendingStart = std::vector<HTTPRequest*>();
	std::vector<HTTPRequest*> m_vecRequestsInFlight = std::vector<HTTPRequest*>();
};

