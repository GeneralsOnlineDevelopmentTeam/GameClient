#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "../NGMP_include.h"
#include "../OnlineServices_Init.h"

HTTPManager::HTTPManager() noexcept
{
	
}

void HTTPManager::SendGETRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback, int timeoutMS)
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_GET, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);

	m_vecRequestsPendingStart.push_back(pRequest);
}

void HTTPManager::SendPOSTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szPostData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback, int timeoutMS, bool bDisableServiceAuth)
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_POST, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);

	if (bDisableServiceAuth)
	{
		pRequest->DisableServiceAuth();
	}

	pRequest->SetPostData(szPostData);

	m_vecRequestsPendingStart.push_back(pRequest);
}

void HTTPManager::SendPUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS)
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_PUT, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
	pRequest->SetPostData(szData);

	m_vecRequestsPendingStart.push_back(pRequest);
}


void HTTPManager::SendS3PUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::vector<uint8_t> vecBuffer, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS /*= -1*/)
{
    CHECK_MAIN_THREAD;

    HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_PUT, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
	pRequest->DisableServiceAuth();
    pRequest->SetPostDataBuffer(vecBuffer);

    m_vecRequestsPendingStart.push_back(pRequest);
}

void HTTPManager::SendDELETERequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS)
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_DELETE, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
	pRequest->SetPostData(szData);

	m_vecRequestsPendingStart.push_back(pRequest);
}

void HTTPManager::Shutdown()
{
	CHECK_MAIN_THREAD;

	// Signal that we're shutting down
	m_bShuttingDown = true;

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTPManager] Shutdown initiated, canceling pending requests...");

	// Cancel all pending requests
	for (HTTPRequest* pRequest : m_vecRequestsPendingStart)
	{
		if (pRequest != nullptr)
		{
			delete pRequest;
		}
	}
	m_vecRequestsPendingStart.clear();

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTPManager] Waiting for %d in-flight requests to complete...", (int)m_vecRequestsInFlight.size());

	// Clear all callbacks on in-flight requests to prevent accessing destroyed objects
	for (HTTPRequest* pRequest : m_vecRequestsInFlight)
	{
		if (pRequest != nullptr)
		{
			pRequest->ClearCallbacks();
		}
	}

	// Wait for all in-flight requests to complete
	if (m_pCurl != nullptr)
	{
		int numRunning = 0;
		do
		{
			// Perform any pending operations
			curl_multi_perform(m_pCurl, &numRunning);
			
			// Check for completed requests
			int msgq = 0;
			CURLMsg* m = nullptr;
			while ((m = curl_multi_info_read(m_pCurl, &msgq)) != nullptr)
			{
				if (m->msg == CURLMSG_DONE)
				{
					CURL* pCurlHandle = m->easy_handle;
					
					// Find and remove the associated request
					for (auto it = m_vecRequestsInFlight.begin(); it != m_vecRequestsInFlight.end(); ++it)
					{
						HTTPRequest* pRequest = *it;
						if (pRequest != nullptr && pRequest->EasyHandleMatches(pCurlHandle))
						{
							// During shutdown, skip invoking the completion callback. The callback
							// may reference objects (e.g. NGMP_OnlineServices_StatsInterface via
							// captured 'this') that have already been or are being destroyed,
							// leading to use-after-free memory corruption and crashes in unrelated
							// destructors such as GameSpyMiscPreferences::~GameSpyMiscPreferences().
							// HTTPRequest::~HTTPRequest() handles all necessary curl handle cleanup.
							delete pRequest;
							m_vecRequestsInFlight.erase(it);
							break;
						}
					}
				}
			}
			
			// Small sleep to avoid busy-waiting if there are still operations pending
			if (numRunning > 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			
		} while (numRunning > 0 || !m_vecRequestsInFlight.empty());
		
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTPManager] All in-flight requests completed");

		// Delete any remaining in-flight requests without invoking their callbacks.
		// These are requests that completed via curl but were not matched above, or
		// requests that are still pending completion. Invoking callbacks here is unsafe
		// as the objects they reference may already be destroyed.
		for (HTTPRequest* pRequest : m_vecRequestsInFlight)
		{
			if (pRequest != nullptr)
			{
				delete pRequest;
			}
		}
		m_vecRequestsInFlight.clear();

		// Now safe to cleanup
		curl_multi_cleanup(m_pCurl);
		m_pCurl = nullptr;

        // Cleanup libcurl global state
        curl_global_cleanup();
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTPManager] Shutdown complete");
}


bool HTTPManager::DeterminePlatformProxySettings()
{
	CHECK_MAIN_THREAD;

	WINHTTP_CURRENT_USER_IE_PROXY_CONFIG pProxyConfig;
	WinHttpGetIEProxyConfigForCurrentUser(&pProxyConfig);

	if (pProxyConfig.lpszProxy != nullptr)
	{
		LPWSTR ws = pProxyConfig.lpszProxy;
		std::string strFullProxy;
		strFullProxy.reserve(wcslen(ws));
		for (; *ws; ws++)
			strFullProxy += (char)*ws;

		size_t ipStart = strFullProxy.find("=");
		ipStart = (ipStart != std::string::npos) ? ipStart + 1 : 0;
		size_t ipEnd = strFullProxy.find(":", ipStart);
		if (ipEnd == std::string::npos) ipEnd = strFullProxy.size();

		m_strProxyAddr = strFullProxy.substr(ipStart, ipEnd - ipStart);

		size_t portStart = ipEnd + 1;
		size_t portEnd = strFullProxy.find(";", portStart);
		std::string strPort = strFullProxy.substr(portStart, portEnd != std::string::npos ? portEnd - portStart : std::string::npos);

		m_proxyPort = (uint16_t)atoi(strPort.c_str());
	}

	m_bProxyEnabled = pProxyConfig.lpszProxy != nullptr;

	if (pProxyConfig.lpszProxy) GlobalFree(pProxyConfig.lpszProxy);
	if (pProxyConfig.lpszAutoConfigUrl) GlobalFree(pProxyConfig.lpszAutoConfigUrl);
	if (pProxyConfig.lpszProxyBypass) GlobalFree(pProxyConfig.lpszProxyBypass);

	return m_bProxyEnabled;
}

HTTPRequest* HTTPManager::PlatformCreateRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS /* = -1 */) noexcept
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pNewRequest = new HTTPRequest(httpVerb, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
	return pNewRequest;
}

std::atomic<bool> HTTPManager::m_bCACertBad = false;

HTTPManager::~HTTPManager()
{
	CHECK_MAIN_THREAD;

	Shutdown();
}

void HTTPManager::Initialize()
{
	CHECK_MAIN_THREAD;

	m_pCurl = curl_multi_init();
	m_bProxyEnabled = DeterminePlatformProxySettings();
}

void HTTPManager::Tick()
{
	CHECK_MAIN_THREAD;

	std::vector<HTTPRequest*> vecItemsToRemove = std::vector<HTTPRequest*>();

	// start anything needing starting
	for (HTTPRequest* pRequest : m_vecRequestsPendingStart)
	{
		pRequest->StartRequest();
		m_vecRequestsInFlight.push_back(pRequest);
	}
	m_vecRequestsPendingStart.clear();

	// perform and poll
	if (m_pCurl == nullptr)
		return;

	int numReqs = 0;
	curl_multi_perform(m_pCurl, &numReqs);
	curl_multi_poll(m_pCurl, NULL, 0, 0, NULL);

#if defined(ARTIFICIAL_DELAY_HTTP_REQUESTS)
	// tick delays
	for (HTTPRequest* pRequest : m_vecRequestsInFlight)
	{
		if (pRequest->WaitingDelayAction())
		{
			bool bDone = pRequest->InvokeDelayAction();
			if (bDone)
			{
				vecItemsToRemove.push_back(pRequest);
			}
		}
		
	}
#endif

	// are we done?
	int msgq = 0;
	CURLMsg* m = nullptr;
	while ((m = curl_multi_info_read(m_pCurl, &msgq)) != nullptr)
	{
		if (m->msg == CURLMSG_DONE)
		{
			CURL* pCurlHandle = m->easy_handle;

			if (pCurlHandle != nullptr)
			{
				// find the associated request
				for (HTTPRequest* pRequest : m_vecRequestsInFlight)
				{
					if (pRequest != nullptr && pRequest->EasyHandleMatches(pCurlHandle))
					{
#if defined(ARTIFICIAL_DELAY_HTTP_REQUESTS)
						pRequest->SetWaitingDelay(m->data.result);
#else
						pRequest->Threaded_SetComplete(m->data.result);
						vecItemsToRemove.push_back(pRequest);
#endif
					}
				}
			}
		}
	}

	// remove any completed
	for (HTTPRequest* pRequestToDestroy : vecItemsToRemove)
	{
		m_vecRequestsInFlight.erase(std::remove(m_vecRequestsInFlight.begin(), m_vecRequestsInFlight.end(), pRequestToDestroy));
		delete pRequestToDestroy;
	}
}

void HTTPManager::AddHandleToMulti(CURL* pNewHandle)
{
	CHECK_MAIN_THREAD;

	curl_multi_add_handle(m_pCurl, pNewHandle);
}

void HTTPManager::RemoveHandleFromMulti(CURL* pHandleToRemove)
{
	CHECK_MAIN_THREAD;

	curl_multi_remove_handle(m_pCurl, pHandleToRemove);
}
