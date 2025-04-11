#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"

extern NGMPGame* TheNGMPGame;

UnicodeString NGMP_OnlineServices_LobbyInterface::GetCurrentLobbyDisplayName()
{
	UnicodeString strDisplayName;

	if (IsInLobby())
	{
		strDisplayName.format(L"%hs", m_CurrentLobby.name.c_str());
	}

	return strDisplayName;
}

UnicodeString NGMP_OnlineServices_LobbyInterface::GetCurrentLobbyMapDisplayName()
{
	UnicodeString strDisplayName;

	if (IsInLobby())
	{
		strDisplayName.format(L"%hs", m_CurrentLobby.map_name.c_str());
	}

	return strDisplayName;
}

AsciiString NGMP_OnlineServices_LobbyInterface::GetCurrentLobbyMapPath()
{
	AsciiString strPath;

	if (IsInLobby())
	{
		strPath = m_CurrentLobby.map_path.c_str();
	}

	return strPath;
}

void NGMP_OnlineServices_LobbyInterface::SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode)
{
	// TODO_NGMP: Custom
	// TODO_NGMP: Support unicode again
	AsciiString strChatMsg;
	strChatMsg.translate(strChatMsgUnicode);

	NetRoom_ChatMessagePacket chatPacket(strChatMsg);

	// TODO_NGMP: Move to uint64 for user id
	std::vector<int64_t> vecUsersToSend;
	for (auto kvPair : m_CurrentLobby.members)
	{
		vecUsersToSend.push_back(kvPair.user_id);
	}
	
	if (m_pLobbyMesh != nullptr)
	{
		m_pLobbyMesh->SendToMesh(chatPacket, vecUsersToSend);
	}
}

NGMP_OnlineServices_LobbyInterface::NGMP_OnlineServices_LobbyInterface()
{
	/*
	// Register for EOS callbacks, we will handle them internally and pass them onto the game as necessary
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	// player joined/left etc
	EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions lobbyStatusRecievedOpts;
	lobbyStatusRecievedOpts.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
	EOS_NotificationId notID1 = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(LobbyHandle, &lobbyStatusRecievedOpts, nullptr, [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* Data)
		{
			// TODO_NGMP: Custom
			//NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateRoomDataCache();
		});

	// player data changed
	EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions lobbyMemberUpdateRecievedOpts;
	lobbyMemberUpdateRecievedOpts.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERUPDATERECEIVED_API_LATEST;
	EOS_NotificationId notID2 = EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(LobbyHandle, &lobbyMemberUpdateRecievedOpts, nullptr, [](const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* Data)
		{
			// TODO_NGMP: Custom
			//NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->UpdateRoomDataCache();
		});

	notID1 = notID1;
	notID2 = notID2;
	*/
}

void NGMP_OnlineServices_LobbyInterface::SearchForLobbies(std::function<void()> onStartCallback, std::function<void(std::vector<LobbyEntry>)> onCompleteCallback)
{
	m_vecLobbies.clear();

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies", true);
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody)
	{
		// TODO_NGMP: Error handling
		try
		{
			nlohmann::json jsonObject = nlohmann::json::parse(strBody);

			for (const auto& lobbyEntryIter : jsonObject["lobbies"])
			{
				LobbyEntry lobbyEntry;
				lobbyEntryIter["id"].get_to(lobbyEntry.lobbyID);
				lobbyEntryIter["owner"].get_to(lobbyEntry.owner);
				lobbyEntryIter["name"].get_to(lobbyEntry.name);
				lobbyEntryIter["map_name"].get_to(lobbyEntry.map_name);
				lobbyEntryIter["map_path"].get_to(lobbyEntry.map_path);
				lobbyEntryIter["current_players"].get_to(lobbyEntry.current_players);
				lobbyEntryIter["max_players"].get_to(lobbyEntry.max_players);

				for (const auto& memberEntryIter : lobbyEntryIter["members"])
				{
					LobbyMemberEntry memberEntry;

					memberEntryIter["user_id"].get_to(memberEntry.user_id);
					memberEntryIter["display_name"].get_to(memberEntry.display_name);
					memberEntryIter["ready"].get_to(memberEntry.ready);

					// NOTE: These fields won't be present becauase they're private properties
					memberEntryIter["ip_addr"].get_to(memberEntry.strIPAddress);
					memberEntryIter["port"].get_to(memberEntry.preferredPort);

					lobbyEntry.members.push_back(memberEntry);
				}

				m_vecLobbies.push_back(lobbyEntry);
			}
		}
		catch (...)
		{

		}

		onCompleteCallback(m_vecLobbies);
	});
}

bool NGMP_OnlineServices_LobbyInterface::IsHost()
{
	if (IsInLobby())
	{
		int64_t myUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
		return m_CurrentLobby.owner == myUserID;
	}

	return false;
}

void NGMP_OnlineServices_LobbyInterface::ApplyLocalUserPropertiesToCurrentNetworkRoom()
{
	/*
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	EOS_Lobby_UpdateLobbyModificationOptions ModifyOptions = {};
	ModifyOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
	ModifyOptions.LobbyId = m_strCurrentLobbyID.c_str();
	ModifyOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	EOS_HLobbyModification LobbyModification = nullptr;
	EOS_EResult Result = EOS_Lobby_UpdateLobbyModification(LobbyHandle, &ModifyOptions, &LobbyModification);
	if (Result != EOS_EResult::EOS_Success)
	{
		// TODO_NGMP: Error
		NetworkLog("[NGMP] Failed to EOS_Lobby_UpdateLobbyModification!\n");
	}

	// DISPLAY NAME
	{
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
	}

	GameSlot* pLocalSlot = TheNGMPGame->getSlot(TheNGMPGame->getLocalSlotNum());
	// READY FLAG
	{
		EOS_Lobby_AttributeData AttributeData;
		AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
		AttributeData.Key = "READY_ACCEPTED";
		AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_BOOLEAN;

		if (IsHost())
		{
			AttributeData.Value.AsBool = true;
		}
		else if (pLocalSlot == nullptr)
		{
			AttributeData.Value.AsBool = false;
		}
		else
		{
			pLocalSlot->isAccepted();
		}

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

			// inform game instance too
			if (TheNGMPGame != nullptr)
			{
				TheNGMPGame->UpdateSlotsFromCurrentLobby();
			}

			if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_RosterNeedsRefreshCallback != nullptr)
			{
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_RosterNeedsRefreshCallback();
			}
		});

	

	// inform game instance too
	// TODO_NGMP: replace all these with UpdateRoomDataCache
	if (TheNGMPGame != nullptr)
	{
		TheNGMPGame->UpdateSlotsFromCurrentLobby();
	}

	if (m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}
	*/
}

void NGMP_OnlineServices_LobbyInterface::UpdateRoomDataCache(std::function<void(void)> fnCallback)
{
	// refresh lobby
	if (m_CurrentLobby.lobbyID != -1)
	{
		std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby", true), m_CurrentLobby.lobbyID);
		std::map<std::string, std::string> mapHeaders;

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody)
		{
			// TODO_NGMP: Error handling
				try
			{
				nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody);

				auto lobbyEntryJSON = jsonObjectRoot["lobby"];

				LobbyEntry lobbyEntry;
				lobbyEntryJSON["id"].get_to(lobbyEntry.lobbyID);
				lobbyEntryJSON["owner"].get_to(lobbyEntry.owner);
				lobbyEntryJSON["name"].get_to(lobbyEntry.name);
				lobbyEntryJSON["map_name"].get_to(lobbyEntry.map_name);
				lobbyEntryJSON["map_path"].get_to(lobbyEntry.map_path);
				lobbyEntryJSON["current_players"].get_to(lobbyEntry.current_players);
				lobbyEntryJSON["max_players"].get_to(lobbyEntry.max_players);

				for (const auto& memberEntryIter : lobbyEntryJSON["members"])
				{
					LobbyMemberEntry memberEntry;

					memberEntryIter["user_id"].get_to(memberEntry.user_id);
					memberEntryIter["display_name"].get_to(memberEntry.display_name);
					memberEntryIter["ready"].get_to(memberEntry.ready);
					memberEntryIter["ip_addr"].get_to(memberEntry.strIPAddress);
					memberEntryIter["port"].get_to(memberEntry.preferredPort);

					lobbyEntry.members.push_back(memberEntry);

					// TODO_NGMP: Much more robust system here
					// TODO_NGMP: If we lose connection to someone in the mesh, who is STILL in otehrs mesh, we need to disconnect or retry
					// TODO_NGMP: handle failure to connect to some users
					
					// is it a new member? connect
					bool bIsNew = true;
					for (LobbyMemberEntry& currentMember : m_CurrentLobby.members)
					{
						if (currentMember.user_id == memberEntry.user_id)
						{
							bIsNew = false;
							break;
						}
					}

					if (bIsNew)
					{
						// if we're joining as a client (not host), lobby mesh will be null here, but it's ok because the initial creation will sync to everyone
						if (m_pLobbyMesh != nullptr)
						{
							m_pLobbyMesh->ConnectToSingleUser(memberEntry);
						}
					}
				}

				// store
				m_CurrentLobby = lobbyEntry;

				// update NGMP Game if it exists

				// inform game instance too
				if (TheNGMPGame != nullptr)
				{
					TheNGMPGame->SyncWithLobby(m_CurrentLobby);
					TheNGMPGame->UpdateSlotsFromCurrentLobby();

					if (m_RosterNeedsRefreshCallback != nullptr)
					{
						m_RosterNeedsRefreshCallback();
					}
				}

				if (fnCallback != nullptr)
				{
					fnCallback();
				}
			}
			catch (...)
			{

			}
		});
	}

	return;

	/*
	// process users
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());
	
	// get a handle to our lobby
	EOS_Lobby_CopyLobbyDetailsHandleOptions opts;
	opts.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
	opts.LobbyId = m_strCurrentLobbyID.c_str();
	opts.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();

	EOS_HLobbyDetails LobbyInstHandle;
	EOS_EResult getLobbyHandlResult = EOS_Lobby_CopyLobbyDetailsHandle(LobbyHandle, &opts, &LobbyInstHandle);

	if (getLobbyHandlResult == EOS_EResult::EOS_Success)
	{
		// get owner
		EOS_LobbyDetails_GetLobbyOwnerOptions getLobbyOwnerOpts;
		getLobbyOwnerOpts.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
		EOS_ProductUserId currentLobbyHost = EOS_LobbyDetails_GetLobbyOwner(LobbyInstHandle, &getLobbyOwnerOpts);

		// get each member
		EOS_LobbyDetails_GetMemberCountOptions optsMemberCount;
		optsMemberCount.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;

		
	}

	if (m_RosterNeedsRefreshCallback != nullptr)
	{
		m_RosterNeedsRefreshCallback();
	}

	// inform game instance too
	if (TheNGMPGame != nullptr)
	{
		TheNGMPGame->UpdateSlotsFromCurrentLobby();
	}
	*/
}

void NGMP_OnlineServices_LobbyInterface::JoinLobby(int index)
{
	m_CurrentLobby = LobbyEntry();
	LobbyEntry lobbyInfo = GetLobbyFromIndex(index);

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby", true), lobbyInfo.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	NetworkLog("[NGMP] Joining lobby with id %d", lobbyInfo.lobbyID);

	nlohmann::json j;
	j["preferred_port"] = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody)
		{
			// TODO_NGMP: Dont do extra get here, just return it in the put...
			bool bJoinSuccess = statusCode == 200;

			// no response body from this, just http codes
			if (statusCode == 200)
			{
				NetworkLog("[NGMP] Joined lobby");

				m_CurrentLobby = lobbyInfo;

				// TODO_NGMP: Cleanup game + dont store 2 ptrs
				if (m_pGameInst == nullptr)
				{
					m_pGameInst = new NGMPGame();
					TheNGMPGame = m_pGameInst;

					AsciiString localName = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName();
					TheNGMPGame->setLocalName(localName);

					// set in game, this actually means in lobby... not in game play, and is necessary to start the game
					TheNGMPGame->setInGame();

					// dont need to do these here, updateroomdatacache does it for us
					//TheNGMPGame->SyncWithLobby(m_CurrentLobby);
					//TheNGMPGame->UpdateSlotsFromCurrentLobby();

					// TODO_NGMP: Rest of these
					/*
					TheNGMPGame.setExeCRC(info->getExeCRC());
					TheNGMPGame.setIniCRC(info->getIniCRC());
					TheNGMPGame.setAllowObservers(info->getAllowObservers());
					TheNGMPGame.setHasPassword(info->getHasPassword());
					TheNGMPGame.setGameName(info->getGameName());
					*/
				}

				// we need to get more lobby info before triggering the game callback...
				UpdateRoomDataCache([=]()
					{
						OnJoinedOrCreatedLobby();
					});
			}
			else if (statusCode == 401)
			{
				NetworkLog("[NGMP] Couldn't join lobby");
			}
			else if (statusCode == 404)
			{
				NetworkLog("[NGMP] Failed to join lobby: Lobby not found");
			}
			else
			{

			}

			if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
			{
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(bJoinSuccess);
			}
		});

	// TODO_NGMP:
	/*
	EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());


	// We need to browse again for the lobby, EOS_Lobby_CopyLobbyDetailsHandle is only available to people in the lobby, so we need to use EOS_LobbySearch_CopySearchResultByIndex
	EOS_Lobby_CreateLobbySearchOptions CreateSearchOptions = {};
	CreateSearchOptions.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
	CreateSearchOptions.MaxResults = 1;

	
	EOS_EResult Result = EOS_Lobby_CreateLobbySearch(LobbyHandle, &CreateSearchOptions, &m_SearchHandle);

	if (Result == EOS_EResult::EOS_Success)
	{
		EOS_LobbySearch_FindOptions FindOptions = {};
		FindOptions.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
		FindOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();

		// TODO_NGMP: Safer method here, plus handle case where not found
		LobbyEntry lobbyInfo = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetLobbyFromIndex(index);

		// must match name
		EOS_LobbySearch_SetLobbyIdOptions lobbyIdOpts;
		lobbyIdOpts.ApiVersion = EOS_LOBBYSEARCH_SETLOBBYID_API_LATEST;
		lobbyIdOpts.LobbyId = lobbyInfo.m_strLobbyID.c_str();

		EOS_EResult res = EOS_LobbySearch_SetLobbyId(m_SearchHandle, &lobbyIdOpts);
		if (res != EOS_EResult::EOS_Success)
		{
			NetworkLog("[NGMP] Failed to set name search param");
		}

		// TODO_NGMP: Make sure 'finished' callback is called in all error cases
		EOS_LobbySearch_Find(m_SearchHandle, &FindOptions, (void*)index, [](const EOS_LobbySearch_FindCallbackInfo* Data)
			{
				//int lobbyIndex = (int)Data->ClientData;

				if (Data->ResultCode == EOS_EResult::EOS_Success)
				{
					EOS_LobbySearch_GetSearchResultCountOptions searchResultOpts;
					searchResultOpts.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;

					auto searchHandle = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_SearchHandle;
					uint32_t numSessions = EOS_LobbySearch_GetSearchResultCount(searchHandle, &searchResultOpts);

					// cache our details
					EOS_LobbySearch_CopySearchResultByIndexOptions getOpts;
					getOpts.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
					getOpts.LobbyIndex = 0;

					EOS_HLobbyDetails detailsHandle = nullptr;
					EOS_LobbySearch_CopySearchResultByIndex(searchHandle, &getOpts, &detailsHandle);

					if (numSessions == 0)
					{
						NetworkLog("[NGMP] Failed to find lobby to join!\n");

						if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
						{
							NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(false);
						}
					}
					else
					{
						EOS_Lobby_JoinLobbyOptions joinOpts;
						joinOpts.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
						joinOpts.LobbyDetailsHandle = detailsHandle;
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
									NetworkLog("[NGMP] Joined lobby!");

									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_strCurrentLobbyID = Data->LobbyId;
									NetworkLog("[NGMP] Joined lobby!\n");

									// TODO_NGMP: Impl
									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->OnJoinedOrCreatedLobby();
									// Set our properties
									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ResetCachedRoomData();
									NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();


									if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
									{
										NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(true);
									}
								}
								else
								{
									NetworkLog("[NGMP] Failed to join lobby!");
									if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
									{
										NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(false);
									}
								}
							});
					}
				}
				else if (Data->ResultCode == EOS_EResult::EOS_NotFound)
				{
					NetworkLog("[NGMP] Failed to find lobby to join!\n");

					if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
					{
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(false);
					}
				}
				else
				{
					NetworkLog("[NGMP] Failed to search for Lobbies!");

					if (NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby != nullptr)
					{
						NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_callbackJoinedLobby(false);
					}
				}
			});
	}
	*/
}

LobbyEntry NGMP_OnlineServices_LobbyInterface::GetLobbyFromIndex(int index)
{
	// TODO_NGMP: safety
	return m_vecLobbies.at(index);
}

enum class ECreateLobbyResponseResult : int
{
	FAILED = 0,
	SUCCEEDED = 1
};

struct CreateLobbyResponse
{
	ECreateLobbyResponseResult result = ECreateLobbyResponseResult::FAILED;
	int64_t lobby_id = -1;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreateLobbyResponse, result, lobby_id)
};

void NGMP_OnlineServices_LobbyInterface::CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, int initialMaxSize)
{
	m_CurrentLobby = LobbyEntry();	
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies", true);
	std::map<std::string, std::string> mapHeaders;

	// convert
	AsciiString strName = AsciiString();
	strName.translate(strLobbyName);

	AsciiString strMapName = AsciiString();
	strMapName.translate(strInitialMapName);

	nlohmann::json j;
	j["name"] = strName.str();
	j["map_name"] = strMapName.str();
	j["map_path"] = strInitialMapPath.str();
	j["max_players"] = initialMaxSize;
	j["preferred_port"] = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();
	std::string strPostData = j.dump();

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody)
		{
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				CreateLobbyResponse resp = jsonObject.get<CreateLobbyResponse>();

				if (resp.result == ECreateLobbyResponseResult::SUCCEEDED)
				{
					// TODO: Do we need more info here? we kick off a lobby GET immediately, maybe that should be the response to creating
					
					// store the basic info (lobby id), we will immediately kick off a full get				
					m_CurrentLobby.lobbyID = resp.lobby_id;
					m_CurrentLobby.owner = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();

					m_CurrentLobby.name = std::string(strName.str());
					m_CurrentLobby.map_name = std::string(strMapName.str());
					m_CurrentLobby.map_path = std::string(strInitialMapPath.str());
					m_CurrentLobby.current_players = 1;
					m_CurrentLobby.max_players = initialMaxSize;

					LobbyMemberEntry me;

					me.user_id = m_CurrentLobby.owner;
					me.display_name = std::string(NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName().str());
					me.ready = true; // host is always ready
					me.strIPAddress = "127.0.0.1";
					me.preferredPort = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();

					m_CurrentLobby.members.push_back(me);

	
					// TODO_NGMP: Cleanup game + dont store 2 ptrs
					if (m_pGameInst == nullptr)
					{
						m_pGameInst = new NGMPGame();
						TheNGMPGame = m_pGameInst;


						// TODO_NGMP: Rest of these
						/*
						TheNGMPGame.setLocalName(m_localName);
						TheNGMPGame.setExeCRC(info->getExeCRC());
						TheNGMPGame.setIniCRC(info->getIniCRC());
						TheNGMPGame.setAllowObservers(info->getAllowObservers());
						TheNGMPGame.setHasPassword(info->getHasPassword());
						TheNGMPGame.setGameName(info->getGameName());
						*/
					}


					AsciiString localName = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName();
					TheNGMPGame->setLocalName(localName);

					// set in game, this actually means in lobby... not in game play, and is necessary to start the game
					TheNGMPGame->setInGame();

					TheNGMPGame->SyncWithLobby(m_CurrentLobby);
					TheNGMPGame->UpdateSlotsFromCurrentLobby();

					NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->OnJoinedOrCreatedLobby();
				}
				else
				{
					NetworkLog("[NGMP] Failed to create lobby!\n");
				}

				// TODO_NGMP: Impl
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->InvokeCreateLobbyCallback(resp.result == ECreateLobbyResponseResult::SUCCEEDED);

				// Set our properties
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ResetCachedRoomData();
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();
			}
			catch (...)
			{

			}

		});
	return;
	/*
	m_PendingCreation_LobbyName = strLobbyName;
	m_PendingCreation_InitialMapDisplayName = strInitialMapName;
	m_PendingCreation_InitialMapPath = strInitialMapPath;

	// TODO_NGMP: Correct values
	EOS_HLobby lobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

	EOS_Lobby_CreateLobbyOptions* createLobbyOpts = new EOS_Lobby_CreateLobbyOptions();
	createLobbyOpts->ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
	createLobbyOpts->LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
	createLobbyOpts->MaxLobbyMembers = initialMaxSize;
	createLobbyOpts->PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
	createLobbyOpts->bPresenceEnabled = false;
	createLobbyOpts->BucketId = "CUSTOM_MATCH";
	createLobbyOpts->bDisableHostMigration = true; // Generals doesnt support host migration during lobby... maybe we should fix that
	createLobbyOpts->bEnableRTCRoom = false;
	createLobbyOpts->LocalRTCOptions = nullptr;
	createLobbyOpts->bEnableJoinById = true;
	createLobbyOpts->bRejoinAfterKickRequiresInvite = false;
	createLobbyOpts->AllowedPlatformIds = nullptr;
	createLobbyOpts->AllowedPlatformIdsCount = 0;
	createLobbyOpts->bCrossplayOptOut = false;
	createLobbyOpts->RTCRoomJoinActionType = EOS_ELobbyRTCRoomJoinActionType::EOS_LRRJAT_ManualJoin;

	EOS_Lobby_CreateLobby(lobbyHandle, createLobbyOpts, nullptr, [](const EOS_Lobby_CreateLobbyCallbackInfo* Data)
		{
			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->SetCurrentLobbyID(Data->LobbyId);
				
				NetworkLog("[NGMP] Lobby created with ID %s!\n", Data->LobbyId);

				// add version param
				EOS_HLobby LobbyHandle = EOS_Platform_GetLobbyInterface(NGMP_OnlineServicesManager::GetInstance()->GetEOSPlatformHandle());

				EOS_Lobby_UpdateLobbyModificationOptions ModifyOptions = {};
				ModifyOptions.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
				ModifyOptions.LobbyId = Data->LobbyId;
				ModifyOptions.LocalUserId = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetEOSUser();
				EOS_HLobbyModification LobbyModification = nullptr;
				EOS_EResult Result = EOS_Lobby_UpdateLobbyModification(LobbyHandle, &ModifyOptions, &LobbyModification);

				// TODO_NGMP: Handle result properly
				if (Result != EOS_EResult::EOS_Success)
				{
					NetworkLog("[NGMP] Failed to set search param");
				}

				// VERSION
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "VERSION";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
					AttributeData.Value.AsInt64 = 1337; // TODO_NGMP: Proper value

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
				}

				// NAME
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "NAME";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;

					AsciiString strName = AsciiString();
					strName.translate(NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_PendingCreation_LobbyName);
					AttributeData.Value.AsUtf8 = strName.str();

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
				}

				// OWNER NAME
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "OWNER_NAME";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;

					AttributeData.Value.AsUtf8 = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetDisplayName().str();

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
				}

				// MAP NAME
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "MAP_NAME";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
;
					AsciiString strMapName = AsciiString();
					strMapName.translate(NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_PendingCreation_InitialMapDisplayName);
					AttributeData.Value.AsUtf8 = strMapName.str();

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
				}

				// MAP PATH
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "MAP_PATH";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_STRING;
					
					AsciiString strMapPath = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->m_PendingCreation_InitialMapPath;
					AttributeData.Value.AsUtf8 = strMapPath.str();

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
				}

				// NAT TYPE
				{
					EOS_LobbyModification_AddAttributeOptions AddAttributeModOptions;
					EOS_Lobby_AttributeData AttributeData;
					AttributeData.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
					AttributeData.Key = "NAT";
					AttributeData.ValueType = EOS_ELobbyAttributeType::EOS_AT_INT64;
					;
					AttributeData.Value.AsInt64 = NGMP_OnlineServicesManager::GetInstance()->GetNATType();

					AddAttributeModOptions.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
					AddAttributeModOptions.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
					AddAttributeModOptions.Attribute = &AttributeData;

					EOS_EResult AddResult = EOS_LobbyModification_AddAttribute(LobbyModification, &AddAttributeModOptions);
					if (AddResult != EOS_EResult::EOS_Success)
					{
						NetworkLog("[NGMP] Failed to set lobby param");
					}
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

				NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->OnJoinedOrCreatedLobby();
			}
			else
			{
				NetworkLog("[NGMP] Failed to create lobby!\n");
			}

			// TODO_NGMP: Impl
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->InvokeCreateLobbyCallback(Data->ResultCode == EOS_EResult::EOS_Success);

			// Set our properties
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ResetCachedRoomData();
			NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->ApplyLocalUserPropertiesToCurrentNetworkRoom();
		});
		*/
}

void NGMP_OnlineServices_LobbyInterface::OnJoinedOrCreatedLobby()
{
	// TODO_NGMP: We need this on create, but this is a double call on join because we already got this info
	// must be done in a callback, this is an async function
	UpdateRoomDataCache();

	// join the network mesh too
	if (m_pLobbyMesh == nullptr)
	{
		m_pLobbyMesh = new NetworkMesh(ENetworkMeshType::GAME_LOBBY);
		m_pLobbyMesh->ConnectToMesh(m_CurrentLobby);
	}
}
