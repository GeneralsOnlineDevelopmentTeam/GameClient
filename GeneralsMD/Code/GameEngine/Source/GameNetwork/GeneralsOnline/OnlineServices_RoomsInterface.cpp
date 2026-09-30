#include <random>
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Moderation.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameClient/DisplayString.h"

// one clock for every websocket timestamp; utc_clock also loads the time zone database on MSVC
static int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// -----------------------------
// Module info structure
// -----------------------------
struct GOModuleInfo {
    std::string path;
    std::string directory;
    DWORD size;
    void* baseAddress;
    std::string publisher;
    bool isSigned;
};

#include <windows.h>
#include <psapi.h>
#include <wincrypt.h>
#include <softpub.h>
#include <string>
#include <vector>
#include <iostream>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "wintrust.lib")

// -----------------------------
// UTF-8 <-> UTF-16 helpers
// -----------------------------
std::wstring ToWide(const std::string& s) {
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], size);
    out.resize(size - 1);
    return out;
}

std::string ToUtf8(const std::wstring& s) {
    int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], size, nullptr, nullptr);
    out.resize(size - 1);
    return out;
}

std::vector<GOModuleInfo> GetLoadedModules() {
    std::vector<GOModuleInfo> modules;

    HMODULE hMods[1024];
    DWORD cbNeeded;

    HANDLE hProcess = GetCurrentProcess();

    if (!EnumProcessModulesEx(hProcess, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL))
        return modules;

    int count = cbNeeded / sizeof(HMODULE);

    for (int i = 0; i < count; i++) {
        wchar_t wpath[MAX_PATH];
        if (!GetModuleFileNameExW(hProcess, hMods[i], wpath, MAX_PATH))
            continue;

        MODULEINFO modInfo;
        if (!GetModuleInformation(hProcess, hMods[i], &modInfo, sizeof(modInfo)))
            continue;

        std::string path = ToUtf8(wpath);

        std::string directory;
        size_t pos = path.find_last_of("/\\");
        if (pos != std::string::npos)
            directory = path.substr(0, pos);
        else
            directory = "";

        bool isSigned = false;
        //std::string publisher = GetPublisherFromSignature(path, isSigned);
		std::string publisher = "";


        modules.push_back({
            path,
            directory,
            (DWORD)modInfo.SizeOfImage,
            modInfo.lpBaseOfDll,
            publisher,
            isSigned
            });
    }

    return modules;
}


WebSocket::WebSocket()
{
	m_pMulti = curl_multi_init();
	m_pHeaders = nullptr;
}

WebSocket::~WebSocket()
{
	Shutdown();
	// Only call Shutdown if it has not been initiated already.
	// NGMP_OnlineServicesManager::Shutdown() calls Shutdown() before releasing the shared_ptr,
	// so calling it again from the destructor would redundantly block for another 100ms sleep
	// and attempt to free already-released curl resources.
	if (!m_bShuttingDown)
	{
		Shutdown();
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

	m_lastPong = NowMs();

	// TODO_CACHE: Cleanup multi too
	if (m_pCurlWS != nullptr)
	{
        // remove from multi before cleanup (required by libcurl)
        if (m_pMulti != nullptr)
        {
            curl_multi_remove_handle(m_pMulti, m_pCurlWS);
        }
        // cleanup
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

        // HTTP v1 seems to have a higher success rate of bypassing DPI
		curl_easy_setopt(m_pCurlWS, CURLOPT_HTTP_VERSION, NGMP_OnlineServicesManager::Settings.Network_GetHTTPVersionForCurl());

#if _DEBUG
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYHOST, 0);

		curl_easy_setopt(m_pCurlWS, CURLOPT_VERBOSE, 1L);
#else
        // Use the OS cert store (needed since we build curl against OpenSSL, not schannel).
        curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(m_pCurlWS, CURLOPT_SSL_VERIFYHOST, 2L);
#endif


		// ws needs auth
		NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		if (pAuthInterface == nullptr)
		{
			curl_easy_cleanup(m_pCurlWS);
			m_pCurlWS = nullptr;
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

void WebSocket::SendData_RoomChatMessage(UnicodeString& msg, bool bIsAction)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_CLIENT;
	j["message"] = to_utf8(msg.str());
	j["action"] = bIsAction;
	std::string strBody = j.dump(-1, 32, true);

	Send(strBody.c_str());
}

void WebSocket::SendData_MarkReady(bool bReady)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_MARK_READY;
	j["ready"] = bReady;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}


void WebSocket::SendData_JoinNetworkRoom(int roomID, uint64_t requestID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_CHANGE_ROOM;
	j["room"] = roomID;
	if (requestID != 0)
	{
		j["request_id"] = requestID;
	}
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::Disconnect()
{
	if (!m_bConnected)
	{
		return;
	}

	if (m_pCurlWS != nullptr)
	{
		// best-effort flush of anything queued since the last Tick() before closing
		std::vector<std::string> outboundBatch;
		{
			std::scoped_lock<std::mutex> lock(m_outboundQueueMutex);
			outboundBatch.swap(m_vecQueuedOutboungMsgs);
		}

		for (std::string& strPayload : outboundBatch)
		{
			size_t sentPayload;
			CURLcode sendResult = curl_ws_send(m_pCurlWS, strPayload.c_str(), strPayload.length(), &sentPayload, 0, CURLWS_BINARY);
			if (sendResult != CURLE_OK)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Disconnect: failed to flush queued message: %s", curl_easy_strerror(sendResult));
			}
		}

		// send close
		size_t sent;
		(void)curl_ws_send(m_pCurlWS, "", 0, &sent, 0, CURLWS_CLOSE);

		// release headers
		if (m_pHeaders != nullptr)
		{
			curl_slist_free_all(m_pHeaders);
			m_pHeaders = nullptr;
		}


		// Remove from multi handle before cleanup (required by libcurl)
		if (m_pMulti != nullptr)
		{
			curl_multi_remove_handle(m_pMulti, m_pCurlWS);
		}


		// cleanup
		curl_easy_cleanup(m_pCurlWS);
		m_pCurlWS = nullptr;
	}

	m_vecWSPartialBuffer.clear();
	m_bConnected = false;
}

void WebSocket::Send(const char* send_payload)
{
	// Thread-safe; always queues. Tick() flushes on the main thread.
	std::scoped_lock<std::mutex> lock(m_outboundQueueMutex);

	static constexpr size_t kMaxQueuedOutboundMsgs = 256;
	if (m_vecQueuedOutboungMsgs.size() >= kMaxQueuedOutboundMsgs)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Outbound queue full (%zu), discarding oldest message", m_vecQueuedOutboungMsgs.size());
		m_vecQueuedOutboungMsgs.erase(m_vecQueuedOutboungMsgs.begin());
	}

	m_vecQueuedOutboungMsgs.push_back(std::string(send_payload));
}

class WebSocketMessageBase
{
public:
	EWebSocketMessageID msg_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessageBase, msg_id)
};

class WebSocketMessage_ModerationNotice : public WebSocketMessageBase
{
public:
	std::string action_type;
	std::string reason;
	std::string scope_type;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(WebSocketMessage_ModerationNotice, msg_id, action_type, reason, scope_type)
};

class WebSocketMessage_ModerationCommandResult : public WebSocketMessageBase
{
public:
	uint64_t request_id = 0;
	bool success = false;
	std::string error_code;
	std::string message;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
		WebSocketMessage_ModerationCommandResult,
		msg_id,
		request_id,
		success,
		error_code,
		message)
};

class WebSocketMessage_NetworkStartSignalling : public WebSocketMessageBase
{
public:
	int64_t lobby_id;
	int64_t user_id;
	uint16_t preferred_port;
	std::string middleware_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkStartSignalling, msg_id, lobby_id, user_id, preferred_port, middleware_id)
};

class WebSocketMessage_ACRegisterPlayer : public WebSocketMessageBase
{
public:
    int64_t user_id;
    std::string mwid;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_ACRegisterPlayer, msg_id, user_id, mwid)
};

class WebSocketMessage_ACDeregisterPlayer : public WebSocketMessageBase
{
public:
    int64_t user_id;
    std::string mwid;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_ACDeregisterPlayer, msg_id, user_id, mwid)
};

class WebSocketMessage_NetworkDisconnectPlayer : public WebSocketMessageBase
{
public:
	int64_t lobby_id;
	int64_t user_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkDisconnectPlayer, msg_id, lobby_id, user_id)
};

class WebSocketMessage_MatchmakingAction_JoinPrearrangedLobby : public WebSocketMessageBase
{
public:
	int64_t lobby_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_MatchmakingAction_JoinPrearrangedLobby, msg_id, lobby_id)
};


class WebSocketMessage_RoomChatIncoming : public WebSocketMessageBase
{
public:
	std::string message;
	bool action;
	bool admin;
	bool name_change;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_RoomChatIncoming, msg_id, message, action, admin, name_change)
};

class WebSocketMessage_Social_FriendChatMessage_Incoming : public WebSocketMessageBase
{
public:
	int64_t source_user_id;
	int64_t target_user_id;
	std::string message;
	
	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_Social_FriendChatMessage_Incoming, msg_id, source_user_id, target_user_id, message)
};

class WebSocketMessage_Social_FriendStatusChanged : public WebSocketMessageBase
{
public:
	std::string display_name;
	bool online;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_Social_FriendStatusChanged, display_name, online)
};

class WebSocketMessage_Social_FriendRequestAccepted : public WebSocketMessageBase
{
public:
	std::string display_name;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_Social_FriendRequestAccepted, display_name)
};

class WebSocketMessage_FriendsOverallStatusUpdate : public WebSocketMessageBase
{
public:
	int num_online;
	int num_pending;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_FriendsOverallStatusUpdate, num_online, num_pending)
};

class WebSocketMessage_NetworkSignal : public WebSocketMessageBase
{
public:
	int64_t target_user_id = -1;
	std::vector<uint8_t> payload;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkSignal, target_user_id, payload)
};

class WebSocketMessage_AnticheatMessage : public WebSocketMessageBase
{
public:
    int64_t target_user_id = -1;
    std::vector<uint8_t> payload;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_AnticheatMessage, target_user_id, payload)
};

class WebSocketMessage_ServerProbe : public WebSocketMessageBase
{
public:
	std::string url;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_ServerProbe, msg_id, url)
};

class WebSocketMessage_StartGameResponse : public WebSocketMessageBase
{
public:
    std::string screenshot_url;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_StartGameResponse, msg_id, screenshot_url)
};

class WebSocketMessage_LobbyChatIncoming : public WebSocketMessageBase
{
public:
	std::string message;
	bool action;
	bool announcement;
	bool show_announcement_to_host;
	int64_t user_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_LobbyChatIncoming, msg_id, message, action, announcement, show_announcement_to_host, user_id)
};

class WebSocketMessage_MatchmakingMessage : public WebSocketMessageBase
{
public:
	std::string message;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_MatchmakingMessage, msg_id, message)
};

class WebSocketMessage_Social_NewFriendRequest : public WebSocketMessageBase
{
public:
	std::string display_name;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_Social_NewFriendRequest, msg_id, display_name)
};

static bool JSONDeserialize(const char* szBuffer, nlohmann::json* jsonObject)
{
	try
	{
		*jsonObject = nlohmann::json::parse(szBuffer);
		return true;
	}
	catch (nlohmann::json::exception& jsonException)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "JSONDeserialize: Unparsable JSON: %s (%s)", szBuffer, jsonException.what());
		return false;
	}
	catch (...)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "JSONDeserialize: Unparsable JSON: %s", szBuffer);
		return false;
	}

	return false;
}

template<typename T>
static bool JSONGetAsObject(nlohmann::json& jsonObject, T* outMsg)
{
	try
	{
		*outMsg = jsonObject.get<T>();

		return true;
	}
	catch (nlohmann::json::exception& jsonException)
	{
		std::string targetTypeName = typeid(T).name();
		NetworkLog(ELogVerbosity::LOG_RELEASE, "JSONGetAsObject: Unparsable JSON: Target Type is %s (%s)", targetTypeName.c_str(), jsonException.what());
		return false;
	}
	catch (...)
	{
		std::string targetTypeName = typeid(T).name();
		NetworkLog(ELogVerbosity::LOG_RELEASE, "JSONGetAsObject: Unparsable JSON: Target Type is %s", targetTypeName.c_str());
		return false;
	}

	return false;
}

//static std::string strSignal = "str:1 ";

// Idempotent: repeated drop signals must not reset the backoff.
void WebSocket::BeginReconnect()
{
	if (m_bReconnecting)
	{
		return;
	}

	m_bReconnecting = true;
	m_numReconnectAttempts = 0;
	m_bFreshSession = false;
	m_reconnectAttemptStarted = -1;
	m_reconnectStartTime = NowMs();
	m_nextReconnectAttempt = m_reconnectStartTime + ComputeReconnectDelay();
}

void WebSocket::EndReconnect()
{
	m_bReconnecting = false;
	m_reconnectAttemptStarted = -1;
}

// First retry 0.5-3s, later ones 60-100% of the step (2s..10s cap), so clients that dropped together spread out.
int64_t WebSocket::ComputeReconnectDelay() const
{
	static thread_local std::mt19937 rng{ std::random_device{}() };

	int64_t lo = m_reconnectBackoffBase / 4;
	int64_t hi = m_reconnectBackoffBase * 3 / 2;
	if (m_numReconnectAttempts > 0)
	{
		int64_t step = m_reconnectBackoffBase;
		for (int i = 1; i < m_numReconnectAttempts && step < m_reconnectBackoffCap; ++i)
		{
			step *= 2;
		}
		step = std::min<int64_t>(step, m_reconnectBackoffCap);
		lo = step * 6 / 10;
		hi = step;
	}

	return std::uniform_int_distribution<int64_t>(lo, hi)(rng);
}

// Restores what the old session had once a fresh one is connected.
static void RestoreSessionState()
{
	NGMP_OnlineServices_SocialInterface* pSocial = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pSocial == nullptr)
	{
		return;
	}

	pSocial->GetFriendsList(false, nullptr);
	pSocial->GetBlockList(nullptr);
	if (pSocial->IsOverlayActive())
	{
		pSocial->RegisterForRealtimeServiceUpdates();
	}

	// resolve the previous room by stable ID against a freshly fetched room list, not by index
	NGMP_OnlineServices_RoomsInterface* pRooms = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRooms == nullptr)
	{
		return;
	}

	const std::vector<NetworkRoom>& roomsBeforeRefresh = pRooms->GetGroupRooms();
	const int previousRoomIndex = pRooms->GetCurrentRoomIndex();
	std::optional<int> previousRoomID;
	UnicodeString strPreviousRoomName;
	if (previousRoomIndex >= 0 && previousRoomIndex < (int)roomsBeforeRefresh.size())
	{
		previousRoomID = roomsBeforeRefresh[previousRoomIndex].GetRoomID();
		strPreviousRoomName = roomsBeforeRefresh[previousRoomIndex].GetRoomDisplayName();
	}

	pRooms->GetRoomList([pRooms, previousRoomID, strPreviousRoomName](bool bSuccess)
		{
			const std::vector<NetworkRoom>& rooms = pRooms->GetGroupRooms();
			if (!bSuccess || rooms.empty())
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] RestoreSessionState: failed to fetch room list, cannot rejoin a room");
				return;
			}

			// fall back to the default room (index 0) if the room we were in no longer exists
			int roomIndexToJoin = 0;
			bool bFoundPreviousRoom = false;
			if (previousRoomID.has_value())
			{
				for (size_t i = 0; i < rooms.size(); ++i)
				{
					if (rooms[i].GetRoomID() == *previousRoomID)
					{
						roomIndexToJoin = (int)i;
						bFoundPreviousRoom = true;
						break;
					}
				}
			}

			if (previousRoomID.has_value() && !bFoundPreviousRoom)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] RestoreSessionState: room '%s' (id %d) no longer exists, joining the default room instead", to_utf8(strPreviousRoomName.str()).c_str(), *previousRoomID);
			}

			pRooms->JoinRoom(roomIndexToJoin);
		});
}

void WebSocket::UpdateReconnect()
{
	if (!m_bReconnecting)
	{
		return;
	}

	// Teardown already pending (kick, ban, auth failure): stop reconnecting.
	if (NGMP_OnlineServicesManager::GetInstance()->IsPendingFullTeardown())
	{
		EndReconnect();
		return;
	}

	const int64_t currTime = NowMs();
	const bool bGameInProgress = TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress();

	// Session gone server-side: nothing to retry during a match, the window starts after it.
	if (m_bFreshSession && bGameInProgress)
	{
		m_reconnectStartTime = currTime;
		m_numReconnectAttempts = 0;
		m_reconnectAttemptStarted = -1;
		m_nextReconnectAttempt = currTime + ComputeReconnectDelay();
		return;
	}

	// Connect() kills the previous handle: leave a pending attempt alone unless stuck.
	if (m_reconnectAttemptStarted != -1 && (currTime - m_reconnectAttemptStarted) < m_reconnectAttemptTimeout)
	{
		return;
	}

	if (!bGameInProgress && (currTime - m_reconnectStartTime) >= m_reconnectMenuWindow)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (%d reconnect attempts failed)", m_numReconnectAttempts);
		NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
		m_bConnected = false;
		m_vecWSPartialBuffer.clear();
		EndReconnect();
		return;
	}

	// New session only once out of the lobby the server dropped us from.
	if (currTime < m_nextReconnectAttempt || (m_bFreshSession && TheNGMPGame != nullptr))
	{
		return;
	}

	++m_numReconnectAttempts;
	m_reconnectAttemptStarted = currTime;
	Connect(m_strWebsocketAddr.c_str(), !m_bFreshSession, m_bFreshSession ? std::function<void()>(RestoreSessionState) : nullptr);
}

void WebSocket::Tick()
{
	// Main thread only; m_pCurlWS/m_vecWSPartialBuffer/m_bConnected are unlocked here.
	UpdateReconnect();



	/*
	if (strSignal.length() == 6)
	{
		for (int i = 0; i < 5000 - 6; ++i)
		{
			if (i == 5000 - 6 - 1)
			{
				strSignal += "+";
			}
			else
			{
				strSignal += i % 2 == 0 ? 'a' : 'b';
			}
		}
	}

	WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();;
	pWS->SendData_Signalling(strSignal);
	*/

	// ping?
	int64_t currTime = NowMs();
	if ((currTime - m_lastPing) > m_timeBetweenUserPings)
	{
		m_lastPing = currTime;
		Ping();
	};

    int numReqs = 0;
    curl_multi_perform(m_pMulti, &numReqs);
    curl_multi_poll(m_pMulti, NULL, 0, 0, NULL);

    {
        // Check for completed requests (initial connection only)
        int msgq = 0;
        CURLMsg* m = nullptr;
        while ((m = curl_multi_info_read(m_pMulti, &msgq)) != nullptr)
        {
            if (m->msg == CURLMSG_DONE)
            {
                CURL* pCurlHandle = m->easy_handle;

                if (pCurlHandle == m_pCurlWS) // shouldnt hear about anything else
                {
					int httpResponseCode = -1;
					curl_easy_getinfo(pCurlHandle, CURLINFO_RESPONSE_CODE, &httpResponseCode);

					/* Check for errors */
                    if (m->data.result != CURLE_OK)
                    {
                        m_bConnected = false;
                        m_vecWSPartialBuffer.clear();
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Failed to connect (%d - %s)", m->data.result, curl_easy_strerror(m->data.result));

                        if (m_bReconnecting)
                        {
                            if (!m_bFreshSession && m->data.result == CURLE_HTTP_RETURNED_ERROR && httpResponseCode == 205) // 205: session gone, can't resume
                            {
                                NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Server dropped our session (205), will reconnect as a new one");
                                m_bFreshSession = true;
                                m_reconnectAttemptStarted = -1;
                                m_reconnectStartTime = NowMs();
                                m_numReconnectAttempts = 0;
                                m_nextReconnectAttempt = m_reconnectStartTime + ComputeReconnectDelay();
                            }
                            else
                            {
                                m_reconnectAttemptStarted = -1;
                                m_nextReconnectAttempt = NowMs() + ComputeReconnectDelay();
                            }
                        }
                        else // give up immediately
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (initial connect)");
                            NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
                            m_bConnected = false;
                            m_vecWSPartialBuffer.clear();
                        }
                    }
                    else
                    {
                        if (m_bReconnecting)
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Re-Connected");
                        }
                        else
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Connected");
                        }

                        /* connected and ready */
                        m_bConnected = true;
                        m_vecWSPartialBuffer.clear();

                        EndReconnect();

                        // connecting is as good as a pong
                        m_lastPong = NowMs();

                        if (m_fnWebsocketConnectedCallback != nullptr)
                        {
                            m_fnWebsocketConnectedCallback();
                        }
                    }
                }
            }
        }
    }

    if (!m_bConnected)
    {
        return;
    }

	// send anything we have queued (things sent while not connected, or from any other thread)
	std::vector<std::string> outboundBatch;
	{
		std::scoped_lock<std::mutex> lock(m_outboundQueueMutex);
		outboundBatch.swap(m_vecQueuedOutboungMsgs);
	}

	for (std::string& strPayload : outboundBatch)
	{
        size_t sent;
        CURLcode result = curl_ws_send(m_pCurlWS, strPayload.c_str(), strPayload.length(), &sent, 0, CURLWS_BINARY);

        if (result != CURLE_OK)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "curl_ws_send() failed: %s\n", curl_easy_strerror(result));
        }
	}

	// do recv
	size_t rlen = 0;
	const struct curl_ws_frame* meta = nullptr;
	char bufferThisRecv[8196 * 4] = { 0 };

	CURLcode ret = CURL_LAST;
	ret = curl_ws_recv(m_pCurlWS, bufferThisRecv, sizeof(bufferThisRecv), &rlen, &meta);

	// SECURITY FIX: Validate rlen is within buffer bounds
	if (rlen > sizeof(bufferThisRecv))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Received data size %zu exceeds buffer size %zu, discarding", rlen, sizeof(bufferThisRecv));
		return;
	}

	if (ret != CURLE_RECV_ERROR && ret != CURL_LAST && ret != CURLE_AGAIN && ret != CURLE_GOT_NOTHING)
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket msg: %s", bufferThisRecv);
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket len: %d", rlen);

		// any server frame proves liveness, not just a JSON PONG
		m_lastPong = NowMs();

		// what type of message?
		if (meta != nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket flags: %d", meta->flags);
			if (meta->flags & CURLWS_PONG) // PONG
			{

			}
			else if (meta->flags & CURLWS_TEXT)
			{
				bool bMessageComplete = false;

				static constexpr size_t MAX_WS_PARTIAL_SIZE = 2 * 1024 * 1024; // 2 MB
				if (m_vecWSPartialBuffer.size() + rlen > MAX_WS_PARTIAL_SIZE)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Partial buffer overflow, discarding message");
					m_vecWSPartialBuffer.clear();
					return;
				}
				
				// SECURITY FIX: Store old size BEFORE resize to avoid off-by-one error in memcpy
				size_t oldSize = m_vecWSPartialBuffer.size();
				m_vecWSPartialBuffer.resize(oldSize + rlen);
				memcpy_s(m_vecWSPartialBuffer.data() + oldSize, rlen, bufferThisRecv, rlen);

				if (meta->flags & CURLWS_CONT)
				{
					bMessageComplete = false;
					NetworkLog(ELogVerbosity::LOG_DEBUG, "WEBSOCKET PARTIAL (CONT) OF SIZE %d, offset %d, bytes left %d! [MESSAGE COMPLETE: %d]", rlen, meta->offset, meta->bytesleft, bMessageComplete);
				}
				else if (meta->bytesleft > 0)
				{
					bMessageComplete = false;
					NetworkLog(ELogVerbosity::LOG_DEBUG, "WEBSOCKET PARTIAL (BYTESLEFT) OF SIZE %d, offset %d! [MESSAGE COMPLETE: %d]", rlen, meta->offset, bMessageComplete);
				}
				else
				{
					// if we got in here, it's a whole message, or the last part of a fragmented message
					bMessageComplete = true;
					NetworkLog(ELogVerbosity::LOG_DEBUG, "WEBSOCKET LAST FRAME OF SIZE %d!", rlen);
				}

				if (bMessageComplete)
				{
					try
					{
						// null terminate buffer
						m_vecWSPartialBuffer.push_back('\0');

						// process it
						nlohmann::json jsonObject;
						bool bDeserializedOK = JSONDeserialize(m_vecWSPartialBuffer.data(), &jsonObject);

						// clear buffer and resize
						m_vecWSPartialBuffer.clear();
						m_vecWSPartialBuffer.resize(0);

						if (bDeserializedOK)
						{
							if (jsonObject.contains("msg_id"))
							{
								WebSocketMessageBase msgDetails;
								bool bParsedBase = JSONGetAsObject<WebSocketMessageBase>(jsonObject, &msgDetails);

								if (bParsedBase)
								{
									EWebSocketMessageID msgID = msgDetails.msg_id;

									switch (msgID)
									{
									case EWebSocketMessageID::MODERATION_NOTICE:
									{
										WebSocketMessage_ModerationNotice moderationNotice;
										if (JSONGetAsObject(jsonObject, &moderationNotice))
										{
											HandleModerationNotice(
												moderationNotice.action_type,
												moderationNotice.reason,
												moderationNotice.scope_type);
										}
									}
									break;

									case EWebSocketMessageID::MODERATION_COMMAND_RESULT:
									{
										WebSocketMessage_ModerationCommandResult commandResult;
										if (JSONGetAsObject(jsonObject, &commandResult))
										{
											NetworkLog(
												ELogVerbosity::LOG_RELEASE,
												"Moderation command %llu %s: %s",
												static_cast<unsigned long long>(commandResult.request_id),
												commandResult.success ? "succeeded" : "failed",
												commandResult.message.c_str());
										}
									}
									break;

									case EWebSocketMessageID::PONG:
									{
										int64_t currTime = NowMs();
										m_lastPong = currTime;
									}
									break;

									case EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_SERVER:
									{
										WebSocketMessage_RoomChatIncoming chatData;
										bool bParsed = JSONGetAsObject(jsonObject, &chatData);

										if (bParsed)
										{
											SYSTEMTIME systemTime;
											GetLocalTime(&systemTime);

											// TheSuperHackers @feature Keep the reading order of the name and the message
											// when either contains right-to-left text. Actions have no brackets to split at.
											UnicodeString message(from_utf8(chatData.message).c_str());
											if (!chatData.action)
												message = IsolateBidiChatLine(message);

											UnicodeString unicodeStr;
											unicodeStr.format(L"[%2.2d:%2.2d] %s", systemTime.wHour, systemTime.wMinute, message.str());

											Color color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_NETWORK_ROOM, true, chatData.action, chatData.admin, chatData.name_change);

											NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
											if (pRoomsInterface != nullptr && pRoomsInterface->m_OnChatCallback != nullptr)
											{
												pRoomsInterface->m_OnChatCallback(unicodeStr, color);
											}
										}
									}
									break;

									case EWebSocketMessageID::SOCIAL_FRIEND_CHAT_MESSAGE_SERVER_TO_CLIENT:
									{
										WebSocketMessage_Social_FriendChatMessage_Incoming chatData;
										bool bParsed = JSONGetAsObject(jsonObject, &chatData);

										if (bParsed)
										{
											UnicodeString unicodeStr(from_utf8(chatData.message).c_str());

											NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
											if (pSocialInterface != nullptr)
											{
												pSocialInterface->OnChatMessage(chatData.source_user_id, chatData.target_user_id, unicodeStr);
											}
										}
									}
									break;

									case EWebSocketMessageID::SOCIAL_FRIEND_ONLINE_STATUS_CHANGED:
									{
										WebSocketMessage_Social_FriendStatusChanged statusChangedData;
										bool bParsed = JSONGetAsObject(jsonObject, &statusChangedData);

										if (bParsed)
										{
											NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
											if (pSocialInterface != nullptr)
											{
												pSocialInterface->OnOnlineStatusChanged(statusChangedData.display_name, statusChangedData.online);
											}
										}
									}
									break;

									case EWebSocketMessageID::SOCIAL_FRIEND_FRIEND_REQUEST_ACCEPTED_BY_TARGET:
									{
										WebSocketMessage_Social_FriendRequestAccepted statusChangedData;
										bool bParsed = JSONGetAsObject(jsonObject, &statusChangedData);

										if (bParsed)
										{
											NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
											if (pSocialInterface != nullptr)
											{
												pSocialInterface->OnFriendRequestAccepted(statusChangedData.display_name);
											}
										}
									}
									break;

									case EWebSocketMessageID::SOCIAL_FRIENDS_LIST_DIRTY:
									{
                                        // nothing to parse here, it's just an event only
                                        extern void updateBuddyInfo(bool bIsAutoRefresh = false, bool bUseCache = false);
										updateBuddyInfo(true);
									}
									break;

									case EWebSocketMessageID::SOCIAL_CANT_ADD_FRIEND_LIST_FULL:
									{
										// always show this notification, it's tied to a local user action
										showNotificationBox(AsciiString::TheEmptyString, UnicodeString(L"Cannot sent friends request. Your friends list is full."));
									}
									break;

									case EWebSocketMessageID::SOCIAL_FRIENDS_OVERALL_STATUS_UPDATE:
									{
										WebSocketMessage_FriendsOverallStatusUpdate statusUpdateData;
										bool bParsed = JSONGetAsObject(jsonObject, &statusUpdateData);

										if (bParsed)
										{
											UnicodeString strFormat = UnicodeString::TheEmptyString;
											if (statusUpdateData.num_online > 0 && statusUpdateData.num_pending > 0)
											{
												strFormat.format(L"You have %d friend(s) online and %d pending friend request(s)", statusUpdateData.num_online, statusUpdateData.num_pending);
											}
											else if (statusUpdateData.num_online > 0)
											{
												strFormat.format(L"You have %d friend(s) online.", statusUpdateData.num_online);
											}
											else if (statusUpdateData.num_pending > 0)
											{
												strFormat.format(L"You have %d pending friend request(s)", statusUpdateData.num_pending);
											}
											else
											{
												strFormat = UnicodeString(L"Press F5 or INSERT to bring up the communicator at any time (including in-game).");
											}

											// show it on the communicator too
											if (statusUpdateData.num_pending > 0)
											{
                                                NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
                                                if (pSocialInterface != nullptr)
                                                {
													pSocialInterface->RegisterInitialPendingRequestsUponLogin(statusUpdateData.num_pending);
                                                }
											}

											if (!strFormat.isEmpty())
											{
												// always show this notification
												showNotificationBox(AsciiString::TheEmptyString, strFormat);
											}
										}
									}
									break;

									case EWebSocketMessageID::START_GAME:
									{
                                        WebSocketMessage_StartGameResponse startGameData;
                                        bool bParsed = JSONGetAsObject(jsonObject, &startGameData);

										if (bParsed)
										{
											// store URL
                                            NGMP_OnlineServicesManager::GetInstance()->SetScreenshotS3URI_StartMatch(startGameData.screenshot_url.c_str());
										}

										// always start, even if we couldnt parse the url
                                        NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
                                        if (pLobbyInterface != nullptr && pLobbyInterface->m_callbackStartGamePacket != nullptr)
                                        {
                                            pLobbyInterface->m_callbackStartGamePacket();
                                        }
									}
									break;

									case EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE:
									{
										int64_t meshCheckID = 0;
										int meshCheckAttempt = 0;
										if (jsonObject.contains("mesh_check_id") && jsonObject["mesh_check_id"].is_number_integer())
										{
											meshCheckID = jsonObject["mesh_check_id"].get<int64_t>();
										}
										if (jsonObject.contains("attempt") && jsonObject["attempt"].is_number_integer())
										{
											meshCheckAttempt = jsonObject["attempt"].get<int>();
										}

										// respond with our state
										std::vector<int64_t> connectivityMap;
										std::vector<int64_t> connectingMap;
										NetworkMesh* pMesh = nullptr;
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr)
										{
											pMesh = pLobbyInterface->GetNetworkMeshForLobby();
										}

										if (pMesh != nullptr)
										{
											for (auto& conn : pMesh->GetAllConnections())
											{
												int64_t userID = conn.first;
												PlayerConnection& playerConn = conn.second;

												if (playerConn.GetState() == EConnectionState::CONNECTED_DIRECT)
												{
													// NOTE: Useful for testing
													//if (userID != 1)
													{
														connectivityMap.push_back(userID);
													}
												}
												else if (playerConn.GetState() == EConnectionState::CONNECTING_DIRECT || playerConn.GetState() == EConnectionState::FINDING_ROUTE)
												{
													// still negotiating: lets the service hold off restarting it
													connectingMap.push_back(userID);
												}
											}
										}

										// send response
										nlohmann::json j;
										j["msg_id"] = EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE;
										j["mesh_check_id"] = meshCheckID;
										j["attempt"] = meshCheckAttempt;
										j["connectivity_map"] = connectivityMap;
										j["connecting_map"] = connectingMap;
										std::string strBody = j.dump();

										Send(strBody.c_str());
										break;
									}

									case EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST:
									{
										// all checks are done, process start for host

										// stale reply for a lobby we've since left/changed
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										int64_t currentLobbyID = pLobbyInterface != nullptr ? pLobbyInterface->GetCurrentLobby().lobbyID : -1;
										if (currentLobbyID != m_connectivityCheckLobbyID)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Ignoring stale reply for a previous lobby");
											break;
										}

										bool bMeshComplete = false;
										std::string strReason;
										std::list<std::pair<int64_t, int64_t>> missingConnections;

										try
										{
											jsonObject["mesh_complete"].get_to(bMeshComplete);

											if (jsonObject.contains("reason"))
											{
												jsonObject["reason"].get_to(strReason);
											}

											if (!bMeshComplete)
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Mesh is not complete for someone, reason: %s", strReason.c_str());
												for (const auto& missingConnectionEntryIter : jsonObject["missing_connections"])
												{
													int64_t source_user_id = -1;
													int64_t target_user_id = -1;

													missingConnectionEntryIter["source_user_id"].get_to(source_user_id);
													missingConnectionEntryIter["target_user_id"].get_to(target_user_id);

													missingConnections.push_back(std::make_pair(source_user_id, target_user_id));
												}
											}
											else
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Mesh is fully complete");
											}
										}
										catch (...)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Error processing response, resolving as failure");
											bMeshComplete = false;
											missingConnections.clear();
											strReason = "parse_error";
										}

										// invoke callback
										if (m_cbOnConnectivityCheckComplete != nullptr)
										{
											m_cbOnConnectivityCheckComplete(bMeshComplete, missingConnections, strReason);
										}

										ClearConnectivityCheckCallback();

										break;
									}

									case EWebSocketMessageID::NETWORK_CONNECTION_START_SIGNALLING:
									{
										WebSocketMessage_NetworkStartSignalling startSignallingData;
										bool bParsed = JSONGetAsObject(jsonObject, &startSignallingData);

										// TODO_NGMP: Better location for this
										// When we find a new player, get their latest stats. Tooltip and loading screen need it, so we'll grab it now and then use cached data later since it cannot possibly change while in a lobby
										NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
										if (pStatsInterface != nullptr)
										{
											pStatsInterface->findPlayerStatsByID(startSignallingData.user_id, [=](bool bSuccess, PSPlayerStats stats)
												{

												}, EStatsRequestPolicy::BYPASS_CACHE_FORCE_REQUEST);
										}

										if (bParsed)
										{
											NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
											if (pLobbyInterface != nullptr)
											{
												NetworkMesh* pMesh = pLobbyInterface->GetNetworkMeshForLobby();

												if (pMesh != nullptr)
												{
                                                    pMesh->StartConnectionSignalling(startSignallingData.middleware_id.c_str(), startSignallingData.user_id, startSignallingData.preferred_port);
                                                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_START_SIGNALLING] Starting signalling with %lld (MWID: %s)", startSignallingData.user_id, startSignallingData.middleware_id.c_str());
												}
												else
												{
													NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_START_SIGNALLING] Network mesh is null");
													break;
												}
											}
											else
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_START_SIGNALLING] Lobby interface is null");
												break;
											}
										}
									}
									break;

									case EWebSocketMessageID::AC_REGISTER_PLAYER:
                                    {
                                        WebSocketMessage_ACRegisterPlayer acData;
                                        bool bParsed = JSONGetAsObject(jsonObject, &acData);

										if (bParsed)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Websocket AC_REGISTER_PLAYER for %lld and %s", acData.user_id, acData.mwid);
											if (!AnticheatPlugInterface::RegisterPlayer(acData.mwid, acData.user_id))
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AnticheatPlugInterface::RegisterPlayer failed");
											}
										}
                                    }
                                    break;

                                    case EWebSocketMessageID::AC_DEREGISTER_PLAYER:
                                    {
                                        WebSocketMessage_ACDeregisterPlayer acData;
                                        bool bParsed = JSONGetAsObject(jsonObject, &acData);

										if (bParsed)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Websocket AC_DEREGISTER_PLAYER for %lld and %s", acData.user_id, acData.mwid);
											if (!AnticheatPlugInterface::DeregisterPlayer(acData.mwid, acData.user_id))
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AnticheatPlugInterface::DeregisterPlayer failed");
											}
										}
                                    }
                                    break;

									case EWebSocketMessageID::NETWORK_CONNECTION_DISCONNECT_PLAYER:
									{
										WebSocketMessage_NetworkDisconnectPlayer disconnectPlayerData;
										bool bParsed = JSONGetAsObject(jsonObject, &disconnectPlayerData);

										if (bParsed)
										{
											NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
											if (pLobbyInterface != nullptr)
											{
												int64_t currentLobbyID = pLobbyInterface->GetCurrentLobby().lobbyID;

												if (currentLobbyID == -1 || currentLobbyID != disconnectPlayerData.lobby_id)
												{
													NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_DISCONNECT_PLAYER] Lobby ID mismatch! Expected %lld, got %lld", currentLobbyID, disconnectPlayerData.lobby_id);
													break;
												}

												NetworkMesh* pMesh = pLobbyInterface->GetNetworkMeshForLobby();

												if (pMesh != nullptr)
												{
													pMesh->DisconnectUser(disconnectPlayerData.user_id);
												}
												else
												{
													NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_DISCONNECT_PLAYER] Network mesh is null");
													break;
												}
											}
											else
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_DISCONNECT_PLAYER] Lobby interface is null");
												break;
											}
										}
									}
									break;

									case EWebSocketMessageID::NETWORK_SIGNAL:
									{
										NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] GOT SIGNAL!");

										WebSocketMessage_NetworkSignal signalData;
										bool bParsed = JSONGetAsObject(jsonObject, &signalData);

										if (bParsed)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] Signal User: %lld!", signalData.target_user_id);
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] Signal Payload Size: %d!", (int)signalData.payload.size());
											PushPendingSignal(std::move(signalData.payload));
										}
									}
									break;

									case EWebSocketMessageID::LOBBY_CHAT_FROM_SERVER:
									{
										WebSocketMessage_LobbyChatIncoming chatData;
										bool bParsed = JSONGetAsObject(jsonObject, &chatData);

										if (bParsed)
										{
											UnicodeString unicodeStr(from_utf8(chatData.message).c_str());
											// TheSuperHackers @feature Keep the reading order of the name and the message
											// when either contains right-to-left text.
											if (!chatData.action && !chatData.announcement)
												unicodeStr = IsolateBidiChatLine(unicodeStr);

											NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
											if (pLobbyInterface != nullptr)
											{
												int lobbySlot = -1;
												auto lobbyMembers = pLobbyInterface->GetMembersListForCurrentRoom();
												for (const auto& lobbyMember : lobbyMembers)
												{
													if (lobbyMember.user_id == chatData.user_id)
													{
														lobbySlot = lobbyMember.m_SlotIndex;
														break;
													}
												}

												// Match local setup notice colors.
												// System announcements stay neutral even when they come from the host;
												// player chat keeps its per-slot color.
												Color color = chatData.announcement
													? DetermineSystemNoticeColor(false, false)
													: DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY, true, chatData.action, false, false, lobbySlot);

												if (pLobbyInterface->m_OnChatCallback != nullptr)
												{
													pLobbyInterface->m_OnChatCallback(unicodeStr, color);
												}
											}
										}
									}
									break;

									case EWebSocketMessageID::NETWORK_ROOM_MEMBER_LIST_UPDATE:
									{
										std::unordered_map<uint64_t, NetworkRoomMember> mapMembers;
										for (const auto& playerEntryIter : jsonObject["members"])
										{
											NetworkRoomMember newMember;
											playerEntryIter["UserID"].get_to(newMember.user_id);
											playerEntryIter["Name"].get_to(newMember.display_name);
											playerEntryIter["IsAdmin"].get_to(newMember.m_bIsAdmin);

											mapMembers.emplace(newMember.user_id, newMember);
										}

										RoomSelectionResult selectionResult;
										if (jsonObject.contains("selected_room_id") && jsonObject["selected_room_id"].is_number_integer())
										{
											selectionResult.selectedRoomID = jsonObject["selected_room_id"].get<int>();
										}
										if (jsonObject.contains("effective_room_id") && jsonObject["effective_room_id"].is_number_integer())
										{
											selectionResult.effectiveRoomID = jsonObject["effective_room_id"].get<int>();
										}
										if (jsonObject.contains("rejected_room_id") && jsonObject["rejected_room_id"].is_number_integer())
										{
											selectionResult.rejectedRoomID = jsonObject["rejected_room_id"].get<int>();
										}
										if (jsonObject.contains("room_selection_error") && jsonObject["room_selection_error"].is_string())
										{
											jsonObject["room_selection_error"].get_to(selectionResult.error);
										}
										if (jsonObject.contains("request_id") && jsonObject["request_id"].is_number_unsigned())
										{
											selectionResult.requestID = jsonObject["request_id"].get<uint64_t>();
										}

                                        NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
                                        if (pRoomsInterface != nullptr)
                                        {
											pRoomsInterface->OnRosterUpdated(std::move(mapMembers), selectionResult);
                                        }
									}
									break;

                                    case EWebSocketMessageID::ANTICHEAT_MESSAGE:
                                    {
                                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] GOT AC MSG FROM WEBSOCKET!");

										WebSocketMessage_AnticheatMessage acMsg;
                                        bool bParsed = JSONGetAsObject(jsonObject, &acMsg);

                                        if (bParsed)
                                        {
                                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AC Msg Signal User: %lld!", acMsg.target_user_id);
                                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AC Msg Signal Payload Size: %d!", (int)acMsg.payload.size());
                                            AnticheatPlugInterface::AC_NetworkMessageArrived(acMsg.target_user_id, acMsg.payload.data(), acMsg.payload.size());
                                        }
                                    }
                                    break;

									case EWebSocketMessageID::LOBBY_CURRENT_LOBBY_UPDATE:
									{
										// re-get the room info as it is stale
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr)
										{
											pLobbyInterface->UpdateRoomDataCache(nullptr);
										}
									}
									break;

									case EWebSocketMessageID::PROBE:
									{
										WebSocketMessage_ServerProbe probe;
                                        bool bParsed = JSONGetAsObject(jsonObject, &probe);

										if (bParsed)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[PROBE] GOT PROBE REQUEST: %s!", probe.url.c_str());

											NGMP_OnlineServicesManager::GetInstance()->CaptureScreenshotForProbe(EScreenshotType::SCREENSHOT_TYPE_GAMEPLAY, probe.url);

											// service needs the response
											nlohmann::json j;
											j["msg_id"] = EWebSocketMessageID::PROBE_RESP;
											j["timestamp"] = "0";
											std::string strBody = j.dump();
											Send(strBody.c_str());
										}
									}
									break;

									case EWebSocketMessageID::NETWORK_ROOM_LOBBY_LIST_UPDATE:
									{
										// re-get the room info as it is stale
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr)
										{
											pLobbyInterface->SetLobbyListDirty();
										}
									}
									break;

									case EWebSocketMessageID::MATCHMAKING_ACTION_JOIN_PREARRANGED_LOBBY:
									{
										WebSocketMessage_MatchmakingAction_JoinPrearrangedLobby mmEvent;
										bool bParsed = JSONGetAsObject(jsonObject, &mmEvent);

										if (bParsed)
										{
											NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
											if (pLobbyInterface != nullptr)
											{
												pLobbyInterface->InvokeMatchmakingMatchFoundCallback();

												// TODO_QUICKMATCH: Only if really in quickmatch

												// TODO_QUICKMATCH: We need to retrieve this info instead
												// basic info needed to join
												LobbyEntry lobbyEntry;
												lobbyEntry.lobbyID = mmEvent.lobby_id;
												lobbyEntry.map_path = "Maps\\Alpine Assault\\Alpine Assault.map";

												pLobbyInterface->JoinLobby(lobbyEntry, std::string());

												pLobbyInterface->InvokeMatchmakingMessageCallback("Joining QuickMatch Lobby");
											}
											else
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[NETWORK_CONNECTION_DISCONNECT_PLAYER] Lobby interface is null");
												break;
											}
										}
									}
									break;

									case EWebSocketMessageID::MATCHMAKING_ACTION_START_GAME:
									{
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr)
										{
											pLobbyInterface->InvokeMatchmakingStartGameCallback();
										}
									}
									break;

									case EWebSocketMessageID::MATCHMAKING_ACTION_REQUEUE:
									{
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr)
										{
											pLobbyInterface->ResetForMatchmakingRequeue();
											pLobbyInterface->InvokeMatchmakingRequeueCallback();
										}
									}
									break;

									case EWebSocketMessageID::MATCHMAKING_ACTION_SETUP_PROGRESS:
									{
										static constexpr int kMinMatchSetupTimeoutMs = 1000;
										static constexpr int kMaxMatchSetupTimeoutMs = 120000;

										int timeoutMs = 0;
										if (jsonObject.contains("timeout_ms") && jsonObject["timeout_ms"].is_number_integer())
										{
											timeoutMs = jsonObject["timeout_ms"].get<int>();
											if (timeoutMs > 0)
											{
												if (timeoutMs < kMinMatchSetupTimeoutMs)
												{
													timeoutMs = kMinMatchSetupTimeoutMs;
												}
												else if (timeoutMs > kMaxMatchSetupTimeoutMs)
												{
													timeoutMs = kMaxMatchSetupTimeoutMs;
												}
											}
										}

										// -1 = older service that doesn't say, the menu infers it
										int countdownMs = -1;
										if (jsonObject.contains("countdown_ms") && jsonObject["countdown_ms"].is_number_integer())
										{
											countdownMs = jsonObject["countdown_ms"].get<int>();
										}

										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr && timeoutMs > 0)
										{
											pLobbyInterface->InvokeMatchmakingSetupProgressCallback(timeoutMs, countdownMs);
										}
									}
									break;

									case EWebSocketMessageID::MATCHMAKING_MESSAGE:
									{
										WebSocketMessage_MatchmakingMessage matchmakingMsg;
										bool bParsed = JSONGetAsObject(jsonObject, &matchmakingMsg);

										if (bParsed)
										{
											NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
											if (pLobbyInterface != nullptr)
											{
												pLobbyInterface->InvokeMatchmakingMessageCallback(matchmakingMsg.message);
											}
										}
									}
									break;

									case EWebSocketMessageID::SOCIAL_NEW_FRIEND_REQUEST:
									{
										WebSocketMessage_Social_NewFriendRequest incomingNotify;
										bool bParsed = JSONGetAsObject(jsonObject, &incomingNotify);

										if (bParsed)
										{
											NGMP_OnlineServices_SocialInterface* pSocialInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
											if (pSocialInterface != nullptr)
											{
												pSocialInterface->InvokeCallback_NewFriendRequest(incomingNotify.display_name);
											}
										}
									}
									break;

									case EWebSocketMessageID::WS_KEEPALIVE:
									{
										std::vector<GOModuleInfo> modules = GetLoadedModules();

										std::vector<std::vector<std::string>> vecResp;

                                        for (auto& m : modules)
										{
											std::vector<std::string> newEntry(2);
											newEntry[0] = m.path;
											newEntry[1] = std::to_string(m.size);
											vecResp.push_back(newEntry);
                                        }

                                        // service needs the response
                                        nlohmann::json j;
                                        j["msg_id"] = EWebSocketMessageID::WS_KEEPALIVE_CLIENT;
                                        j["resp"] = vecResp;
                                        std::string strBody = j.dump();
                                        Send(strBody.c_str());
									}
									break;

									default:
										NetworkLog(ELogVerbosity::LOG_RELEASE, "Unhandled WebSocketMessage: %d", (int)msgID);
										break;
									}
								}
								else
								{
									NetworkLog(ELogVerbosity::LOG_RELEASE, "Malformed WebSocketMessage: couldn't parse as WebSocketMessageBase");
								}
							}
						}
						else
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "Malformed WebSocketMessage");
						}
					}
					catch (nlohmann::json::exception& jsonException)
					{

						NetworkLog(ELogVerbosity::LOG_RELEASE, "Unparsable WebSocketMessage 101: %s (JSON: %s)", bufferThisRecv, jsonException.what());
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Buildup buffer is: %s", m_vecWSPartialBuffer.data());

						m_vecWSPartialBuffer.clear();
					}
					catch (std::exception& e)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Unparsable WebSocketMessage 100: %s (%s)", bufferThisRecv, e.what());

						m_vecWSPartialBuffer.clear();
					}
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Unparsable WebSocketMessage 102: %s", bufferThisRecv);

						m_vecWSPartialBuffer.clear();
					}
				}
			}
			else if (meta->flags & CURLWS_BINARY)
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket binary");
				// noop
			}
			else if (meta->flags & CURLWS_CLOSE)
			{
				// TODO_NGMP: Dont do this during gameplay, they can play without the WS, just 'queue' it for when they get back to the front end

				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket close");
				NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
				m_bConnected = false;
				m_vecWSPartialBuffer.clear();
				// TODO_NGMP: Handle this
			}
			else if (meta->flags & CURLWS_PING)
			{
				// TODO_NGMP: Handle this
			}
			else if (meta->flags & CURLWS_OFFSET)
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket offset");
				// noop
			}
		}
		else
		{
			NetworkLog(ELogVerbosity::LOG_DEBUG, "websocket meta was null");
		}
	}
	else if (ret == CURLE_RECV_ERROR)
	{

		NetworkLog(ELogVerbosity::LOG_RELEASE, "Got websocket disconnect (ERROR: %s), Attempting reconnect", curl_easy_strerror(ret));

		m_bConnected = false;
		BeginReconnect();
		m_vecWSPartialBuffer.clear();


		// send event to sentry
#if defined(GENERALS_ONLINE_USE_SENTRY)
        if (TheNGMPGame != nullptr)
        {
			AsciiString sentryMsg;
            sentryMsg.format("Got websocket disconnect (ERROR: %s), Attempting reconnect", curl_easy_strerror(ret));
            sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "WEBSOCKET_DISCONNECT_ERROR", sentryMsg.str()));
        }
#endif
	}

	// time since last pong?
	if (m_lastPong != -1 && (currTime - m_lastPong) >= m_timeForWSTimeout)
	{
        // send event to sentry
#if defined(GENERALS_ONLINE_USE_SENTRY)
        if (TheNGMPGame != nullptr)
        {
            AsciiString sentryMsg;
            sentryMsg.format("Got websocket disconnect (Timeout: %s), timeout is %lld, last pong was at %lld, current time is %lld, attempting reconnect", curl_easy_strerror(ret), currTime - m_lastPong, m_lastPong, currTime);
            sentry_capture_event(sentry_value_new_message_event(SENTRY_LEVEL_ERROR, "WEBSOCKET_DISCONNECT_TIMEOUT", sentryMsg.str()));
        }
#endif

		NetworkLog(ELogVerbosity::LOG_RELEASE, "Got websocket disconnect (Timeout: %s), timeout is %lld, last pong was at %lld, current time is %lld, attempting reconnect", curl_easy_strerror(ret), currTime - m_lastPong, m_lastPong, currTime);
        m_bConnected = false;
		BeginReconnect();
        m_vecWSPartialBuffer.clear();
	};
}

NGMP_OnlineServices_RoomsInterface::NGMP_OnlineServices_RoomsInterface()
{

}

void NGMP_OnlineServices_RoomsInterface::GetRoomList(std::function<void(bool)> cb)
{
	m_vecRooms.clear();
	m_CurrentRoomIndex = -1;
	m_EffectiveRoomID.reset();
	m_PendingRoomChange.reset();
	m_bRoomSelectionResultsSupported = false;
	m_bSupportsModerationCommands = false;

	// Cache our buddies on lobby list
	NGMP_OnlineServices_SocialInterface* pSocialInterface =
		NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_SocialInterface>();
	if (pSocialInterface != nullptr)
	{
		pSocialInterface->GetFriendsList(false, nullptr);
	}

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Rooms");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			if (!bSuccess || statusCode != 200)
			{
				cb(false);
				return;
			}

			try
			{
				std::vector<NetworkRoom> rooms;
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				const bool roomSelectionResultsSupported = jsonObject.value("supports_room_selection_results", false);
				const bool supportsModerationCommands = jsonObject.value("supports_moderation_commands", false);

				for (const auto& roomEntryIter : jsonObject["rooms"])
				{
					int id = 0;
					std::string strName;
					ERoomFlags flags = ERoomFlags::ROOM_FLAGS_DEFAULT;
					int parentRoomID = -1;

					roomEntryIter["id"].get_to(id);
					roomEntryIter["name"].get_to(strName);
					if (roomEntryIter.contains("flags") && !roomEntryIter["flags"].is_null())
					{
						roomEntryIter["flags"].get_to(flags);
					}
					if (roomEntryIter.contains("parent_id") && !roomEntryIter["parent_id"].is_null())
					{
						roomEntryIter["parent_id"].get_to(parentRoomID);
					}
					rooms.emplace_back(id, strName, flags, parentRoomID);
				}

				m_vecRooms = std::move(rooms);
				m_bRoomSelectionResultsSupported = roomSelectionResultsSupported;
				m_bSupportsModerationCommands = supportsModerationCommands;

				cb(true);
				return;
			}
			catch (const std::exception& exception)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to parse room list: %s", exception.what());
			}

			cb(false);
			return;
		});
}

void NGMP_OnlineServices_RoomsInterface::JoinRoom(int roomIndex)
{
	const std::vector<NetworkRoom>& rooms = GetGroupRooms();
	if (roomIndex < 0 || roomIndex >= (int)rooms.size())
	{
		ReportRoomJoinFailure(std::format("Invalid room index {}.", roomIndex));
		return;
	}

	std::shared_ptr<WebSocket> pWS = NGMP_OnlineServicesManager::GetWebSocket();
	if (pWS == nullptr)
	{
		ReportRoomJoinFailure("The room service is not connected.");
		return;
	}

	if (m_PendingRoomChange.has_value())
	{
		ReportRoomJoinFailure("Another room change is already in progress.");
		return;
	}

	const uint64_t requestID = m_NextRoomChangeRequestID++;
	m_PendingRoomChange = PendingRoomChange{
		roomIndex,
		rooms[roomIndex].GetRoomID(),
		requestID,
		std::chrono::steady_clock::now() + std::chrono::seconds(10)
	};
	pWS->SendData_JoinNetworkRoom(rooms[roomIndex].GetRoomID(), requestID);
}

std::unordered_map<uint64_t, NetworkRoomMember>& NGMP_OnlineServices_RoomsInterface::GetMembersListForCurrentRoom()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Repopulating network room roster using local data");
	return m_mapMembers;
}

void NGMP_OnlineServices_RoomsInterface::SendChatMessageToCurrentRoom(UnicodeString& strChatMsgUnicode, bool bIsAction)
{
	std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
	if (pWS != nullptr)
	{
		pWS->SendData_RoomChatMessage(strChatMsgUnicode, bIsAction);
	}
}

void NGMP_OnlineServices_RoomsInterface::OnRosterUpdated(std::unordered_map<uint64_t, NetworkRoomMember> mapMembers,
	const RoomSelectionResult& selectionResult)
{
	m_mapMembers = std::move(mapMembers);

	int changedRoomIndex = -1;
	bool effectiveRoomChanged = true;
	bool refreshRoster = true;
	std::string roomJoinFailure;
	if (m_PendingRoomChange.has_value())
	{
		const PendingRoomChange& pendingRoomChange = *m_PendingRoomChange;
		const bool requestMatches = !selectionResult.requestID.has_value()
			|| pendingRoomChange.requestID == *selectionResult.requestID;
		if (requestMatches && selectionResult.rejectedRoomID == pendingRoomChange.roomID)
		{
			roomJoinFailure = selectionResult.error.empty() ? "The room selection was rejected." : selectionResult.error;
			m_PendingRoomChange.reset();
		}
		else
		{
			const bool selectionMatches = requestMatches
				&& (selectionResult.selectedRoomID.has_value()
					? pendingRoomChange.roomID == *selectionResult.selectedRoomID
					: !m_bRoomSelectionResultsSupported);
			if (selectionMatches)
			{
				if (selectionResult.effectiveRoomID.has_value())
				{
					effectiveRoomChanged = !m_EffectiveRoomID.has_value()
						|| *m_EffectiveRoomID != *selectionResult.effectiveRoomID;
					m_EffectiveRoomID = selectionResult.effectiveRoomID;
					refreshRoster = effectiveRoomChanged;
				}
				m_CurrentRoomIndex = pendingRoomChange.roomIndex;
				changedRoomIndex = m_CurrentRoomIndex;
				m_PendingRoomChange.reset();
			}
		}
	}

	if (changedRoomIndex >= 0 && m_RoomChangedCallback != nullptr)
	{
		m_RoomChangedCallback(changedRoomIndex, effectiveRoomChanged);
	}
	if (!roomJoinFailure.empty())
	{
		ReportRoomJoinFailure(roomJoinFailure);
	}

	std::scoped_lock<std::mutex> lock(m_rosterCallbackMutex);
	if (refreshRoster && m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}
}

void NGMP_OnlineServices_RoomsInterface::Tick()
{
	if (m_PendingRoomChange.has_value()
		&& std::chrono::steady_clock::now() >= m_PendingRoomChange->deadline)
	{
		m_PendingRoomChange.reset();
		ReportRoomJoinFailure("The room change timed out. Please try again.");
	}
}

void NGMP_OnlineServices_RoomsInterface::ReportRoomJoinFailure(const std::string& error)
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Room change failed: %s", error.c_str());
	if (m_OnChatCallback != nullptr)
	{
		UnicodeString message;
		message = L"Couldn't join that room. Please try again.";
		m_OnChatCallback(message, GameMakeColor(255, 0, 0, 255));
	}
}



