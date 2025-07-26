#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

size_t WriteMemoryCallback(void* contents, size_t sizePerByte, size_t numBytes, void* userp)
{
	size_t trueNumBytes = sizePerByte * numBytes;

	HTTPRequest* pRequest = (HTTPRequest*)userp;
	pRequest->OnResponsePartialWrite((uint8_t*)contents, trueNumBytes);
	return trueNumBytes;
}

HTTPRequest::HTTPRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback,
	std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS/*= -1*/) noexcept
{
	m_pCURL = curl_easy_init();

	// -1 means use default
	if (timeoutMS > 0)
	{
		m_timeoutMS = timeoutMS;
	}

	m_httpVerb = httpVerb;
	m_protover = protover;
	m_strURI = szURI;
	m_completionCallback = completionCallback;

	m_mapHeaders = inHeaders;

	m_progressCallback = progressCallback;
}

HTTPRequest::~HTTPRequest()
{
	HTTPManager* pHTTPManager = NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager();
	CURLM* pMultiHandle = pHTTPManager->GetMultiHandle();

	curl_multi_remove_handle(pMultiHandle, m_pCURL);
	curl_easy_cleanup(m_pCURL);

	m_vecBuffer.clear();
	m_vecBuffer.resize(0);

    if (headers) {
        curl_slist_free_all(headers);
    }
}

void HTTPRequest::PlatformThreaded_SetComplete()
{
	curl_easy_getinfo(m_pCURL, CURLINFO_RESPONSE_CODE, &m_responseCode);
}

void HTTPRequest::SetPostData(const char* szPostData)
{
	// TODO_HTTP: Error if verb isnt post
	m_strPostData = std::string(szPostData);
}

void HTTPRequest::StartRequest()
{
	m_bIsStarted = true;
	m_bIsComplete = false;

	m_vecBuffer.resize(g_initialBufSize);

	m_currentBufSize_Used = 0;

	PlatformStartRequest();
}

void HTTPRequest::OnResponsePartialWrite(std::uint8_t* pBuffer, size_t numBytes)
{
	// TODO_HTTP: Progress update + buffer sizes are accessed from main thread, probably needs to be atomic or lock

	// TODO_HTTP: Get the size up front, dont realloc
	// TODO_HTTP: Use vector instead

	// do we need a buffer resize?
	if (m_currentBufSize_Used + numBytes >= m_vecBuffer.size())
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p] Doing buffer resize", this);
		m_vecBuffer.resize(m_currentBufSize_Used + numBytes);
	}

	std::copy(pBuffer, pBuffer + numBytes, m_vecBuffer.begin() + m_currentBufSize_Used);
	m_currentBufSize_Used += numBytes;

	NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p] Received: %d bytes", this, numBytes);

	m_bNeedsProgressUpdate = true;
}

void HTTPRequest::InvokeCallbackIfComplete()
{
	if (m_bIsComplete)
	{
		// TODO_HTTP: Assert if not game thread
		if (m_completionCallback != nullptr)
		{
			// Convert m_vecBuffer to std::string for m_strResponse
			std::string strResponse;
			if (!m_vecBuffer.empty() && m_currentBufSize_Used > 0)
			{
				strResponse = std::string(reinterpret_cast<const char*>(m_vecBuffer.data()), m_currentBufSize_Used);
			}
			else
			{
				strResponse.clear();
			}
			m_completionCallback(true, m_responseCode, strResponse, this);
		}
	}
}

void HTTPRequest::Threaded_SetComplete(CURLcode result)
{
	PlatformThreaded_SetComplete();

	m_bIsComplete = true;

	// finalize the size, so we can use .size etc
	m_vecBuffer.resize(m_currentBufSize_Used);

	std::string strURIRedacted = m_strURI;

	size_t tokenpos = strURIRedacted.find("token:");
	if (tokenpos != -1)
	{
		std::string strReplace = "<redacted>";
		const size_t tokenLen = 32;
		strURIRedacted = strURIRedacted.replace(tokenpos + 6, tokenLen, strReplace);
	}

	std::string strResponse = std::string(reinterpret_cast<const char*>(m_vecBuffer.data()), m_currentBufSize_Used);
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s] Transfer is complete: %d bytes total! Curl result is %d", this, strURIRedacted.c_str(), m_currentBufSize_Used, result);

	// if we got an error, set the response code to 0

	std::transform(strResponse.begin(), strResponse.end(), strResponse.begin(),
		[](unsigned char c) { return std::tolower(c); });
	if (strResponse.find("token") != std::string::npos)
	{
		strResponse = "<redacted>";
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s] Response was %d - %s!", this, strURIRedacted.c_str(), m_responseCode, strResponse.c_str());


	// debug write to file
	/*
	std::ofstream file("libcurltest.dat", std::ofstream::out | std::ofstream::binary);
	file.write((const char*)m_pBuffer, m_currentBufSize_Used);
	file.close();
	*/
}

void HTTPRequest::PlatformStartRequest()
{
	if (m_pCURL)
	{
		HTTPManager* pHTTPManager = static_cast<HTTPManager*>(NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager());
		CURLM* pMultiHandle = pHTTPManager->GetMultiHandle();
		curl_multi_add_handle(pMultiHandle, m_pCURL);

		curl_easy_setopt(m_pCURL, CURLOPT_URL, m_strURI.c_str());
		curl_easy_setopt(m_pCURL, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(m_pCURL, CURLOPT_WRITEDATA, (void*)this);
		curl_easy_setopt(m_pCURL, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
		curl_easy_setopt(m_pCURL, CURLOPT_USERAGENT, "GeneralsOnline/1.0");

		
		curl_easy_setopt(m_pCURL, CURLOPT_CONNECTTIMEOUT_MS, m_timeoutMS);
		curl_easy_setopt(m_pCURL, CURLOPT_TIMEOUT_MS, m_timeoutMS);

		if (m_protover == EIPProtocolVersion::DONT_CARE)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		}
		else if (m_protover == EIPProtocolVersion::FORCE_IPV4)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		}
		else if (m_protover == EIPProtocolVersion::FORCE_IPV6)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
		}
		

		// Are we authenticated? attach our auth header
		if (NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->IsLoggedIn())
		{
			m_mapHeaders["Authorization"] = "Bearer " + NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken();
		}

		for (auto& kvPair : m_mapHeaders)
		{
			char szHeaderBuffer[256] = { 0 };
			sprintf_s(szHeaderBuffer, "%s: %s", kvPair.first.c_str(), kvPair.second.c_str());
			headers = curl_slist_append(headers, szHeaderBuffer);
		}
		curl_easy_setopt(m_pCURL, CURLOPT_HTTPHEADER, headers);

		if (m_httpVerb == EHTTPVerb::HTTP_VERB_POST || m_httpVerb == EHTTPVerb::HTTP_VERB_PUT || m_httpVerb == EHTTPVerb::HTTP_VERB_DELETE)
		{
			//if (m_strPostData.length() > 0)
			{
				//char* pEscaped = curl_easy_escape(m_pCURL, m_strPostData.c_str(), m_strPostData.length());
				curl_easy_setopt(m_pCURL, CURLOPT_POSTFIELDS, m_strPostData.c_str());
			}
		}

		// needed for PUT etc
		if (m_httpVerb == EHTTPVerb::HTTP_VERB_PUT)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_CUSTOMREQUEST, "PUT");
		}
		else if (m_httpVerb == EHTTPVerb::HTTP_VERB_DELETE)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_CUSTOMREQUEST, "DELETE");
		}

#if _DEBUG
		if (pHTTPManager->IsProxyEnabled())
		{
			curl_easy_setopt(m_pCURL, CURLOPT_PROXY, pHTTPManager->GetProxyAddress().c_str());
			curl_easy_setopt(m_pCURL, CURLOPT_PROXYPORT, pHTTPManager->GetProxyPort());
		}

		curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYHOST, 0);
		curl_easy_setopt(m_pCURL, CURLOPT_VERBOSE, 1);
#else
		curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYHOST, 0);
#endif
	}
}