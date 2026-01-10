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

	// TODO_CACHE: Cleanup multi too
	if (m_pCurlWS != nullptr)
	{
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


void WebSocket::SendData_JoinNetworkRoom(int roomID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_CHANGE_ROOM;
	j["room"] = roomID;
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
		// send close
		size_t sent;
		(void)curl_ws_send(m_pCurlWS, "", 0, &sent, 0, CURLWS_CLOSE);

		// release headers
		if (m_pHeaders != nullptr)
		{
			curl_slist_free_all(m_pHeaders);
			m_pHeaders = nullptr;
		}

		// cleanup
		curl_easy_cleanup(m_pCurlWS);
		m_pCurlWS = nullptr;
	}

	m_vecWSPartialBuffer.clear();
}

void WebSocket::Send(const char* send_payload)
{
	if (!AcquireLock())
	{
		return;
	}

	if (!m_bConnected)
	{
		// just queue it instead
		m_vecQueuedOutboungMsgs.push_back(std::string(send_payload));

		ReleaseLock();
		return;
	}

	size_t sent;
	CURLcode result = curl_ws_send(m_pCurlWS, send_payload, strlen(send_payload), &sent, 0, CURLWS_BINARY);

	if (result != CURLE_OK)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "curl_ws_send() failed: %s\n", curl_easy_strerror(result));
	}

	ReleaseLock();
}

class WebSocketMessageBase
{
public:
	EWebSocketMessageID msg_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessageBase, msg_id)
};

class WebSocketMessage_NetworkStartSignalling : public WebSocketMessageBase
{
public:
	int64_t lobby_id;
	int64_t user_id;
	uint16_t preferred_port;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkStartSignalling, msg_id, lobby_id, user_id, preferred_port)
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

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_RoomChatIncoming, msg_id, message, action, admin)
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

class WebSocketMessage_NetworkRoomMemberListUpdate : public WebSocketMessageBase
{
public:
	std::vector<std::string> names;
	std::vector<int64_t> ids;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkRoomMemberListUpdate, names, ids)
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
void WebSocket::Tick()
{
    if (!AcquireLock())
    {
        return;
    }

	// attempting to reconnect?
	if (m_bReconnecting)
	{
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

		int maxReconnectAttempts = (TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress()) ? maxReconnectAttempts_Ingame : maxReconnectAttempts_Frontend;
		if (m_numReconnectAttempts >= maxReconnectAttempts)
		{
			// fully disconnect
            NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (reconnect 1)");
            NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
            m_bConnected = false;
            m_vecWSPartialBuffer.clear();

            // clear reconnection flags
            m_bReconnecting = false;
            m_numReconnectAttempts = 0;
            m_lastReconnectAttempt = -1;
		}
		else
		{
			int timeBetweenReconnectAttempts = (TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress()) ? timeBetweenReconnectAttempts_Ingame : timeBetweenReconnectAttempts_Frontend;

            if (currTime - m_lastReconnectAttempt >= timeBetweenReconnectAttempts)
            {
                m_lastReconnectAttempt = currTime;
                ++m_numReconnectAttempts;

				Connect(m_strWebsocketAddr.c_str(), true, nullptr);
            }
		}
	}



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
	int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
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

                        // reconnecting? give up eventually
                        if (m_bReconnecting)
                        {
                            int maxReconnectAttempts = (TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress()) ? maxReconnectAttempts_Ingame : maxReconnectAttempts_Frontend;

                            if (m_numReconnectAttempts >= maxReconnectAttempts || (m->data.result == CURLE_HTTP_RETURNED_ERROR && httpResponseCode == 205)) // 205 = need full teardown
                            {
                                if (httpResponseCode == 205)
                                {
                                    NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (reconnect 205)");
                                }
                                else
                                {
                                    NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (reconnect 2)");
                                }

                                NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
                                m_bConnected = false;
                                m_vecWSPartialBuffer.clear();

                                // clear reconnection flags
                                m_bReconnecting = false;
                                m_numReconnectAttempts = 0;
                                m_lastReconnectAttempt = -1;
                            }
                        }
                        else // give up immediately
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "Going to teardown (initial connect)");
                            NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
                            m_bConnected = false;
                            m_vecWSPartialBuffer.clear();

                            // clear reconnection flags
                            m_bReconnecting = false;
                            m_numReconnectAttempts = 0;
                            m_lastReconnectAttempt = -1;
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

                        // clear reconnection flags
                        m_bReconnecting = false;
                        m_numReconnectAttempts = 0;
                        m_lastReconnectAttempt = -1;

                        // connecting is as good as a pong
                        m_lastPong = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
                    }

                    if (m_fnWebsocketConnectedCallback != nullptr)
                    {
                        m_fnWebsocketConnectedCallback();
                    }
                }
            }
        }
    }

    if (!m_bConnected)
    {
        ReleaseLock();
        return;
    }

	// send anything we have buffered (e.g. things that were queued while not connected)
	for (std::string& strPayload : m_vecQueuedOutboungMsgs)
	{
        size_t sent;
        CURLcode result = curl_ws_send(m_pCurlWS, strPayload.c_str(), strPayload.length(), &sent, 0, CURLWS_BINARY);

        if (result != CURLE_OK)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "curl_ws_send() failed: %s\n", curl_easy_strerror(result));
        }
	}
	m_vecQueuedOutboungMsgs.clear();

	// do recv
	size_t rlen = 0;
	const struct curl_ws_frame* meta = nullptr;
	char bufferThisRecv[8196 * 4] = { 0 };

	CURLcode ret = CURL_LAST;
	ret = curl_ws_recv(m_pCurlWS, bufferThisRecv, sizeof(bufferThisRecv), &rlen, &meta);

	if (ret != CURLE_RECV_ERROR && ret != CURL_LAST && ret != CURLE_AGAIN && ret != CURLE_GOT_NOTHING)
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket msg: %s", bufferThisRecv);
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket len: %d", rlen);
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket flags: %d", meta->flags);

		// what type of message?
		if (meta != nullptr)
		{
			if (meta->flags & CURLWS_PONG) // PONG
			{

			}
			else if (meta->flags & CURLWS_TEXT)
			{
				bool bMessageComplete = false;

				m_vecWSPartialBuffer.resize(m_vecWSPartialBuffer.size() + rlen);
				memcpy_s(m_vecWSPartialBuffer.data() + m_vecWSPartialBuffer.size() - rlen, rlen, bufferThisRecv, rlen);

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

									case EWebSocketMessageID::PONG:
									{
										int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
										m_lastPong = currTime;
									}
									break;

									case EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_SERVER:
									{
										WebSocketMessage_RoomChatIncoming chatData;
										bool bParsed = JSONGetAsObject(jsonObject, &chatData);

										if (bParsed)
										{
											UnicodeString unicodeStr(from_utf8(chatData.message).c_str());

											Color color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_NETWORK_ROOM, true, chatData.action, chatData.admin);

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
										NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
										if (pLobbyInterface != nullptr && pLobbyInterface->m_callbackStartGamePacket != nullptr)
										{
											pLobbyInterface->m_callbackStartGamePacket();
										}
									}
									break;

									case EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE:
									{
										// respond with our state
										std::vector<int64_t> connectivityMap;
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
											}
										}

										// send response
										nlohmann::json j;
										j["msg_id"] = EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE;
										j["connectivity_map"] = connectivityMap;
										std::string strBody = j.dump();

										Send(strBody.c_str());
										break;
									}

									case EWebSocketMessageID::FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST:
									{
										// all checks are done, process start for host

										bool bMeshComplete = false;

										try
										{
											jsonObject["mesh_complete"].get_to(bMeshComplete);

											std::list<std::pair<int64_t, int64_t>> missingConnections;
											if (!bMeshComplete)
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Mesh is not complete for someone");
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

											// invoke callback
											if (m_cbOnConnectivityCheckComplete != nullptr)
											{
												m_cbOnConnectivityCheckComplete(bMeshComplete, missingConnections);
											}

											m_cbOnConnectivityCheckComplete = NULL;
										}
										catch (...)
										{
											NetworkLog(ELogVerbosity::LOG_RELEASE, "[FULL_MESH_CONNECTIVITY_CHECK_RESPONSE_COMPLETE_TO_HOST] Error processing response");
											break;
										}

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
													pMesh->StartConnectionSignalling(startSignallingData.user_id, startSignallingData.preferred_port);
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
											// Validate payload is not empty to prevent crashes in OpenSSL CMP validation
											if (signalData.payload.size() > 0)
											{
												m_pendingSignals.push(signalData.payload);
											}
											else
											{
												NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] Warning: Ignoring empty signal payload from user %lld", signalData.target_user_id);
											}
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

												// no admin chat in lobby
												Color color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY, true, chatData.action, false, lobbySlot);

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
										WebSocketMessage_NetworkRoomMemberListUpdate memberList;
										bool bParsed = JSONGetAsObject(jsonObject, &memberList);

										if (bParsed)
										{
											NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
											if (pRoomsInterface != nullptr)
											{
												pRoomsInterface->OnRosterUpdated(memberList.names, memberList.ids);
											}
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
										NetworkLog(ELogVerbosity::LOG_RELEASE, "[PROBE] GOT PROBE REQUEST!");

										NGMP_OnlineServicesManager::GetInstance()->CaptureScreenshotForProbe(EScreenshotType::SCREENSHOT_TYPE_GAMEPLAY);

										// service needs the response
                                        nlohmann::json j;
                                        j["msg_id"] = EWebSocketMessageID::PROBE_RESP;
										j["timestamp"] = "0";
                                        std::string strBody = j.dump();
                                        Send(strBody.c_str());
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
		m_bReconnecting = true;
        m_numReconnectAttempts = 0;
        m_lastReconnectAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
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
        m_bReconnecting = true;
        m_numReconnectAttempts = 0;
        m_lastReconnectAttempt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
        m_vecWSPartialBuffer.clear();
	};

	ReleaseLock();
}

NGMP_OnlineServices_RoomsInterface::NGMP_OnlineServices_RoomsInterface()
{

}

void NGMP_OnlineServices_RoomsInterface::GetRoomList(std::function<void(void)> cb)
{
	m_vecRooms.clear();

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Rooms");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);

				for (const auto& roomEntryIter : jsonObject["rooms"])
				{
					int id = 0;
					std::string strName;
					ERoomFlags flags;


					roomEntryIter["id"].get_to(id);
					roomEntryIter["name"].get_to(strName);
					roomEntryIter["flags"].get_to(flags);
					NetworkRoom roomEntry(id, strName, flags);

					m_vecRooms.push_back(roomEntry);
				}

				cb();
				return;
			}
			catch (...)
			{

			}

			// TODO_NGMP: Error handling
			cb();
			return;
		});
}

void NGMP_OnlineServices_RoomsInterface::JoinRoom(int roomIndex, std::function<void()> onStartCallback, std::function<void()> onCompleteCallback)
{
	// TODO_NGMP: Safety

	// TODO_NGMP: Remove this, its no longer a call really, or make a call
	onStartCallback();
	m_CurrentRoomID = roomIndex;

	// TODO_NGMP: What if there are zero rooms? e.g. the service request failed
	NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
	if (pRoomsInterface != nullptr)
	{
		if (!pRoomsInterface->GetGroupRooms().empty())
		{
			// if the room doesnt exist, try the first room
			if (roomIndex < 0 || roomIndex >= pRoomsInterface->GetGroupRooms().size())
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Invalid room index %d, using first room", roomIndex);
				roomIndex = 0;
			}

			NetworkRoom targetNetworkRoom = pRoomsInterface->GetGroupRooms().at(roomIndex);

			std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
			if (pWS != nullptr)
			{
				pWS->SendData_JoinNetworkRoom(targetNetworkRoom.GetRoomID());
			}
		}
	}

	onCompleteCallback();
}

std::map<uint64_t, NetworkRoomMember>& NGMP_OnlineServices_RoomsInterface::GetMembersListForCurrentRoom()
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

void NGMP_OnlineServices_RoomsInterface::OnRosterUpdated(std::vector<std::string> vecNames, std::vector<int64_t> vecIDs)
{
	m_mapMembers.clear();

	int index = 0;
	for (std::string strDisplayName : vecNames)
	{
		int64_t id = vecIDs.at(index);

		NetworkRoomMember newMember;
		newMember.display_name = strDisplayName;
		newMember.user_id = id;
		m_mapMembers.emplace(id, newMember);

		++index;
	}

	if (m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}
}
