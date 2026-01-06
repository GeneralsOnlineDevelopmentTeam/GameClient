#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"
#include "GameNetwork/GameSpy/PeerDefs.h"


WebSocket::WebSocket()
{
	m_pMulti = curl_multi_init();
	m_pHeaders = nullptr;
}

WebSocket::~WebSocket()
{
	Shutdown();

	// Cleanup multi handle
	if (m_pMulti != nullptr)
	{
		curl_multi_cleanup(m_pMulti);
		m_pMulti = nullptr;
	}

	if (m_pHeaders != nullptr)
	{
		curl_slist_free_all(m_pHeaders);
		m_pHeaders = nullptr;
	}
}

int WebSocket::Ping()
{
	size_t sent;
	CURLcode result = curl_ws_send(m_pCurlWS, "wsping", strlen("wsping"), &sent, 0,
		CURLWS_PING);

	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::PING;
	std::string strBody = j.dump();

	Send(strBody.c_str());

	return (int)result;
}


void WebSocket::Connect(const char* url, bool bIsReconnect, std::function<void(void)> fnWebsocketConnectedCallback)
{
	if (m_bConnected)
	{
		return;
	}

	m_lastPong = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// Cleanup old easy handle if it exists
	if (m_pCurlWS != nullptr)
	{
		// Must remove from multi handle before cleanup to avoid SSL/certificate processing issues
		if (m_pMulti != nullptr)
		{
			curl_multi_remove_handle(m_pMulti, m_pCurlWS);
		}
		
		// Now safe to cleanup the easy handle
		curl_easy_cleanup(m_pCurlWS);
		m_pCurlWS = nullptr;
	}

	// Free old headers before creating new ones
	if (m_pHeaders != nullptr)
	{
		curl_slist_free_all(m_pHeaders);
		m_pHeaders = nullptr;
	}

	m_pCurlWS = curl_easy_init();

	if (m_pCurlWS != nullptr)
	{
		m_fnWebsocketConnectedCallback = fnWebsocketConnectedCallback;

		int httpResponseCode = -1;
		m_strWebsocketAddr = std::string(url);
		curl_easy_setopt(m_pCurlWS, CURLOPT_URL, url);

		curl_easy_getinfo(m_pCurlWS, CURLINFO_RESPONSE_CODE, &httpResponseCode);

		curl_easy_setopt(m_pCurlWS, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */

#if _DEBUG
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYHOST, 0);

		curl_easy_setopt(m_pCurlWS, CURLOPT_VERBOSE, 1L);
#else
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYHOST, 0);
#endif


		// ws needs auth
		NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		if (pAuthInterface == nullptr)
		{
			return;
		}

		char szHeaderBuffer[8192] = { 0 };
		sprintf_s(szHeaderBuffer, "Authorization: Bearer %s", pAuthInterface->GetAuthToken().c_str());
		m_pHeaders = curl_slist_append(m_pHeaders, szHeaderBuffer);

		sprintf_s(szHeaderBuffer, "is-reconnect: %s", bIsReconnect ? "true": "false");
		m_pHeaders = curl_slist_append(m_pHeaders, szHeaderBuffer);

		curl_easy_setopt(m_pCurlWS, CURLOPT_HTTPHEADER, m_pHeaders);

		//curl_easy_setopt(m_pCurl, CURLOPT_TIMEOUT_MS, 1000);

		/* Perform the request, res gets the return code */
		//CURLcode res = curl_easy_perform(m_pCurl);
		curl_multi_add_handle(m_pMulti, m_pCurlWS);
	}
}