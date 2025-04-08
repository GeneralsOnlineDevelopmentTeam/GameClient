#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

WebSocket::WebSocket()
{
	m_pCurl = curl_easy_init();
}

WebSocket::~WebSocket()
{
	Disconnect();
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

		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYPEER, 0); /* websocket style */
		curl_easy_setopt(m_pCurl, CURLOPT_SSL_VERIFYHOST, 0); /* websocket style */

		curl_easy_setopt(m_pCurl, CURLOPT_TIMEOUT_MS, 1000);

		/* Perform the request, res gets the return code */
		CURLcode res = curl_easy_perform(m_pCurl);
		/* Check for errors */
		if (res != CURLE_OK)
		{
			m_bConnected = false;
			fprintf(stderr, "curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));

			NetworkLog("[WebSocket] Failed to connect");
		}
		else
		{
			/* connected and ready */
			m_bConnected = true;

			NetworkLog("[WebSocket] Connected");
		}
	}
}

void WebSocket::SendData_RoomChatMessage(const char* szMessage)
{
	nlohmann::json j;
	j["msg_id"] = EWebSocketMessageID::NETWORK_ROOM_CHAT_FROM_CLIENT;
	j["message"] = szMessage;
	std::string strBody = j.dump();

	Send(strBody.c_str());
}

void WebSocket::Disconnect()
{
	if (!m_bConnected)
	{
		return;
	}

	// send close
	size_t sent;
	(void)curl_ws_send(m_pCurl, "", 0, &sent, 0, CURLWS_CLOSE);

	// cleanup
	curl_easy_cleanup(m_pCurl);
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
		fprintf(stderr, "curl_ws_send() failed: %s\n",
			curl_easy_strerror(result));
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

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(WebSocketMessage_RoomChatIncoming, msg_id, message)
};

void WebSocket::Tick()
{
	if (!m_bConnected)
	{
		return;
	}

	// do recv
	size_t rlen = 0;
	const struct curl_ws_frame* meta = nullptr;
	char buffer[256] = { 0 };

	CURLcode ret = CURL_LAST;
	ret = curl_ws_recv(m_pCurl, buffer, sizeof(buffer), &rlen, &meta);

	if (ret != CURL_LAST && ret != CURLE_AGAIN)
	{
		NetworkLog("Got websocket msg: %s", buffer);

		try
		{

			nlohmann::json jsonObject = nlohmann::json::parse(buffer);

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

					NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_OnChatCallback(strChatMsg);
				}
				break;
				default:
					break;
				}
			}
		}
		catch (...)
		{

		}
	}

}

NGMP_OnlineServices_RoomsInterface::NGMP_OnlineServices_RoomsInterface()
{
	
	/*
	// Register for EOS callbacks, we will handle them internally and pass them onto the game as necessary
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	// player joined/left etc
	EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions lobbyStatusRecievedOpts;
	lobbyStatusRecievedOpts.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
	EOS_NotificationId notID1 = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(LobbyHandle, &lobbyStatusRecievedOpts, nullptr, [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* Data)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->UpdateRoomDataCache();
		});

	// player data changed
	EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions lobbyMemberUpdateRecievedOpts;
	lobbyMemberUpdateRecievedOpts.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;
	EOS_NotificationId notID2 = EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(LobbyHandle, &lobbyMemberUpdateRecievedOpts, nullptr, [](const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* Data)
		{
			NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->UpdateRoomDataCache();
		});

	notID1 = notID1;
	notID2 = notID2;
	*/
}

void NGMP_OnlineServices_RoomsInterface::UpdateRoomDataCache()
{
	/*
	// process users
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());
	NetworkRoom targetNetworkRoom = NGMP_OnlineServicesManager::GetInstance()->GetGroupRooms().at(m_CurrentRoomID);

	// get a handle to our lobby
	EOS_Lobby_CopyLobbyDetailsHandleOptions opts;
	opts.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
	opts.LobbyId = targetNetworkRoom.GetRoomInternalName().str();
	opts.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();

	EOS_HLobbyDetails LobbyInstHandle;
	EOS_EResult getLobbyHandlResult = EOS_Lobby_CopyLobbyDetailsHandle(LobbyHandle, &opts, &LobbyInstHandle);

	if (getLobbyHandlResult == EOS_EResult::EOS_Success)
	{
		// get each member
		EOS_LobbyDetails_GetMemberCountOptions optsMemberCount;
		optsMemberCount.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;

		uint32_t numMembers = EOS_LobbyDetails_GetMemberCount(LobbyInstHandle, &optsMemberCount);
		NetworkLog("[NGMP] Network room has %d members", numMembers);

		for (uint32_t memberIndex = 0; memberIndex < numMembers; ++memberIndex)
		{
			EOS_LobbyDetails_GetMemberByIndexOptions opts4;
			opts4.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
			opts4.MemberIndex = memberIndex;
			EOS_ProductUserId lobbyMember = EOS_LobbyDetails_GetMemberByIndex(LobbyInstHandle, &opts4);

			char szUserID[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
			int len = EOS_PRODUCTUSERID_MAX_LENGTH + 1;
			EOS_EResult r = EOS_ProductUserId_ToString(lobbyMember, szUserID, &len);
			if (r != EOS_EResult::EOS_Success)
			{
				// TODO_NGMP: Error
				NetworkLog("[NGMP] EOS error!\n");
			}

			NetworkLog("[NGMP] Network room member %d is %s", memberIndex, szUserID);

			// does the user already exist? if not, register the user
            if (m_mapMembers.find(lobbyMember) == m_mapMembers.end())
			{
				m_mapMembers.emplace(lobbyMember, NetworkRoomMember());

				// new member, send them a hello!
				// TODO_NGMP: More robust impl
				m_pNetRoomMesh->SendHelloMsg(lobbyMember);
			}

			// read member data we care about
			EOS_Lobby_Attribute* attrDisplayName = nullptr;
			EOS_LobbyDetails_CopyMemberAttributeByKeyOptions copyAttrOpts;
			copyAttrOpts.ApiVersion = EOS_LOBBYDETAILS_COPYMEMBERATTRIBUTEBYKEY_API_LATEST;
			copyAttrOpts.AttrKey = "DISPLAY_NAME";
			copyAttrOpts.TargetUserId = lobbyMember;
			EOS_EResult rCopyMemberAttr = EOS_LobbyDetails_CopyMemberAttributeByKey(LobbyInstHandle, &copyAttrOpts, &attrDisplayName);
			if (rCopyMemberAttr != EOS_EResult::EOS_Success)
			{
				// Handle gracefully and fall back to stringified version of user id
				m_mapMembers[lobbyMember].m_strName = AsciiString(szUserID);
				NetworkLog("[NGMP] Couldn't read display name\n");
			}
			else
			{
				m_mapMembers[lobbyMember].m_strName = attrDisplayName->Data->Value.AsUtf8;
			}
			
		}
	}

	if (m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}
	*/
}

void NGMP_OnlineServices_RoomsInterface::JoinRoom(int roomIndex, std::function<void()> onStartCallback, std::function<void()> onCompleteCallback)
{
	/*
	// TODO_NGMP: Only set this when actually joining successfully
	m_CurrentRoomID = roomIndex;

	m_PendingRoomJoinCompleteCallback = onCompleteCallback;

	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	EOS_Lobby_CreateLobbySearchOptions CreateSearchOptions = {};
	CreateSearchOptions.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
	CreateSearchOptions.MaxResults = 1;


	EOS_EResult Result = EOS_Lobby_CreateLobbySearch(LobbyHandle, &CreateSearchOptions, &m_SearchHandle);

	if (Result == EOS_EResult::EOS_Success)
	{
		EOS_LobbySearch_FindOptions FindOptions = {};
		FindOptions.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
		FindOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();

		// get room internal name
		// TODO_NGMP: Safer method here, plus handle case where not found
		NetworkRoom targetNetworkRoom = NGMP_OnlineServicesManager::GetInstance()->GetGroupRooms().at(roomIndex);

		// must match name
		EOS_LobbySearch_SetLobbyIdOptions lobbyIdOpts;
		lobbyIdOpts.ApiVersion = EOS_LOBBYSEARCH_SETLOBBYID_API_LATEST;
		lobbyIdOpts.LobbyId = targetNetworkRoom.GetRoomInternalName().str();

		EOS_EResult res = EOS_LobbySearch_SetLobbyId(m_SearchHandle, &lobbyIdOpts);
		if (res != EOS_EResult::EOS_Success)
		{
			NetworkLog("[NGMP] Failed to set name search param");
		}
		
		if (onStartCallback != nullptr)
		{
			onStartCallback();
		}

		// TODO_NGMP: Make sure 'finished' callback is called in all error cases
		EOS_LobbySearch_Find(m_SearchHandle, &FindOptions, (void*)roomIndex, [](const EOS_LobbySearch_FindCallbackInfo* Data)
			{
				int roomIndex = (int)Data->ClientData;

				if (Data->ResultCode == EOS_EResult::EOS_Success)
				{
					EOS_LobbySearch_GetSearchResultCountOptions searchResultOpts;
					searchResultOpts.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;

					auto searchHandle = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_SearchHandle;
					uint32_t numSessions = EOS_LobbySearch_GetSearchResultCount(searchHandle, &searchResultOpts);

					// cache our details
					EOS_LobbySearch_CopySearchResultByIndexOptions getOpts;
					getOpts.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
					getOpts.LobbyIndex = 0;

					NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_currentRoomDetailsHandle = nullptr;
					EOS_LobbySearch_CopySearchResultByIndex(searchHandle, &getOpts, &(NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_currentRoomDetailsHandle));
					
					if (numSessions == 0)
					{
						NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->CreateRoom(roomIndex);
					}
					else
					{
						EOS_Lobby_JoinLobbyOptions joinOpts;
						joinOpts.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
						joinOpts.LobbyDetailsHandle = NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_currentRoomDetailsHandle;
						joinOpts.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
						joinOpts.bPresenceEnabled = false;
						joinOpts.LocalRTCOptions = nullptr;
						joinOpts.bCrossplayOptOut = false;
						joinOpts.RTCRoomJoinActionType = EOS_ELobbyRTCRoomJoinActionType::EOS_LRRJAT_ManualJoin;


						EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());
						EOS_Lobby_JoinLobby(LobbyHandle, &joinOpts, nullptr, [](const EOS_Lobby_JoinLobbyCallbackInfo* Data)
							{
								if (Data->ResultCode == EOS_EResult::EOS_Success)
								{
									NetworkLog("[NGMP] Joined room!");

									// Set our properties
									NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->ResetCachedRoomData();
									NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();
								}
								else
								{
									NetworkLog("[NGMP] Failed to join room!");
								}
							});

						if (NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_PendingRoomJoinCompleteCallback != nullptr)
						{
							NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_PendingRoomJoinCompleteCallback();
						}
					}
				}
				else if (Data->ResultCode == EOS_EResult::EOS_NotFound)
				{
					// create
					NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->CreateRoom(roomIndex);
				}
				else
				{
					NetworkLog("[NGMP] Failed to search for Lobbies!");
				}
			});
	}
	*/
}

void NGMP_OnlineServices_RoomsInterface::CreateRoom(int roomIndex)
{
	/*
	NetworkRoom targetNetworkRoom = NGMP_OnlineServicesManager::GetInstance()->GetGroupRooms().at(roomIndex);

	EOS_HLobby lobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	EOS_Lobby_CreateLobbyOptions* createLobbyOpts = new EOS_Lobby_CreateLobbyOptions();
	createLobbyOpts->ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
	createLobbyOpts->LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	createLobbyOpts->MaxLobbyMembers = 64;
	createLobbyOpts->PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
	createLobbyOpts->bPresenceEnabled = false;
	createLobbyOpts->BucketId = "NETWORK_ROOMS";
	createLobbyOpts->LobbyId = targetNetworkRoom.GetRoomInternalName().str();
	createLobbyOpts->bDisableHostMigration = false; // we can host migrate netrooms
	createLobbyOpts->bEnableRTCRoom = false;
	createLobbyOpts->LocalRTCOptions = nullptr;
	createLobbyOpts->bEnableJoinById = true;
	createLobbyOpts->bRejoinAfterKickRequiresInvite = false;
	createLobbyOpts->AllowedPlatformIds = nullptr;
	createLobbyOpts->AllowedPlatformIdsCount = 0;
	createLobbyOpts->bCrossplayOptOut = false;
	createLobbyOpts->RTCRoomJoinActionType = EOS_ELobbyRTCRoomJoinActionType::EOS_LRRJAT_ManualJoin;

	EOS_Lobby_CreateLobby(lobbyHandle, createLobbyOpts, (void*)roomIndex, [](const EOS_Lobby_CreateLobbyCallbackInfo* Data)
		{
			//int roomIndex = (int)Data->ClientData;

			// TODO_NGMP: If we get a result saying it exists, just join it
			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				// TODO_NGMP: We need to search again to get lobby details handle... but we shouldnt just call join again, its hacky
				//NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->JoinRoom(roomIndex, nullptr, nullptr);

				NetworkLog("[NGMP] Lobby created!\n");

				if (NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_PendingRoomJoinCompleteCallback != nullptr)
				{
					NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->m_PendingRoomJoinCompleteCallback();
				}
				
				// Set our properties
				NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->ResetCachedRoomData();
				NGMP_OnlineServicesManager::GetInstance()->GetRoomsInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();
			}
			else
			{
				NetworkLog("[NGMP] Failed to create lobby!\n");
			}

		});
		*/
}

std::map<uint64_t, NetworkRoomMember>& NGMP_OnlineServices_RoomsInterface::GetMembersListForCurrentRoom()
{
	NetworkLog("[NGMP] Refreshing network room roster");
	return m_mapMembers;
}

void NGMP_OnlineServices_RoomsInterface::SendChatMessageToCurrentRoom(UnicodeString& strChatMsgUnicode)
{
	// TODO_NGMP: Support unicode again
	AsciiString strChatMsg;
	strChatMsg.translate(strChatMsgUnicode);

	NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_RoomChatMessage(strChatMsg.str());

	NetRoom_ChatMessagePacket chatPacket(strChatMsg);

	std::vector<uint64_t> vecUsers;
	for (auto kvPair : m_mapMembers)
	{
		vecUsers.push_back(kvPair.first);
	}

	//m_pNetRoomMesh->SendToMesh(chatPacket, vecUsers);
}

void NGMP_OnlineServices_RoomsInterface::ApplyLocalUserPropertiesToCurrentNetworkRoom()
{
	/*
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());
	NetworkRoom targetNetworkRoom = NGMP_OnlineServicesManager::GetInstance()->GetGroupRooms().at(m_CurrentRoomID);

	EOS_Lobby_UpdateLobbyModificationOptions ModifyOptions = {};
	ModifyOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
	ModifyOptions.LobbyId = targetNetworkRoom.GetRoomInternalName().str();
	ModifyOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	EOS_HLobbyModification LobbyModification = nullptr;
	EOS_EResult Result = EOS_Lobby_UpdateLobbyModification(LobbyHandle, &ModifyOptions, &LobbyModification);
	if (Result != EOS_EResult::EOS_Success)
	{
		// TODO_NGMP: Error
		NetworkLog("[NGMP] Failed to EOS_Lobby_UpdateLobbyModification!\n");
	}

	EOS_Lobby_AttributeData AttributeData;
	AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
	AttributeData.Key = "DISPLAY_NAME";
	AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
	AttributeData.Value.AsUtf8 = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName().str();

	EOS_LobbyModification_AddMemberAttributeOptions addMemberAttrOpts;
	addMemberAttrOpts.ApiVersion = EOS_LOBBYMODIFICATION_ADDMEMBERATTRIBUTE_API_LATEST;
	addMemberAttrOpts.Attribute = &AttributeData;
	addMemberAttrOpts.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
	EOS_EResult r = EOS_LobbyModification_AddMemberAttribute(LobbyModification, &addMemberAttrOpts);
	if (r != EOS_EResult::EOS_Success)
	{
		// TODO_NGMP: Error
		NetworkLog("[NGMP] Failed to set our local user properties in net room!\n");
	}

	// now update lobby
	EOS_Lobby_UpdateLobbyOptions UpdateOptions = {};
	UpdateOptions.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
	UpdateOptions.LobbyModificationHandle = LobbyModification;
	EOS_Lobby_UpdateLobby(LobbyHandle, &UpdateOptions, nullptr, [](const EOS_Lobby_UpdateLobbyCallbackInfo* Data)
		{
			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				NetworkLog("[NGMP] Lobby Updated!\n");
			}
			else
			{
				NetworkLog("[NGMP] Lobby Update failed!\n");
			}
		});

	// join the network mesh too
	m_pNetRoomMesh = new NetworkMesh(ENetworkMeshType::NETWORK_ROOM);
	m_pNetRoomMesh->ConnectToMesh(targetNetworkRoom.GetRoomInternalName().str());

	if (m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}
	*/
}