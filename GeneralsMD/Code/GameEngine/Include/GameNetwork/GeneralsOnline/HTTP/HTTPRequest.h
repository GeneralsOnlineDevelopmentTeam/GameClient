#pragma once

#include "GameNetwork/GeneralsOnline/Vendor/libcurl/curl.h"
#include <map>
#include <string>
#include <functional>

enum class EHTTPVerb;
enum class EIPProtocolVersion;

class HTTPRequest
{
public:
	HTTPRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)>
		progressCallback = nullptr) noexcept;
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
	void StartRequest();

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

	void Threaded_SetComplete();

	// mainly used for downloads
	uint8_t* GetBuffer() { return m_pBuffer; }
	size_t GetBufferSize() { return m_currentBufSize_Used; }

private:
	void PlatformStartRequest();

private:
	CURL* m_pCURL = nullptr;

	int m_responseCode = -1;
	std::string m_strResponse;

	EHTTPVerb m_httpVerb;

	EIPProtocolVersion m_protover;

	std::string m_strURI;
	std::string m_strPostData;

	std::map<std::string, std::string> m_mapHeaders;

	std::uint8_t* m_pBuffer = nullptr;
	size_t m_currentBufSize = 0;
	size_t m_currentBufSize_Used = 0;

	const size_t g_initialBufSize = (1024 * 1024) * 1;

	bool m_bNeedsProgressUpdate = false;
	bool m_bIsStarted = false;
	bool m_bIsComplete = false;

	std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> m_completionCallback = nullptr;

	std::function<void(size_t bytesReceived)> m_progressCallback = nullptr;
};