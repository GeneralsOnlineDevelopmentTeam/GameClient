#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"
#include "../../GameSpy/PeerDefs.h"


WebSocket::WebSocket()
{
	m_pCurl = curl_easy_init();
}

WebSocket::~WebSocket()
{
	Shutdown();
}

int WebSocket::Ping()
{
	size_t sent;
	CURLcode result = curl_ws_send(m_pCurl, "wsping", strlen("wsping"), &sent, 0,
			CURLWS_PING);

	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::PING;
	std::string strBody = j.dump();

	Send(strBody.c_str());

	return (int)result;
}


void WebSocket::Connect(const char* url)
{
	if (m_bConnected)
	{
		return;
	}

	m_lastPong = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	if (m_pCurl != nullptr)
	{
		curl_easy_setopt(m_pCurl, CURLOPT_URL, url);

		curl_easy_setopt(m_pCurl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */

#if _DEBUG
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYHOST, 0);

		curl_easy_setopt(m_pCurl, CURLOPT_VERBOSE, 1L);
#else
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYHOST, 0);
#endif

		// ws needs auth
		NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		if (pAuthInterface == nullptr)
		{
			return;
		}

		struct curl_slist* headers = nullptr;
		char szHeaderBuffer[8192] = { 0 };
		sprintf_s(szHeaderBuffer, "Authorization: Bearer %s", pAuthInterface->GetAuthToken().c_str());
		headers = curl_slist_append(headers, szHeaderBuffer);

		curl_easy_setopt(m_pCurl, CURLOPT_HTTPHEADER, headers);

		//curl_easy_setopt(m_pCurl, CURLOPT_TIMEOUT_MS, 1000);

		/* Perform the request, res gets the return code */
		CURLcode res = curl_easy_perform(m_pCurl);
		/* Check for errors */
		if (res != CURLE_OK)
		{
			m_bConnected = false;
			m_vecWSPartialBuffer.clear();
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Failed to connect");
		}
		else
		{
			/* connected and ready */
			m_bConnected = true;
			m_vecWSPartialBuffer.clear();

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Connected");
		}
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

void WebSocket::SendData_ConnectionRelayUpgrade(int64_t userID)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::PLAYER_CONNECTION_RELAY_UPGRADE;
	j["target_user_id"] = userID;
	
	std::string strBody = j.dump();

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

	if (m_pCurl != nullptr)
	{
		// send close
		size_t sent;
		(void)curl_ws_send(m_pCurl, "", 0, &sent, 0, CURLWS_CLOSE);

		// cleanup
		curl_easy_cleanup(m_pCurl);
		m_pCurl = nullptr;
	}

	m_vecWSPartialBuffer.clear();
}

void WebSocket::Send(const char* send_payload)
{
	std::scoped_lock<std::recursive_mutex> lock(m_mutex);

	if (!m_bConnected)
	{
		return;
	}

	size_t sent;
	CURLcode result = curl_ws_send(m_pCurl, send_payload, strlen(send_payload), &sent, 0,
			CURLWS_BINARY);

	if (result != CURLE_OK)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "curl_ws_send() failed: %s\n", curl_easy_strerror(result));
	}
}

class WebSocketMessageBase
{
public:
	EWebSocketMessageID msg_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessageBase, msg_id)
};

class WebSocketMessage_RoomChatIncoming : public WebSocketMessageBase
{
public:
	std::string message;
	bool action;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_RoomChatIncoming, msg_id, message, action)
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

class WebSocketMessage_RelayUpgrade : public WebSocketMessageBase
{
public:
	int64_t target_user_id = -1;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_RelayUpgrade, msg_id, target_user_id)
};

class WebSocketMessage_NetworkRoomMemberListUpdate : public WebSocketMessageBase
{
public:
	std::vector<std::string> names;
	std::vector<int64_t> ids;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkRoomMemberListUpdate, names, ids)
};

//static std::string strSignal = "str:1 ";
void WebSocket::Tick()
{
	std::scoped_lock<std::recursive_mutex> lock(m_mutex);

	if (!m_bConnected)
	{
		return;
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

	// do recv
	size_t rlen = 0;
	const struct curl_ws_frame* meta = nullptr;
	char bufferThisRecv[8196 * 4] = { 0 };

	CURLcode ret = CURL_LAST;
	ret = curl_ws_recv(m_pCurl, bufferThisRecv, sizeof(bufferThisRecv) - 1, &rlen, &meta);
	bufferThisRecv[rlen] = 0; // Ensure null-termination
	
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
				NetworkLog(ELogVerbosity::LOG_DEBUG, "websocket recv buffer is: %s", bufferThisRecv);

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
						// process it
						nlohmann::json jsonObject = nlohmann::json::parse(m_vecWSPartialBuffer);

						// clear buffer and resize
						m_vecWSPartialBuffer.clear();
						m_vecWSPartialBuffer.resize(0);


						if (jsonObject.contains("msg_id"))
						{
							WebSocketMessageBase msgDetails = jsonObject.get<WebSocketMessageBase>();
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
								WebSocketMessage_RoomChatIncoming chatData = jsonObject.get<WebSocketMessage_RoomChatIncoming>();

								UnicodeString unicodeStr(from_utf8(chatData.message).c_str());

								Color color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_NETWORK_ROOM, true, chatData.action);

								NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
								if (pRoomsInterface != nullptr && pRoomsInterface->m_OnChatCallback != nullptr)
								{
									pRoomsInterface->m_OnChatCallback(unicodeStr, color);
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

							case EWebSocketMessageID::NETWORK_SIGNAL:
							{
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] GOT SIGNAL!");
								WebSocketMessage_NetworkSignal signalData = jsonObject.get<WebSocketMessage_NetworkSignal>();

								NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] Signal User: %lld!", signalData.target_user_id);
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] Signal Payload Size: %d!", (int)signalData.payload.size());
								m_pendingSignals.push(signalData.payload);
							}
							break;

							case EWebSocketMessageID::LOBBY_CHAT_FROM_SERVER:
							{
								WebSocketMessage_LobbyChatIncoming chatData = jsonObject.get<WebSocketMessage_LobbyChatIncoming>();

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

									Color color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY, true, chatData.action, lobbySlot);

									if (pLobbyInterface->m_OnChatCallback != nullptr)
									{
										pLobbyInterface->m_OnChatCallback(unicodeStr, color);
									}
								}
							}
							break;

							// TODO_STEAM: remove relay upgrade path from everything
							case EWebSocketMessageID::PLAYER_CONNECTION_RELAY_UPGRADE:
							{
								WebSocketMessage_RelayUpgrade relayUpgrade = jsonObject.get<WebSocketMessage_RelayUpgrade>();
								NetworkLog(ELogVerbosity::LOG_RELEASE, "Got relay upgrade for user %lld", relayUpgrade.target_user_id);
								NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
								if (pMesh != nullptr)
								{
									// TODO_STEAM
									//pMesh->OnRelayUpgrade(relayUpgrade.target_user_id);
								}
							}
							break;

							case EWebSocketMessageID::NETWORK_ROOM_MEMBER_LIST_UPDATE:
							{
								WebSocketMessage_NetworkRoomMemberListUpdate memberList = jsonObject.get<WebSocketMessage_NetworkRoomMemberListUpdate>();
								NGMP_OnlineServices_RoomsInterface* pRoomsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_RoomsInterface>();
								if (pRoomsInterface != nullptr)
								{
									pRoomsInterface->OnRosterUpdated(memberList.names, memberList.ids);
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

							default:
								NetworkLog(ELogVerbosity::LOG_RELEASE, "Unhandled WebSocketMessage: %d", (int)msgID);
								break;
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
		// TODO_NGMP: Dont do this during gameplay, they can play without the WS, just 'queue' it for when they get back to the front end

		NetworkLog(ELogVerbosity::LOG_RELEASE, "Got websocket disconnect (ERROR)");
		NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
		m_bConnected = false;
		m_vecWSPartialBuffer.clear();
	}

	// time since last pong?
	if ((currTime - m_lastPong) >= m_timeForWSTimeout)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Got websocket disconnect (Timeout)");
		NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
		m_bConnected = false;
		m_vecWSPartialBuffer.clear();
	};

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

			WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();;
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
	WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();;
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