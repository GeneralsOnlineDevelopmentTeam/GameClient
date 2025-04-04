#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"

size_t WriteMemoryCallback(void* contents, size_t sizePerByte, size_t numBytes, void* userp)
{
	size_t trueNumBytes = sizePerByte * numBytes;

	HTTPRequest* pRequest = (HTTPRequest*)userp;
	pRequest->OnResponsePartialWrite((uint8_t*)contents, trueNumBytes);
	return trueNumBytes;
}

HTTPRequest::HTTPRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody)> completionCallback,
	std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/) noexcept
{
	m_pCURL = curl_easy_init();

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

	////delete[] m_pBuffer;
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

	// 1MB buffer at first, will resize
	m_pBuffer = (uint8_t*)malloc(g_initialBufSize);
	memset(m_pBuffer, 0, g_initialBufSize);

	m_currentBufSize = g_initialBufSize;
	m_currentBufSize_Used = 0;

	PlatformStartRequest();
}

void HTTPRequest::OnResponsePartialWrite(std::uint8_t* pBuffer, size_t numBytes)
{
	// TODO_HTTP: Progress update + buffer sizes are accessed from main thread, probably needs to be atomic or lock

	// TODO_HTTP: Get the size up front, dont realloc
	// TODO_HTTP: Use vector instead

	// do we need a buffer resize?
	if (m_currentBufSize_Used + numBytes >= m_currentBufSize)
	{
		m_currentBufSize = m_currentBufSize + g_initialBufSize;

		// realloc with an additional 1MB
		m_pBuffer = (uint8_t*)realloc(m_pBuffer, m_currentBufSize);

		// memset new 1mb
		memset(m_pBuffer + m_currentBufSize_Used, 0, m_currentBufSize - m_currentBufSize_Used);

		if (m_pBuffer == NULL)
		{
			// no memory 
			NetworkLog("[HTTP Manager] ERROR: Out of memory");
		}
	}

	memcpy(m_pBuffer + m_currentBufSize_Used, pBuffer, numBytes);
	m_currentBufSize_Used += numBytes;

	NetworkLog("[%p] Received: %d bytes", this, numBytes);

	m_bNeedsProgressUpdate = true;
}

void HTTPRequest::InvokeCallbackIfComplete()
{
	if (m_bIsComplete)
	{
		// TODO_HTTP: Assert if not game thread
		m_completionCallback(true, m_responseCode, m_strResponse);
	}
}

void HTTPRequest::Threaded_SetComplete()
{
	PlatformThreaded_SetComplete();

	m_strResponse = std::string((char*)m_pBuffer, m_currentBufSize_Used);

	m_bIsComplete = true;
	NetworkLog("[%p] Transfer is complete: %d bytes total!", this, m_currentBufSize_Used);

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

		if (m_protover == EIPProtocolVersion::DONT_CARE)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_WHATEVER);
		}
		else if (m_protover == EIPProtocolVersion::FORCE_IPV4)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
		}
		else if (m_protover == EIPProtocolVersion::FORCE_IPV6)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
		}
		

		struct curl_slist* headers = NULL;
		for (auto& kvPair : m_mapHeaders)
		{
			char szHeaderBuffer[256] = { 0 };
			sprintf_s(szHeaderBuffer, "%s: %s", kvPair.first.c_str(), kvPair.second.c_str());
			headers = curl_slist_append(headers, szHeaderBuffer);
		}
		curl_easy_setopt(m_pCURL, CURLOPT_HTTPHEADER, headers);

		if (m_httpVerb == EHTTPVerb::POST || m_httpVerb == EHTTPVerb::PUT)
		{
			//if (m_strPostData.length() > 0)
			{
				//char* pEscaped = curl_easy_escape(m_pCURL, m_strPostData.c_str(), m_strPostData.length());
				curl_easy_setopt(m_pCURL, CURLOPT_POSTFIELDS, m_strPostData.c_str());
			}
		}

		// needed for PUT etc
		if (m_httpVerb == EHTTPVerb::PUT)
		{
			curl_easy_setopt(m_pCURL, CURLOPT_CUSTOMREQUEST, "PUT");
		}

		if (pHTTPManager->IsProxyEnabled())
		{
			curl_easy_setopt(m_pCURL, CURLOPT_PROXY, pHTTPManager->GetProxyAddress().c_str());
			curl_easy_setopt(m_pCURL, CURLOPT_PROXYPORT, pHTTPManager->GetProxyPort());
		}

		curl_easy_setopt(m_pCURL, CURLOPT_VERBOSE, 1);
		curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 0);

		CURLcode res = curl_easy_perform(m_pCURL);
		if (res != CURLE_OK)
		{
			NetworkLog("CURL ERROR: %d", res);
		}
		NetworkLog("RES: %d", res);
	}
}