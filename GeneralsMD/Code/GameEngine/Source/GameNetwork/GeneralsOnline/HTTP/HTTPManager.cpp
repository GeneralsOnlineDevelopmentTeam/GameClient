#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"

HTTPManager::HTTPManager() noexcept
{
	m_pCurl = curl_multi_init();

	// HTTP background thread
	m_backgroundThread = new std::thread(&HTTPManager::BackgroundThreadRun, this);

	m_bProxyEnabled = DeterminePlatformProxySettings();

	SetThreadDescription(static_cast<HANDLE>(m_backgroundThread->native_handle()), L"HTTP Background Thread");
}

void HTTPManager::SendGETRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback)
{
	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_GET, protover, szURI, inHeaders, completionCallback, progressCallback);

	m_mutex.lock();
	m_vecRequestsPendingstart.push_back(pRequest);
	m_mutex.unlock();
}

void HTTPManager::SendPOSTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szPostData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback)
{
	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_POST, protover, szURI, inHeaders, completionCallback, progressCallback);
	pRequest->SetPostData(szPostData);

	m_mutex.lock();
	m_vecRequestsPendingstart.push_back(pRequest);
	m_mutex.unlock();
}

void HTTPManager::SendPUTRequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/)
{
	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_PUT, protover, szURI, inHeaders, completionCallback, progressCallback);
	pRequest->SetPostData(szData);

	m_mutex.lock();
	m_vecRequestsPendingstart.push_back(pRequest);
	m_mutex.unlock();
}

void HTTPManager::SendDELETERequest(const char* szURI, EIPProtocolVersion protover, std::map<std::string, std::string>& inHeaders, const char* szData, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/)
{
	HTTPRequest* pRequest = PlatformCreateRequest(EHTTPVerb::HTTP_VERB_DELETE, protover, szURI, inHeaders, completionCallback, progressCallback);
	pRequest->SetPostData(szData);

	m_mutex.lock();
	m_vecRequestsPendingstart.push_back(pRequest);
	m_mutex.unlock();
}

void HTTPManager::PlatformThreadedTick_PreLock()
{
	// We do this pre-lock becaues because it doesn't access anything shared (m_vecRequestsInFlight etc), but it does block/consume time, so locking means the game thread wont run logic for a while, dropping fps
	int numReqs = 0;
	curl_multi_perform(m_pCurl, &numReqs);
	curl_multi_poll(m_pCurl, NULL, 0, 1, NULL);
}

void HTTPManager::PlatformThreadedTick_Locked()
{
	// are we done?
	int msgq = 0;
	CURLMsg* m = curl_multi_info_read(m_pCurl, &msgq);
	if (m && (m->msg == CURLMSG_DONE))
	{
		CURL* e = m->easy_handle;

		// find the associated request
		for (HTTPRequest* pRequest : m_vecRequestsInFlight)
		{
			HTTPRequest* pPlatformRequest = static_cast<HTTPRequest*>(pRequest);
			if (pPlatformRequest->EasyHandleMatches(e))
			{
				pPlatformRequest->Threaded_SetComplete();
			}
		}
	}
}

void HTTPManager::Shutdown()
{
	m_mutex.lock();
	curl_multi_cleanup(m_pCurl);
	m_pCurl = nullptr;

	m_mutex.unlock();

	// TODO_HTTP: Wait for background thread to finish
	m_bExitRequested = true;
}

void HTTPManager::BackgroundThreadRun()
{
	// TODO_HTTP: While should be until core is existing
	while (!m_bExitRequested)
	{
		m_mutex.lock();

		PlatformThreadedTick_PreLock();

		PlatformThreadedTick_Locked();

		for (HTTPRequest* pRequest : m_vecRequestsPendingstart)
		{
			if (!pRequest->HasStarted())
			{
				pRequest->StartRequest();
			}

			// add to the proper queue
			m_vecRequestsInFlight.push_back(pRequest);
		}
		m_vecRequestsPendingstart.clear();

		m_mutex.unlock();

		Sleep(1);
	}
}


char* HTTPManager::PlatformEscapeString(const char* szString, int len)
{
	CURL* pCURL = curl_easy_init();
	char* pEsc = curl_easy_escape(pCURL, szString, len);

	// TODO_NGMP: Delete pcurl or keep a persistent one around?
	//delete pCURL;
	return pEsc;
}

HTTPRequest* HTTPManager::PlatformCreateRequest(EHTTPVerb httpVerb, EIPProtocolVersion protover, const char* szURI, std::map<std::string, std::string>& inHeaders, std::function<void(bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)> completionCallback, std::function<void(size_t bytesReceived)> progressCallback /*= nullptr*/) noexcept
{
	HTTPRequest* pNewRequest = new HTTPRequest(httpVerb, protover, szURI, inHeaders, completionCallback, progressCallback);
	return pNewRequest;
}

HTTPManager::~HTTPManager()
{
	Shutdown();
}

void HTTPManager::MainThreadTick()
{
	m_mutex.lock();

	std::vector<HTTPRequest*> vecItemsToRemove = std::vector<HTTPRequest*>();
	for (HTTPRequest* pRequest : m_vecRequestsInFlight)
	{
		// do we need a progress update? We do this here so it comes from the main thread so that UI etc can consume it
		if (pRequest->NeedsProgressUpdate())
		{
			pRequest->InvokeProgressUpdateCallback();
		}

		// are we done?
		if (pRequest->IsComplete())
		{
			vecItemsToRemove.push_back(pRequest);

			// trigger callback
			pRequest->InvokeCallbackIfComplete();
		}
	}

	for (HTTPRequest* pRequestToDestroy : vecItemsToRemove)
	{
		m_vecRequestsInFlight.erase(std::remove(m_vecRequestsInFlight.begin(), m_vecRequestsInFlight.end(), pRequestToDestroy));
		delete pRequestToDestroy;
	}

	m_mutex.unlock();
}
