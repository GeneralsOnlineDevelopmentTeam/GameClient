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
	m_proxyResolution.reset();
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
	m_setupStarted = std::chrono::steady_clock::now();
	m_proxyResolution = ServiceConnection::StartProxyResolution(
		NGMP_OnlineServicesManager::Settings.Network_GetConnectionSettings(), m_strURI);
}

void HTTPRequest::PollConnectionSetup()
{
	if (!m_proxyResolution || m_bIsComplete)
		return;
	if (std::chrono::steady_clock::now() - m_setupStarted >= std::chrono::milliseconds(m_timeoutMS))
	{
		m_proxyResolution.reset();
		m_setupFailurePhase = "proxy-timeout";
		Threaded_SetComplete(CURLE_OPERATION_TIMEDOUT);
		return;
	}
	if (!m_proxyResolution->ready.load(std::memory_order_acquire))
		return;
	m_proxyPolicy = std::move(m_proxyResolution->result);
	m_proxyResolution.reset();
	if (!m_proxyPolicy.error.empty() || m_proxyPolicy.routes.empty())
	{
		m_setupFailurePhase = m_proxyPolicy.failurePhase.empty() ? "proxy-policy" : m_proxyPolicy.failurePhase;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Proxy policy failed: phase=%s native_code=%lu detail=%s",
			m_setupFailurePhase.c_str(), m_proxyPolicy.nativeError, m_proxyPolicy.error.c_str());
		Threaded_SetComplete(CURLE_FAILED_INIT);
		return;
	}
	if (ServiceConnection::ShouldLogProxyPolicy(false, m_proxyPolicy))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Proxy route policy: %zu route(s), first=%s",
			m_proxyPolicy.routes.size(), m_proxyPolicy.routes.front().direct ? "direct" : m_proxyPolicy.routes.front().url.c_str());
		if (!m_proxyPolicy.fallbackReason.empty())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Proxy policy fallback: native_code=%lu detail=%s",
				m_proxyPolicy.nativeError, m_proxyPolicy.fallbackReason.c_str());
		}
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
			m_timeRequestComplete = -1;
			Threaded_SetComplete(m_pendingCURLCode);
			return true;
		}
	}

	return false;
}

#endif

void HTTPRequest::Threaded_SetComplete(CURLcode result)
{
	long proxyStatus = 0;
	curl_easy_getinfo(m_pCURL, CURLINFO_HTTP_CONNECTCODE, &proxyStatus);
	curl_easy_getinfo(m_pCURL, CURLINFO_RESPONSE_CODE, &m_responseCode);
	// CONNECT authentication failures do not populate the origin response code.
	if (proxyStatus == 407)
		m_responseCode = 407;
	if (proxyStatus != 407 && m_responseCode != 407 && (result == CURLE_COULDNT_CONNECT || result == CURLE_COULDNT_RESOLVE_PROXY || result == CURLE_COULDNT_RESOLVE_HOST)
		&& m_proxyRoute + 1 < m_proxyPolicy.routes.size()
		&& std::chrono::steady_clock::now() - m_setupStarted < std::chrono::milliseconds(m_timeoutMS))
	{
		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->RemoveHandleFromMulti(m_pCURL);
		m_handleAdded = false;
		++m_proxyRoute;
		curl_easy_reset(m_pCURL);
		curl_slist_free_all(headers);
		headers = nullptr;
		m_currentBufSize_Used = 0;
		m_vecBuffer.resize(g_initialBufSize);
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Trying next policy route after pre-connection failure (%d)", result);
		PlatformStartRequest();
		return;
	}
	m_result = result;

	if (result == CURLE_OK && m_proxyPolicy.routes[m_proxyRoute].direct && NGMP_OnlineServicesManager::IsServiceURLTrusted(m_strURI))
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

	long protocol = 0;
	curl_easy_getinfo(m_pCURL, CURLINFO_HTTP_VERSION, &protocol);
	// successful requests are only logged in verbose mode
	const bool success = m_setupFailurePhase.empty() && result == CURLE_OK && m_responseCode >= 200 && m_responseCode < 300;
	if (!success || g_LogVerbosity == ELogVerbosity::LOG_DEBUG || NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging())
	{
		const char* method = "UNKNOWN";
		switch (m_httpVerb)
		{
		case EHTTPVerb::HTTP_VERB_GET: method = "GET"; break;
		case EHTTPVerb::HTTP_VERB_POST: method = "POST"; break;
		case EHTTPVerb::HTTP_VERB_PUT: method = "PUT"; break;
		case EHTTPVerb::HTTP_VERB_DELETE: method = "DELETE"; break;
		}
		const auto url = ServiceConnection::RequestURLForLog(m_strURI, NGMP_OnlineServicesManager::GetAPIEndpoint(""));
		if (!m_setupFailurePhase.empty())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Completed: method=%s url=%s source=proxy-policy phase=%s curl=not-started native_code=%lu",
				method, url.c_str(), m_setupFailurePhase.c_str(), m_proxyPolicy.nativeError);
		}
		else
		{
			NetworkLog(success ? ELogVerbosity::LOG_DEBUG : ELogVerbosity::LOG_RELEASE, "[HTTP] Completed: method=%s url=%s phase=%s curl=%d status=%ld proxy_status=%ld protocol=%s bytes=%zu",
				method, url.c_str(), ServiceConnection::FailurePhase(result, m_responseCode, proxyStatus), result, m_responseCode, proxyStatus,
				ServiceConnection::ProtocolName(protocol), m_currentBufSize_Used);
		}

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

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Response body for %s: %s", url.c_str(), strResponse.c_str());
		}
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
	if (m_proxyRoute > 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[HTTP] Route %zu: %s", m_proxyRoute + 1, m_proxyPolicy.routes[m_proxyRoute].direct ? "direct" : m_proxyPolicy.routes[m_proxyRoute].url.c_str());
	}
	std::string error;
	if (!ServiceConnection::ConfigureCurl(m_pCURL, m_strURI, m_proxyPolicy.routes[m_proxyRoute], error))
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
	// proxy lookup and earlier routes share the request deadline
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_setupStarted).count();
	const long remaining = static_cast<long>(std::max<int64_t>(1, m_timeoutMS - elapsed));
	option(CURLOPT_CONNECTTIMEOUT_MS, remaining);
	option(CURLOPT_TIMEOUT_MS, remaining);
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
