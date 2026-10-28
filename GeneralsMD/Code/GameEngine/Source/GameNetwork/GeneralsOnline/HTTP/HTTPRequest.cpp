#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/ServiceEndpointPolicy.h"
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
	NGMP_OnlineServicesManager* manager = NGMP_OnlineServicesManager::GetInstance();
	if (m_handleAdded && manager != nullptr && manager->GetHTTPManager() != nullptr)
	{
		manager->GetHTTPManager()->RemoveHandleFromMulti(m_pCURL);
	}
	curl_easy_cleanup(m_pCURL);
	curl_slist_free_all(headers);
}


void HTTPRequest::SetPostData(const char* szPostData)
{
	// TODO_HTTP: Error if verb isnt post
	m_strPostData = std::string(szPostData);

	if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p|%s|Verb %d] Transfer is created: Body is %s", this, m_strURI.c_str(), m_httpVerb, szPostData);
	}
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

	if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[%p|%s|Verb %d] Transfer is starting: Body is %s", this, m_strURI.c_str(), m_httpVerb, m_strPostData.c_str());
	}
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
			m_completionCallback(m_result == CURLE_OK, static_cast<int>(m_responseCode), strResponse, this);
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
	// store response code
	curl_easy_getinfo(m_pCURL, CURLINFO_RESPONSE_CODE, &m_responseCode);
	m_result = result;

	if (result == CURLE_OK && NGMP_OnlineServicesManager::IsServiceURLTrusted(m_strURI))
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

	// response bodies can carry tokens and codes, so only their size is logged unless verbose
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s|Verb %d] Completed: curl=%d status=%ld bytes=%zu", this, strURIRedacted.c_str(), m_httpVerb, result, m_responseCode, m_currentBufSize_Used);
	if (NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		std::string strResponse(reinterpret_cast<const char*>(m_vecBuffer.data()), m_currentBufSize_Used);

#if !_DEBUG
		// release builds keep hiding the RNG seed and anything token related even when verbose
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

		std::string strResponseLower = strResponse;
		std::transform(strResponseLower.begin(), strResponseLower.end(), strResponseLower.begin(),
			[](unsigned char c) { return std::tolower(c); });
		if (strResponseLower.find("token") != std::string::npos)
		{
			strResponse = "<redacted>";
		}
#endif

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[%p|%s|Verb %d] Response was %ld - %s!", this, strURIRedacted.c_str(), m_httpVerb, m_responseCode, strResponse.c_str());
	}

	// trigger callback
	InvokeCallbackIfComplete();
}

void HTTPRequest::PlatformStartRequest()
{
	if (m_pCURL == nullptr)
	{
		Threaded_SetComplete(CURLE_FAILED_INIT);
		return;
	}
	std::string error;
	if (!ServiceConnection::ConfigureServiceTransport(m_pCURL, m_strURI, error))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Connection policy: %s", error.c_str());
		Threaded_SetComplete(CURLE_FAILED_INIT);
		return;
	}

	// credentials only ever go to the selected service route
	const bool trusted = NGMP_OnlineServicesManager::IsServiceURLTrusted(m_strURI);
	for (const auto& kvPair : m_mapHeaders)
	{
		if (_stricmp(kvPair.first.c_str(), "Authorization") == 0 && !trusted)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Refused credentials outside selected service route");
			Threaded_SetComplete(CURLE_URL_MALFORMAT);
			return;
		}
	}

	// Are we authenticated? attach our auth header
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (trusted && m_bAppendAuthIfPresent && pAuthInterface != nullptr && pAuthInterface->IsLoggedIn())
	{
		m_mapHeaders["Authorization"] = "Bearer " + pAuthInterface->GetAuthToken();
	}

	for (const auto& kvPair : m_mapHeaders)
	{
		const std::string strHeader = kvPair.first + ": " + kvPair.second;
		curl_slist* updated = curl_slist_append(headers, strHeader.c_str());
		if (updated == nullptr)
		{
			Threaded_SetComplete(CURLE_OUT_OF_MEMORY);
			return;
		}
		headers = updated;
	}

	CURLcode result = CURLE_OK;
	auto option = [&](CURLoption key, auto value) {
		if (result == CURLE_OK)
		{
			result = curl_easy_setopt(m_pCURL, key, value);
		}
	};
	option(CURLOPT_URL, m_strURI.c_str());
	option(CURLOPT_WRITEDATA, static_cast<void*>(this));
	option(CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
	option(CURLOPT_USERAGENT, "GeneralsOnline Client");
	option(CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(m_timeoutMS));
	option(CURLOPT_TIMEOUT_MS, static_cast<long>(m_timeoutMS));
	option(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_NONE));
	option(CURLOPT_IPRESOLVE, static_cast<long>(m_protover == EIPProtocolVersion::FORCE_IPV4 ? CURL_IPRESOLVE_V4 :
		m_protover == EIPProtocolVersion::FORCE_IPV6 ? CURL_IPRESOLVE_V6 : CURL_IPRESOLVE_WHATEVER));
	option(CURLOPT_HTTPHEADER, headers);

	if (m_httpVerb == EHTTPVerb::HTTP_VERB_POST || m_httpVerb == EHTTPVerb::HTTP_VERB_PUT || m_httpVerb == EHTTPVerb::HTTP_VERB_DELETE)
	{
		if (!m_vecPostDataBuffer.empty())
		{
			option(CURLOPT_POSTFIELDS, m_vecPostDataBuffer.data());
			option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(m_vecPostDataBuffer.size()));
		}
		else
		{
			option(CURLOPT_POSTFIELDS, m_strPostData.c_str());
			option(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(m_strPostData.size()));
		}
	}

	if (m_maxSendBytesPerSec > 0)
	{
		option(CURLOPT_MAX_SEND_SPEED_LARGE, static_cast<curl_off_t>(m_maxSendBytesPerSec));
	}

	// needed for PUT etc
	if (m_httpVerb == EHTTPVerb::HTTP_VERB_PUT)
	{
		option(CURLOPT_CUSTOMREQUEST, "PUT");
	}
	else if (m_httpVerb == EHTTPVerb::HTTP_VERB_DELETE)
	{
		option(CURLOPT_CUSTOMREQUEST, "DELETE");
	}

	if (result != CURLE_OK)
	{
		Threaded_SetComplete(result);
		return;
	}

	m_handleAdded = NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->AddHandleToMulti(m_pCURL);
	if (!m_handleAdded)
	{
		Threaded_SetComplete(CURLE_FAILED_INIT);
	}
}
