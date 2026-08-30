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
	NGMP_OnlineServicesManager* pMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pMgr == nullptr)
		return;

	HTTPManager* pHTTPManager = pMgr->GetHTTPManager();
	pHTTPManager->RemoveHandleFromMulti(m_pCURL);

	curl_easy_cleanup(m_pCURL);

	m_vecBuffer.clear();

    if (headers)
	{
        curl_slist_free_all(headers);
		headers = nullptr;
    }
}


void HTTPRequest::SetPostData(const char* szPostData)
{
	// TODO_HTTP: Error if verb isnt post
	m_strPostData = std::string(szPostData);

	NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p|%s|Verb %d] Transfer is created: Body is %s", this, m_strURI.c_str(), m_httpVerb, szPostData);
}

void HTTPRequest::SetPostDataBuffer(std::vector<uint8_t> vecBuffer)
{
	m_vecPostDataBuffer = std::move(vecBuffer);
}

void HTTPRequest::StartRequest()
{
	m_bIsStarted = true;
	m_bIsComplete = false;

	m_vecBuffer.resize(g_initialBufSize);

	m_currentBufSize_Used = 0;

	NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p|%s|Verb %d] Transfer is starting: Body is %s", this, m_strURI.c_str(), m_httpVerb, m_strPostData.c_str());
	PlatformStartRequest();
}

void HTTPRequest::OnResponsePartialWrite(std::uint8_t* pBuffer, size_t numBytes)
{
	if (m_currentBufSize_Used + numBytes > m_vecBuffer.size())
	{
		size_t newSize = std::max<size_t>(m_vecBuffer.size() * 2, m_currentBufSize_Used + numBytes);
		m_vecBuffer.resize(newSize);
	}

	// do we need a buffer resize?
	if (m_currentBufSize_Used + numBytes >= m_vecBuffer.size())
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p] Doing buffer resize", this);
		m_vecBuffer.resize(m_currentBufSize_Used + numBytes);
	}

	std::copy(pBuffer, pBuffer + numBytes, m_vecBuffer.begin() + m_currentBufSize_Used);
	m_currentBufSize_Used += numBytes;

	NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p] Received: %d bytes", this, numBytes);

	InvokeProgressUpdateCallback();
}

void HTTPRequest::InvokeCallbackIfComplete()
{
	if (m_bIsComplete)
	{
		if (m_completionCallback != nullptr)
		{
			// Move the callback into a local variable and clear the member before
			// invoking it. This prevents a use-after-free: the HTTPRequest may be
			// deleted (by HTTPManager::Tick) immediately after this call returns,
			// which would run ~HTTPRequest and destroy m_completionCallback's
			// captured state while it is still on the call stack.
			auto callback = std::move(m_completionCallback);
			m_completionCallback = nullptr;

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
			callback(true, m_responseCode, strResponse, this);
		}
	}
}

#if defined(ARTIFICIAL_DELAY_HTTP_REQUESTS)
void HTTPRequest::SetWaitingDelay(CURLcode result)
{
	m_timeRequestComplete = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	m_pendingCURLCode = result;
}

bool HTTPRequest::InvokeDelayAction()
{
	if (m_timeRequestComplete != -1)
	{
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		if (currTime - m_timeRequestComplete > 2000)
		{
			Threaded_SetComplete(m_pendingCURLCode);
			return true;
		}
	}

	return false;
}

#endif

void HTTPRequest::Threaded_SetComplete(CURLcode result)
{
	if (result == CURLE_SSL_CACERT_BADFILE || result == CURLE_PEER_FAILED_VERIFICATION)
	{
		HTTPManager::SetCACertStoreBad();
	}
	// store response code
	curl_easy_getinfo(m_pCURL, CURLINFO_RESPONSE_CODE, &m_responseCode);

	if (result == CURLE_OK)
	{
		HTTPManager* pHTTPManager = static_cast<HTTPManager*>(NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager());
		if (pHTTPManager != nullptr)
		{
            if (pHTTPManager->GetProtocolInUse() == EIPProtocolVersion::DONT_CARE)
			{
                char* ip = nullptr;
                curl_easy_getinfo(m_pCURL, CURLINFO_PRIMARY_IP, &ip);

                if (ip)
                {
                    std::string addr(ip);
                    if (addr.find(':') != std::string::npos)
                    {
						pHTTPManager->SetProtocolInUse(EIPProtocolVersion::FORCE_IPV6);
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] We are connected to GO services using IPv6");
                    }
                    else
                    {
						pHTTPManager->SetProtocolInUse(EIPProtocolVersion::FORCE_IPV4);
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] We are connected to GO services using IPv4");
                    }
                }
			}
		}
	}

	m_bIsComplete = true;

	// finalize the size, so we can use .size etc
	m_vecBuffer.resize(m_currentBufSize_Used);

	std::string strURIRedacted = m_strURI;

#if !_DEBUG
	size_t tokenpos = strURIRedacted.find("token:");
	if (tokenpos != -1)
	{
		std::string strReplace = "<redacted>";
		const size_t tokenLen = 32;
		strURIRedacted = strURIRedacted.replace(tokenpos + 6, tokenLen, strReplace);
	}
#endif

	std::string strResponse = std::string(reinterpret_cast<const char*>(m_vecBuffer.data()), m_currentBufSize_Used);
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s|Verb %d] Transfer is complete: %d bytes total! Curl result is %d", this, strURIRedacted.c_str(), m_httpVerb, m_currentBufSize_Used, result);

	// if we got an error, set the response code to 0

#if !_DEBUG
	static const std::string strSeedKey = "\"RNGSeed\":";
	for (size_t seedPos = strResponse.find(strSeedKey); seedPos != std::string::npos; seedPos = strResponse.find(strSeedKey, seedPos))
	{
		size_t valueStart = seedPos + strSeedKey.length();
		size_t valueEnd = strResponse.find_first_of(",}", valueStart);
		if (valueEnd == std::string::npos)
			break;

		strResponse.replace(valueStart, valueEnd - valueStart, "<redacted>");
		seedPos = valueStart;
	}

	std::transform(strResponse.begin(), strResponse.end(), strResponse.begin(),
		[](unsigned char c) { return std::tolower(c); });
	if (strResponse.find("token") != std::string::npos)
	{
		strResponse = "<redacted>";
	}
#endif

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s|Verb %d] Response was %d - %s!", this, strURIRedacted.c_str(), m_httpVerb, m_responseCode, strResponse.c_str());

	// trigger callback
	InvokeCallbackIfComplete();
}

void HTTPRequest::PlatformStartRequest()
{
	if (m_pCURL)
	{
		HTTPManager* pHTTPManager = static_cast<HTTPManager*>(NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager());

		curl_easy_setopt(m_pCURL, CURLOPT_URL, m_strURI.c_str());
		curl_easy_setopt(m_pCURL, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(m_pCURL, CURLOPT_WRITEDATA, (void*)this);
		curl_easy_setopt(m_pCURL, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
		curl_easy_setopt(m_pCURL, CURLOPT_USERAGENT, "GeneralsOnline Client");

		
		curl_easy_setopt(m_pCURL, CURLOPT_CONNECTTIMEOUT_MS, m_timeoutMS);
		curl_easy_setopt(m_pCURL, CURLOPT_TIMEOUT_MS, m_timeoutMS);

		curl_easy_setopt(m_pCURL, CURLOPT_HTTP_VERSION, NGMP_OnlineServicesManager::Settings.Network_GetHTTPVersionForCurl());

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
		

		// Are we authenticated? attach our auth header
		NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		if (pAuthInterface != nullptr && pAuthInterface->IsLoggedIn())
		{
			if (m_bAppendAuthIfPresent)
			{
				m_mapHeaders["Authorization"] = "Bearer " + pAuthInterface->GetAuthToken();
			}
		}

		for (auto& kvPair : m_mapHeaders)
		{
			std::string strHeader = kvPair.first + ": " + kvPair.second;
			headers = curl_slist_append(headers, strHeader.c_str());
		}
		curl_easy_setopt(m_pCURL, CURLOPT_HTTPHEADER, headers);

		if (m_httpVerb == EHTTPVerb::HTTP_VERB_POST || m_httpVerb == EHTTPVerb::HTTP_VERB_PUT || m_httpVerb == EHTTPVerb::HTTP_VERB_DELETE)
		{
			//if (m_strPostData.length() > 0)
			{
				//char* pEscaped = curl_easy_escape(m_pCURL, m_strPostData.c_str(), m_strPostData.length());

				if (!m_vecPostDataBuffer.empty())
				{
                    curl_easy_setopt(m_pCURL, CURLOPT_POSTFIELDS, m_vecPostDataBuffer.data());
					curl_easy_setopt(m_pCURL, CURLOPT_POSTFIELDSIZE, m_vecPostDataBuffer.size());
				}
				else
				{
                    curl_easy_setopt(m_pCURL, CURLOPT_POSTFIELDS, m_strPostData.c_str());
				}
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

		// TODO_NGMP: We should move to libcurl backed by SChannel so we don't need to do this
		// Check if cacert.pem exists

		if (HTTPManager::IsCACertStoreBad())
		{
            curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 0);
            curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYHOST, 0);
		}
		else
		{
            std::ifstream certFile("cacert.pem");
            if (certFile.good())
            {
                certFile.close();
                curl_easy_setopt(m_pCURL, CURLOPT_CAINFO, "cacert.pem");

                curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 1L);
                curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYHOST, 2L);
            }
            else
            {
				HTTPManager::SetCACertStoreBad();
                curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYPEER, 0);
                curl_easy_setopt(m_pCURL, CURLOPT_SSL_VERIFYHOST, 0);
            }
		}

       
#endif

		pHTTPManager->AddHandleToMulti(m_pCURL);
	}
}
