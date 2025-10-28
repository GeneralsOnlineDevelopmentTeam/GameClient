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

void HTTPManager::SendPOSTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szPostData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback, int timeoutMS)
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_POST, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
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
							// During shutdown, don't invoke callbacks to avoid use-after-free
							pRequest->Threaded_SetComplete(m->data.result, false);
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

		// Now safe to cleanup
		curl_multi_cleanup(m_pCurl);
		m_pCurl = nullptr;
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

HTTPRequest* HTTPManager::PlatformCreateRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/, int timeoutMS /* = -1 */) noexcept
{
	CHECK_MAIN_THREAD;

	HTTPRequest* pNewRequest = new HTTPRequest(httpVerb, protover, szURI, inHeaders, completionCallback, progressCallback, timeoutMS);
	return pNewRequest;
}

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
	CURLMsg* m = curl_multi_info_read(m_pCurl, &msgq);
	
	if (m != nullptr && m->msg == CURLMSG_DONE)
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
					// During normal operation (Tick), invoke callbacks
					pRequest->Threaded_SetComplete(m->data.result, true);
					vecItemsToRemove.push_back(pRequest);
#endif
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
