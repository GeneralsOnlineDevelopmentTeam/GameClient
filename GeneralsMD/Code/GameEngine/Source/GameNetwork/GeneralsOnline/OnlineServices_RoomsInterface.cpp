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
	CURLcode result =
		curl_ws_send(m_pCurl, "wsping", strlen("wsping"), &sent, 0,
			CURLWS_PING);
	return (int)result;
}


void WebSocket::Connect(const char* url)
{
	if (m_bConnected)
	{
		return;
	}

	if (m_pCurl != nullptr)
	{
		curl_easy_setopt(m_pCurl, CURLOPT_URL, url);

		curl_easy_setopt(m_pCurl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */

#if _DEBUG
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYHOST, 0);
#else
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYPEER, 0);
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYHOST, 0);
#endif

		// ws needs auth
		struct curl_slist* headers = nullptr;
		char szHeaderBuffer[8192] = { 0 };
		sprintf_s(szHeaderBuffer, "Authorization: Bearer %s", NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken().c_str());
		headers = curl_slist_append(headers, szHeaderBuffer);

		curl_easy_setopt(m_pCurl, CURLOPT_HTTPHEADER, headers);

		//curl_easy_setopt(m_pCurl, CURLOPT_TIMEOUT_MS, 1000);

		/* Perform the request, res gets the return code */
		CURLcode res = curl_easy_perform(m_pCurl);
		/* Check for errors */
		if (res != CURLE_OK)
		{
			m_bConnected = false;
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Failed to connect");
		}
		else
		{
			/* connected and ready */
			m_bConnected = true;

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[WebSocket] Connected");
		}
	}
}

void WebSocket::SendData_RoomChatMessage(const char* szMessage, bool bIsAction)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_CLIENT;
	j["message"] = szMessage;
	j["action"] = bIsAction;
	std::string strBody = j.dump();

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
}

void WebSocket::Send(const char* send_payload)
{
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
	std::string signal;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_NetworkSignal, signal)
};

class WebSocketMessage_LobbyChatIncoming : public WebSocketMessageBase
{
public:
	std::string message;
	bool action;
	bool announcement;
	bool show_announcement_to_host;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_LobbyChatIncoming, msg_id, message, action, announcement, show_announcement_to_host)
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

	WebSocket* pWS = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket();
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
	char buffer[8196 * 4] = { 0 };

	CURLcode ret = CURL_LAST;
	ret = curl_ws_recv(m_pCurl, buffer, sizeof(buffer) - 1, &rlen, &meta);
	buffer[rlen] = 0; // Ensure null-termination
	
	if (ret != CURLE_RECV_ERROR && ret != CURL_LAST && ret != CURLE_AGAIN && ret != CURLE_GOT_NOTHING)
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket msg: %s", buffer);
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
				NetworkLog(ELogVerbosity::LOG_DEBUG, "websocket buffer is: %s", buffer);

				if (meta->flags & CURLWS_CONT)
				{
					strBuf.append(buffer, rlen);
					NetworkLog(ELogVerbosity::LOG_DEBUG, "WEBSOCKET PARTIAL!");
				}
				else
				{
					try
					{
						nlohmann::json jsonObject;
						if (!strBuf.empty())
						{
							strBuf.append(buffer, rlen);
							jsonObject = nlohmann::json::parse(strBuf);

							strBuf.clear();
						}
						else
						{
							jsonObject = nlohmann::json::parse(buffer);
						}

						if (jsonObject.contains("msg_id"))
						{
							WebSocketMessageBase msgDetails = jsonObject.get<WebSocketMessageBase>();
							EWebSocketMessageID msgID = msgDetails.msg_id;

							switch (msgID)
							{
							case EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_SERVER:
							{
								WebSocketMessage_RoomChatIncoming chatData = jsonObject.get<WebSocketMessage_RoomChatIncoming>();

								UnicodeString strChatMsg;
								strChatMsg.format(L"%hs", chatData.message.c_str());

								GameSpyColors color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_NETWORK_ROOM, true, chatData.action);

								if (NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_OnChatCallback != nullptr)
								{
									NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_OnChatCallback(strChatMsg, color);
								}
							}
							break;

							case EWebSocketMessageID::START_GAME:
							{
								if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket != nullptr)
								{
									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackStartGamePacket();
								}
							}
							break;

							case EWebSocketMessageID::NETWORK_SIGNAL:
							{
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] GOT SIGNAL!");
								WebSocketMessage_NetworkSignal signalData = jsonObject.get<WebSocketMessage_NetworkSignal>();

								m_pendingSignals.push(signalData.signal);
							}
							break;

							case EWebSocketMessageID::LOBBY_CHAT_FROM_SERVER:
							{
								WebSocketMessage_LobbyChatIncoming chatData = jsonObject.get<WebSocketMessage_LobbyChatIncoming>();

								GameSpyColors color = DetermineColorForChatMessage(EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY, true, chatData.action);

								UnicodeString str;
								str.format(L"%hs", chatData.message.c_str());

								if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback != nullptr)
								{
									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_OnChatCallback(str, color);
								}
							}
							break;

							// TODO_STEAM: remove relay upgrade path from everything
							case EWebSocketMessageID::PLAYER_CONNECTION_RELAY_UPGRADE:
							{
								WebSocketMessage_RelayUpgrade relayUpgrade = jsonObject.get<WebSocketMessage_RelayUpgrade>();
								NetworkLog(ELogVerbosity::LOG_RELEASE, "Got relay upgrade for user %lld", relayUpgrade.target_user_id);
								NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
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
								NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->OnRosterUpdated(memberList.names, memberList.ids);
							}
							break;

							case EWebSocketMessageID::LOBBY_CURRENT_LOBBY_UPDATE:
							{
								// re-get the room info as it is stale
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateRoomDataCache(nullptr);
							}
							break;

							case EWebSocketMessageID::NETWORK_ROOM_LOBBY_LIST_UPDATE:
							{
								// re-get the room info as it is stale
								NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SetLobbyListDirty();
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
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Unparsable WebSocketMessage: %s", buffer);
					}
				}
			}
			else if (meta->flags & CURLWS_BINARY)
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket binary");
				// noop
			}
			else if (meta->flags & CURLWS_CONT)
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket cont");
				// noop
			}
			else if (meta->flags & CURLWS_CLOSE)
			{
				// TODO_NGMP: Dont do this during gameplay, they can play without the WS, just 'queue' it for when they get back to the front end

				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got websocket close");
				NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
				m_bConnected = false;
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

		NetworkLog(ELogVerbosity::LOG_RELEASE, "Got websocket disconnect");
		NGMP_OnlineServicesManager::GetInstance()->SetPendingFullTeardown(EGOTearDownReason::LOST_CONNECTION);
		m_bConnected = false;
	}

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
	if (!NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().empty())
	{
		// if the room doesnt exist, try the first room
		if (roomIndex < 0 || roomIndex >= NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().size())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Invalid room index %d, using first room", roomIndex);
			roomIndex = 0;
		}

		NetworkRoom targetNetworkRoom = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->GetGroupRooms().at(roomIndex);
		NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_JoinNetworkRoom(targetNetworkRoom.GetRoomID());
	}
	
	onCompleteCallback();
}

std::map<uint64_t, NetworkRoomMember>& NGMP_OnlineServices_RoomsInterface::GetMembersListForCurrentRoom()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Refreshing network room roster");
	return m_mapMembers;
}

void NGMP_OnlineServices_RoomsInterface::SendChatMessageToCurrentRoom(UnicodeString& strChatMsgUnicode, bool bIsAction)
{
	// TODO_NGMP: Support unicode again
	AsciiString strChatMsg;
	strChatMsg.translate(strChatMsgUnicode);

	NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_RoomChatMessage(strChatMsg.str(), bIsAction);
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