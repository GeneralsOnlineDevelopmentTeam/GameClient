#pragma once

#include <curl/curl.h>
#include <map>
#include <string>
#include <functional>
#include <chrono>
#include "../ServiceConnectionPolicy.h"

enum class EHTTPVerb;
enum class EIPProtocolVersion;

class HTTPRequest
{
public:
	HTTPRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)>
		progressCallback = nullptr, int timeout = -1) noexcept;
	~HTTPRequest();

	bool EasyHandleMatches(CURL* pHandle)
	{
		if (m_pCURL == nullptr)
		{
			return false;
		}

		return m_pCURL == pHandle;
	}

	void PlatformThreaded_SetComplete();

	void SetPostData(const char* szPostData);
	void SetPostDataBuffer(std::vector<uint8_t> vecBuffer);
	void StartRequest();
	void PollConnectionSetup();

	void DisableServiceAuth()
	{
		m_bAppendAuthIfPresent = false;
	}

	void SetMaxSendSpeed(int bytesPerSec)
	{
		m_maxSendBytesPerSec = bytesPerSec;
	}

	void OnResponsePartialWrite(std::uint8_t* pBuffer, size_t numBytes);

	bool HasStarted() const { return m_bIsStarted; }
	bool IsComplete() const { return m_bIsComplete; }

	bool NeedsProgressUpdate() const { return m_bNeedsProgressUpdate; }
	void InvokeProgressUpdateCallback()
	{
		if (m_progressCallback != nullptr)
		{
			m_progressCallback(m_currentBufSize_Used);
		}
	}

	void InvokeCallbackIfComplete();

#if defined(ARTIFICIAL_DELAY_HTTP_REQUESTS)
	void SetWaitingDelay(CURLcode result);
	bool InvokeDelayAction();
	bool WaitingDelayAction() const { return m_timeRequestComplete != -1; }
#endif
	void Threaded_SetComplete(CURLcode result);

	// mainly used for downloads
	std::vector<uint8_t> GetBuffer() { return m_vecBuffer; }
	size_t GetBufferSize() { return m_vecBuffer.size(); }

	std::string GetURI() { return m_strURI; }

	// route the finished request went through
	ServiceConnection::ProxyRoute GetProxyRoute() const { return m_proxyRoute < m_proxyPolicy.routes.size() ? m_proxyPolicy.routes[m_proxyRoute] : ServiceConnection::ProxyRoute(); }

private:
	void PlatformStartRequest();

private:
	CURL* m_pCURL = nullptr;
	bool m_handleAdded = false;
	std::shared_ptr<ServiceConnection::ProxyResolution> m_proxyResolution;
	ServiceConnection::ResolvedProxyPolicy m_proxyPolicy;
	std::string m_setupFailurePhase;
	size_t m_proxyRoute = 0;
	std::chrono::steady_clock::time_point m_setupStarted;

	long m_responseCode = 0;
	CURLcode m_result = CURLE_OK;

	EHTTPVerb m_httpVerb;

	bool m_bAppendAuthIfPresent = true;

	int m_maxSendBytesPerSec = 0; // 0 = unlimited

	EIPProtocolVersion m_protover;

	int m_timeoutMS = 5000;

	std::string m_strURI;
	std::string m_strPostData;
	std::vector<uint8_t> m_vecPostDataBuffer;

	std::map<std::string, std::string> m_mapHeaders;

	std::vector<uint8_t> m_vecBuffer;
	size_t m_currentBufSize_Used = 0;

#if defined(ARTIFICIAL_DELAY_HTTP_REQUESTS)
	std::int64_t m_timeRequestComplete = -1;
	CURLcode m_pendingCURLCode = CURL_LAST;
#endif

	const size_t g_initialBufSize = (1024 * 32); // 32KB

	bool m_bNeedsProgressUpdate = false;
	bool m_bIsStarted = false;
	bool m_bIsComplete = false;

	struct curl_slist* headers = nullptr;

	std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> m_completionCallback = nullptr;

	std::function<void(size_t bytesReceived)> m_progressCallback = nullptr;

	
};
