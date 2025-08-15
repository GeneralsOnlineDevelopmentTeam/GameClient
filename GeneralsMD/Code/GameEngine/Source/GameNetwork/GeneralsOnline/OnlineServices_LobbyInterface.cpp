#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameClient/MapUtil.h"

extern void OnKickedFromLobby();

extern NGMPGame* TheNGMPGame;

struct JoinLobbyResponse
{
	bool success = false;
	std::string turn_username;
	std::string turn_token;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(JoinLobbyResponse, success, turn_username, turn_token)
};

UnicodeString NGMP_OnlineServices_LobbyInterface::GetCurrentLobbyDisplayName()
{
	UnicodeString strDisplayName;

	if (IsInLobby())
	{
		strDisplayName = UnicodeString(from_utf8(m_CurrentLobby.name).c_str());
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

enum class ELobbyUpdateField
{
	LOBBY_MAP = 0,
	MY_SIDE = 1,
	MY_COLOR = 2,
	MY_START_POS = 3,
	MY_TEAM = 4,
	LOBBY_STARTING_CASH = 5,
	LOBBY_LIMIT_SUPERWEAPONS = 6,
	HOST_ACTION_FORCE_START = 7,
	LOCAL_PLAYER_HAS_MAP = 8,
	GAME_STARTED = 9,
	GAME_FINISHED = 10,
	HOST_ACTION_KICK_USER = 11,
	HOST_ACTION_SET_SLOT_STATE = 12,
	AI_SIDE = 13,
	AI_COLOR = 14,
	AI_TEAM = 15,
	AI_START_POS = 16,
	MAX_CAMERA_HEIGHT = 17
};

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_Map(AsciiString strMap, AsciiString strMapPath, bool bIsOfficial, int newMaxPlayers)
{
	// reset autostart if host changes anything (because ready flag will reset too)
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	// sanitize map path
	// we need to parse out the map name for custom maps... its an absolute path
	// it's safe to just get the file name, dir name and file name MUST be the same. Game enforces this
	AsciiString sanitizedMapPath = strMapPath;
	if (sanitizedMapPath.reverseFind('\\'))
	{
		sanitizedMapPath = sanitizedMapPath.reverseFind('\\') + 1;
	}

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::LOBBY_MAP;
	j["map"] = strMap.str();
	j["map_path"] = sanitizedMapPath.str();
	j["map_official"] = bIsOfficial;
	j["max_players"] = newMaxPlayers;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			
		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_LimitSuperweapons(bool bLimitSuperweapons)
{
	// reset autostart if host changes anything (because ready flag will reset too)
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::LOBBY_LIMIT_SUPERWEAPONS;
	j["limit_superweapons"] = bLimitSuperweapons;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_StartingCash(UnsignedInt startingCashValue)
{
	// reset autostart if host changes anything (because ready flag will reset too)
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::LOBBY_STARTING_CASH;
	j["startingcash"] = startingCashValue;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::MarkCurrentGameAsStarted()
{
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::GAME_STARTED;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::MarkCurrentGameAsFinished()
{
	if (m_bMarkedGameAsFinished)
	{
		return;
	}

	m_bMarkedGameAsFinished = true;

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::GAME_FINISHED;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}


void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_HasMap()
{
	// do we have the map?
	bool bHasMap = TheMapCache->findMap(AsciiString(m_CurrentLobby.map_path.c_str()));

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::LOCAL_PLAYER_HAS_MAP;
	j["has_map"] = bHasMap;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

// start AI
void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_AISide(int slot, int side, int updatedStartPos)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::AI_SIDE;
	j["slot"] = slot;
	j["side"] = side;
	j["start_pos"] = updatedStartPos;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_AITeam(int slot, int team)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::AI_TEAM;
	j["slot"] = slot;
	j["team"] = team;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_AIStartPos(int slot, int startpos)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::AI_START_POS;
	j["slot"] = slot;
	j["start_pos"] = startpos;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobbyMaxCameraHeight(uint16_t maxCameraHeight)
{
	if (IsHost())
	{
		UnicodeString strInform;
		strInform.format(L"The host has set the maximum camera height to %lu.", maxCameraHeight);

		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		if (pLobbyInterface != nullptr)
		{
			pLobbyInterface->SendAnnouncementMessageToCurrentLobby(strInform, true);
		}

		// reset autostart if host changes anything (because ready flag will reset too)
		ClearAutoReadyCountdown();

		std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
		std::map<std::string, std::string> mapHeaders;

		nlohmann::json j;
		j["field"] = ELobbyUpdateField::MAX_CAMERA_HEIGHT;
		j["max_camera_height"] = maxCameraHeight;
		std::string strPostData = j.dump();

		// convert
		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{

			});
	}
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_AIColor(int slot, int color)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::AI_COLOR;
	j["slot"] = slot;
	j["color"] = color;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}
// end AI

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_MySide(int side, int updatedStartPos)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::MY_SIDE;
	j["side"] = side;
	j["start_pos"] = updatedStartPos;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_MyColor(int color)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::MY_COLOR;
	j["color"] = color;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_MyStartPos(int startpos)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::MY_START_POS;
	j["startpos"] = startpos;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_MyTeam(int team)
{
	// reset autostart if host changes anything (because ready flag will reset too). This occurs on client too, but nothing happens for them
	ClearAutoReadyCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::MY_TEAM;
	j["team"] = team;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_SetSlotState(uint16_t slotIndex, uint16_t slotState)
{
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::HOST_ACTION_SET_SLOT_STATE;
	j["slot_index"] = slotIndex;
	j["slot_state"] = slotState;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_KickUser(int64_t userID, UnicodeString name)
{
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::HOST_ACTION_KICK_USER;
	j["userid"] = userID;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			UnicodeString msg;
			msg.format(L"'%s' was kicked by the host.", name.str());;
			SendAnnouncementMessageToCurrentLobby(msg, true);
		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_ForceReady()
{
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::HOST_ACTION_FORCE_START;
	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			UnicodeString msg = UnicodeString(L"All players have been forced to ready up.");
			SendAnnouncementMessageToCurrentLobby(msg, true);
		});
}

void NGMP_OnlineServices_LobbyInterface::SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode, bool bIsAction)
{
	WebSocket* pWS = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket();
	if (pWS != nullptr)
	{
		pWS->SendData_LobbyChatMessage(strChatMsgUnicode, bIsAction, false, false);
	}
}

// TODO_NGMP: Just send a separate packet for each announce, more efficient and less hacky
void NGMP_OnlineServices_LobbyInterface::SendAnnouncementMessageToCurrentLobby(UnicodeString& strAnnouncementMsgUnicode, bool bShowToHost)
{
	WebSocket* pWS = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket();
	if (pWS != nullptr)
	{
		pWS->SendData_LobbyChatMessage(strAnnouncementMsgUnicode, false, true, bShowToHost);
	}
}

NGMP_OnlineServices_LobbyInterface::NGMP_OnlineServices_LobbyInterface()
{

}

void NGMP_OnlineServices_LobbyInterface::SearchForLobbies(std::function<void()> onStartCallback, std::function<void(std::vector<LobbyEntry>)> onCompleteCallback)
{
	if (m_bSearchInProgress)
	{
		return;
	}

	m_bSearchInProgress = true;
	m_vecLobbies.clear();

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
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
				lobbyEntryIter["map_official"].get_to(lobbyEntry.map_official);
				lobbyEntryIter["current_players"].get_to(lobbyEntry.current_players);
				lobbyEntryIter["max_players"].get_to(lobbyEntry.max_players);
				lobbyEntryIter["vanilla_teams"].get_to(lobbyEntry.vanilla_teams);
				lobbyEntryIter["starting_cash"].get_to(lobbyEntry.starting_cash);
				lobbyEntryIter["limit_superweapons"].get_to(lobbyEntry.limit_superweapons);
				lobbyEntryIter["track_stats"].get_to(lobbyEntry.track_stats);
				lobbyEntryIter["passworded"].get_to(lobbyEntry.passworded);
				lobbyEntryIter["allow_observers"].get_to(lobbyEntry.allow_observers);
				lobbyEntryIter["max_cam_height"].get_to(lobbyEntry.max_cam_height);
				lobbyEntryIter["exe_crc"].get_to(lobbyEntry.exe_crc);
				lobbyEntryIter["ini_crc"].get_to(lobbyEntry.ini_crc);

				// correct map path
				if (lobbyEntry.map_official)
				{
					lobbyEntry.map_path = std::format("Maps\\{}", lobbyEntry.map_path.c_str());
				}
				else
				{
					lobbyEntry.map_path = std::format("{}\\{}", TheMapCache->getUserMapDir(true).str(), lobbyEntry.map_path.c_str());
				}

				// NOTE: These fields won't be present becauase they're private properties
				//memberEntryIter["enc_key"].get_to(strEncKey);

				for (const auto& memberEntryIter : lobbyEntryIter["members"])
				{
					LobbyMemberEntry memberEntry;

					memberEntryIter["user_id"].get_to(memberEntry.user_id);
					memberEntryIter["display_name"].get_to(memberEntry.display_name);
					memberEntryIter["ready"].get_to(memberEntry.m_bIsReady);
					memberEntryIter["slot_index"].get_to(memberEntry.m_SlotIndex);
					memberEntryIter["slot_state"].get_to(memberEntry.m_SlotState);

					// NOTE: These fields won't be present becauase they're private properties
					//memberEntryIter["ip_addr"].get_to(memberEntry.strIPAddress);
					//memberEntryIter["port"].get_to(memberEntry.preferredPort);


					// TODO_NGMP: MAybe surface the player slots via tooltip?

					// NOTE: These fields wont be presen't because they really don't matter until you're in the match
					//memberEntryIter["side"].get_to(memberEntry.side);
					//memberEntryIter["color"].get_to(memberEntry.color);
					//memberEntryIter["team"].get_to(memberEntry.team);
					//memberEntryIter["startpos"].get_to(memberEntry.startpos);
					//memberEntryIter["has_map"].get_to(memberEntry.has_map);


					lobbyEntry.members.push_back(memberEntry);
				}

				m_vecLobbies.push_back(lobbyEntry);
			}
		}
		catch (...)
		{

		}

		onCompleteCallback(m_vecLobbies);
		m_bSearchInProgress = false;
	});
}

bool NGMP_OnlineServices_LobbyInterface::IsHost()
{
	if (IsInLobby())
	{
		NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
		int64_t myUserID = pAuthInterface == nullptr ? -1 : pAuthInterface->GetUserID();
		return m_CurrentLobby.owner == myUserID;
	}

	return false;
}

void NGMP_OnlineServices_LobbyInterface::ApplyLocalUserPropertiesToCurrentNetworkRoom()
{
	// TODO_NGMP: Better detection of this, dont update always
	
	// TODO_NGMP: Support unreadying again when player changes team etc
	// are we ready?

	WebSocket* pWS = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket();
	if (pWS != nullptr)
	{
		GameSlot* pLocalSlot = TheNGMPGame->getSlot(TheNGMPGame->getLocalSlotNum());
		if (IsHost())
		{
			pWS->SendData_MarkReady(true);
		}
		else
		{
			pWS->SendData_MarkReady(pLocalSlot->isAccepted());
		}
	}
}

void NGMP_OnlineServices_LobbyInterface::UpdateRoomDataCache(std::function<void(void)> fnCallback)
{
	// refresh lobby
	if (m_CurrentLobby.lobbyID != -1 && TheNGMPGame != nullptr)
	{
		std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
		std::map<std::string, std::string> mapHeaders;

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// safety, lobby could've been torn down by the time we get our response
				if (m_CurrentLobby.lobbyID != -1 && TheNGMPGame != nullptr)
				{
					// dont let the service be authoritative during gameplay, the game host handles connections at this point
					if (TheNGMPGame->isGameInProgress())
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Ignoring lobby update request during gameplay.");
						return;
					}

					// TODO_NGMP: Error handling
					try
					{
						if (statusCode == 404) // lobby destroyed, just leave
						{
							m_bPendingHostHasLeft = true;
							// error msg

							// TODO_NGMP: We still want to do this, but we need to send back that it failed and back out, proceeding to lobby crashes because mesh wasn't created
							if (fnCallback != nullptr)
							{
								//fnCallback();
							}

							LeaveCurrentLobby();
							return;
						}

						nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody);

						auto lobbyEntryJSON = jsonObjectRoot["lobby"];

						LobbyEntry lobbyEntry;
						lobbyEntryJSON["id"].get_to(lobbyEntry.lobbyID);
						lobbyEntryJSON["owner"].get_to(lobbyEntry.owner);
						lobbyEntryJSON["name"].get_to(lobbyEntry.name);
						lobbyEntryJSON["map_name"].get_to(lobbyEntry.map_name);
						lobbyEntryJSON["map_path"].get_to(lobbyEntry.map_path);
						lobbyEntryJSON["map_official"].get_to(lobbyEntry.map_official);
						lobbyEntryJSON["current_players"].get_to(lobbyEntry.current_players);
						lobbyEntryJSON["max_players"].get_to(lobbyEntry.max_players);
						lobbyEntryJSON["vanilla_teams"].get_to(lobbyEntry.vanilla_teams);
						lobbyEntryJSON["starting_cash"].get_to(lobbyEntry.starting_cash);
						lobbyEntryJSON["limit_superweapons"].get_to(lobbyEntry.limit_superweapons);
						lobbyEntryJSON["track_stats"].get_to(lobbyEntry.track_stats);
						lobbyEntryJSON["allow_observers"].get_to(lobbyEntry.allow_observers);
						lobbyEntryJSON["passworded"].get_to(lobbyEntry.passworded);
						lobbyEntryJSON["rng_seed"].get_to(lobbyEntry.rng_seed);
						lobbyEntryJSON["max_cam_height"].get_to(lobbyEntry.max_cam_height);
						lobbyEntryJSON["exe_crc"].get_to(lobbyEntry.exe_crc);
						lobbyEntryJSON["ini_crc"].get_to(lobbyEntry.ini_crc);

						// correct map path
						if (lobbyEntry.map_official)
						{
							lobbyEntry.map_path = std::format("Maps\\{}", lobbyEntry.map_path.c_str());
						}
						else
						{
							// TODO_NGMP: This needs to match identically, but why did it change from the base game?
							AsciiString strUserMapDIr = TheMapCache->getUserMapDir(true);
							strUserMapDIr.toLower();

							lobbyEntry.map_path = std::format("{}\\{}", strUserMapDIr.str(), lobbyEntry.map_path.c_str());
						}

						// did the map change? cache that we need to reset and transmit our ready state
						bool bNeedsHasMapUpdate = false;
						if (strcmp(lobbyEntry.map_path.c_str(), TheNGMPGame->getMap().str()) != 0)
						{
							bNeedsHasMapUpdate = true;
						}

						bool bFoundSelfInOld = false;
						bool bFoundSelfInNew = false;
						NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
						int64_t myUserID = pAuthInterface == nullptr ? -1 : pAuthInterface->GetUserID();

						// check for user in old lobby data
						for (LobbyMemberEntry& currentMember : m_CurrentLobby.members)
						{
							if (currentMember.IsHuman())
							{
								// detect local kick
								if (currentMember.user_id == myUserID)
								{
									bFoundSelfInOld = true;
									break;
								}
							}
						}

						for (const auto& memberEntryIter : lobbyEntryJSON["members"])
						{
							LobbyMemberEntry memberEntry;

							memberEntryIter["user_id"].get_to(memberEntry.user_id);
							memberEntryIter["display_name"].get_to(memberEntry.display_name);
							memberEntryIter["ready"].get_to(memberEntry.m_bIsReady);
							memberEntryIter["ip_addr"].get_to(memberEntry.strIPAddress);
							memberEntryIter["port"].get_to(memberEntry.preferredPort);
							memberEntryIter["side"].get_to(memberEntry.side);
							memberEntryIter["color"].get_to(memberEntry.color);
							memberEntryIter["team"].get_to(memberEntry.team);
							memberEntryIter["startpos"].get_to(memberEntry.startpos);
							memberEntryIter["has_map"].get_to(memberEntry.has_map);
							memberEntryIter["slot_index"].get_to(memberEntry.m_SlotIndex);
							memberEntryIter["slot_state"].get_to(memberEntry.m_SlotState);

							lobbyEntry.members.push_back(memberEntry);

							// TODO_NGMP: Much more robust system here
							// TODO_NGMP: If we lose connection to someone in the mesh, who is STILL in otehrs mesh, we need to disconnect or retry
							// TODO_NGMP: handle failure to connect to some users

							bool bMapOwnershipStateChanged = true;

							// is it a new member? connect
							bool bIsNew = true;
							for (LobbyMemberEntry& currentMember : m_CurrentLobby.members)
							{
								if (memberEntry.IsHuman())
								{
									// detect local kick
									if (memberEntry.user_id == myUserID)
									{
										bFoundSelfInNew = true;
									}

									if (currentMember.user_id == memberEntry.user_id)
									{
										// check if the map state changes
										if (currentMember.has_map == memberEntry.has_map)
										{
											bMapOwnershipStateChanged = false;
										}

										bIsNew = false;
										break;
									}
								}
							}
							if (bIsNew)
							{
								// if we're joining as a client (not host), lobby mesh will be null here, but it's ok because the initial creation will sync to everyone
								if (m_pLobbyMesh != nullptr)
								{
									if (memberEntry.IsHuman())
									{
										m_pLobbyMesh->ConnectToSingleUser(memberEntry);
									}
								}
							}

							if (bMapOwnershipStateChanged)
							{
								// changed and the person no longer has the map
								if (!memberEntry.has_map)
								{
									if (m_cbPlayerDoesntHaveMap != nullptr)
									{
										m_cbPlayerDoesntHaveMap(memberEntry);
									}
								}
							}
						}

						
						if (bFoundSelfInOld && !bFoundSelfInNew)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "We were kicked from the lobby...");
							OnKickedFromLobby();
						}

						// disconnect from anyone who is no longer in the lobby
						if (m_pLobbyMesh != nullptr)
						{
							m_pLobbyMesh->SyncConnectionListToLobbyMemberList(lobbyEntry.members);
						}

						// did the host change?
						if (lobbyEntry.owner != m_CurrentLobby.owner)
						{
							m_bHostMigrated = true;
						}

						// store
						m_CurrentLobby = lobbyEntry;

						// update NGMP Game if it exists

						// inform game instance too
						if (TheNGMPGame != nullptr)
						{
							TheNGMPGame->SyncWithLobby(m_CurrentLobby);
							TheNGMPGame->UpdateSlotsFromCurrentLobby();

							if (bNeedsHasMapUpdate)
							{
								UpdateCurrentLobby_HasMap();
							}

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
						// TODO_NGMP: We still want to do this, but we need to send back that it failed and back out, proceeding to lobby crashes because mesh wasn't created
						if (fnCallback != nullptr)
						{
							//fnCallback();
						}
					}
				}
				else
				{
					// TODO_NGMP: We still want to do this, but we need to send back that it failed and back out, proceeding to lobby crashes because mesh wasn't created
					if (fnCallback != nullptr)
					{
						//fnCallback();
					}
				}
		});
	}

	return;
}

void NGMP_OnlineServices_LobbyInterface::JoinLobby(int index, const char* szPassword)
{
	LobbyEntry lobbyInfo = GetLobbyFromIndex(index);
	JoinLobby(lobbyInfo, szPassword);
}

void NGMP_OnlineServices_LobbyInterface::JoinLobby(LobbyEntry lobbyInfo, const char* szPassword)
{
	if (m_bAttemptingToJoinLobby)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Not attempting to join lobby because a join attempt is already in progress");
		return;
	}

	m_bAttemptingToJoinLobby = true;
	m_CurrentLobby = LobbyEntry();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), lobbyInfo.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Joining lobby with id %d", lobbyInfo.lobbyID);

	bool bHasMap = TheMapCache->findMap(AsciiString(lobbyInfo.map_path.c_str()));

	nlohmann::json j;
	j["preferred_port"] = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();
	j["has_map"] = bHasMap;

	if (szPassword != nullptr && strlen(szPassword) > 0)
	{
		j["password"] = szPassword;
	}

	std::string strPostData = j.dump();

	// convert
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
	{
		// reset trying to join
		ResetLobbyTryingToJoin();

		// TODO_NGMP: Dont do extra get here, just return it in the put...
		EJoinLobbyResult JoinResult = EJoinLobbyResult::JoinLobbyResult_JoinFailed;

		if (statusCode == 200 && bSuccess)
		{
			JoinResult = EJoinLobbyResult::JoinLobbyResult_Success;
		}
		else if (statusCode == 401)
		{
			JoinResult = EJoinLobbyResult::JoinLobbyResult_BadPassword;
		}
		else if (statusCode == 406)
		{
			JoinResult = EJoinLobbyResult::JoinLobbyResult_FullRoom;
		}
		// TODO_NGMP: Handle room full error (JoinLobbyResult_FullRoom, can we even get that?

		// no response body from this, just http codes
		if (JoinResult == EJoinLobbyResult::JoinLobbyResult_Success)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Joined lobby");

			m_CurrentLobby = lobbyInfo;

			// failing to parse this isnt really a fatal error, but would be weird
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				JoinLobbyResponse resp = jsonObject.get<JoinLobbyResponse>();

				m_strTURNUsername = resp.turn_username;
				m_strTURNToken = resp.turn_token;
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got TURN username: %s, token: %s", m_strTURNUsername.c_str(), m_strTURNToken.c_str());
			}
			catch (...)
			{

			}

			// for safety
			if (TheNGMPGame != nullptr)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "NGMP_OnlineServices_LobbyInterface::JoinLobby - Safety check - Expected NGMPGame to be null by now, it wasn't so forcefully destroying");
				delete TheNGMPGame;
				TheNGMPGame = nullptr;
			}

			// TODO_NGMP: Cleanup game + dont store 2 ptrs
			if (TheNGMPGame == nullptr)
			{
				TheNGMPGame = new NGMPGame();

				// set in game, this actually means in lobby... not in game play, and is necessary to start the game
				TheNGMPGame->setInGame();

				// set some initial dummy data so the game doesnt balk, we'll do UpdateRoomDataCache immediately below before invoking callback and doing the UI transition, user will never see it
				TheNGMPGame->setStartingCash(TheGlobalData->m_defaultStartingCash);

				// dont need to do these here, updateroomdatacache does it for us
				//TheNGMPGame->SyncWithLobby(m_CurrentLobby);
				//TheNGMPGame->UpdateSlotsFromCurrentLobby();

				// TODO_NGMP: Rest of these
				/*
				TheNGMPGame.setExeCRC(info->getExeCRC());
				TheNGMPGame.setIniCRC(info->getIniCRC());
					
				TheNGMPGame.setHasPassword(info->getHasPassword());
				TheNGMPGame.setGameName(info->getGameName());
				*/
			}

			// we need to get more lobby info before triggering the game callback...
			UpdateRoomDataCache([=]()
				{
					OnJoinedOrCreatedLobby(false, [=]()
						{
							m_bAttemptingToJoinLobby = false;
							NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
							if (pLobbyInterface != nullptr && pLobbyInterface->m_callbackJoinedLobby != nullptr)
							{
								pLobbyInterface->m_callbackJoinedLobby(JoinResult);
							}
						});
				});
		}
		else if (statusCode == 401)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Couldn't join lobby, unauthorized, probably the wrong password");
		}
		else if (statusCode == 404)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to join lobby: Lobby not found");
		}
		else if (statusCode == 406)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to join lobby: Lobby is full");
		}
			

		if (JoinResult != EJoinLobbyResult::JoinLobbyResult_Success)
		{
			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
			if (pLobbyInterface != nullptr && pLobbyInterface->m_callbackJoinedLobby != nullptr)
			{
				pLobbyInterface->m_callbackJoinedLobby(JoinResult);
			}
			m_bAttemptingToJoinLobby = false;
		}

			
	});
}

void NGMP_OnlineServices_LobbyInterface::LeaveCurrentLobby()
{
	// kill mesh
	if (m_pLobbyMesh != nullptr)
	{
		m_pLobbyMesh->Disconnect();
		delete m_pLobbyMesh;
		m_pLobbyMesh = nullptr;
	}

	if (TheNGMPGame != nullptr)
	{
		delete TheNGMPGame;
		TheNGMPGame = nullptr;
	}

	m_timeStartAutoReadyCountdown = -1;
	
	if (!IsInLobby())
	{
		return;
	}

	// leave on service
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", nullptr);

	// reset local data
	ResetCachedRoomData();
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
	std::string turn_username;
	std::string turn_token;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(CreateLobbyResponse, result, lobby_id, turn_username, turn_token)
};

void NGMP_OnlineServices_LobbyInterface::CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, bool bIsOfficial, int initialMaxSize, bool bVanillaTeamsOnly, bool bTrackStats, uint32_t startingCash, bool bPassworded, const char* szPassword, bool bAllowObservers)
{
	m_CurrentLobby = LobbyEntry();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies");
	std::map<std::string, std::string> mapHeaders;

	// convert
	AsciiString strMapName = AsciiString();
	strMapName.translate(strInitialMapName);

	// sanitize map path
	// we need to parse out the map name for custom maps... its an absolute path
	// it's safe to just get the file name, dir name and file name MUST be the same. Game enforces this
	AsciiString sanitizedMapPath = strInitialMapPath;
	if (sanitizedMapPath.reverseFind('\\'))
	{
		sanitizedMapPath = sanitizedMapPath.reverseFind('\\') + 1;
	}

	nlohmann::json j;
	j["name"] = to_utf8(strLobbyName.str());
	j["map_name"] = strMapName.str();
	j["map_path"] = sanitizedMapPath.str();
	j["map_official"] = bIsOfficial;
	j["max_players"] = initialMaxSize;
	j["preferred_port"] = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();
	j["vanilla_teams"] = bVanillaTeamsOnly;
	j["track_stats"] = bTrackStats;
	j["starting_cash"] = startingCash;
	j["passworded"] = bPassworded;
	j["password"] = szPassword;
	j["allow_observers"] = bAllowObservers;
	j["exe_crc"] = TheGlobalData->m_exeCRC;
	j["ini_crc"] = TheGlobalData->m_iniCRC;
	j["max_cam_height"] = NGMP_OnlineServicesManager::Settings.Camera_GetMaxHeight_WhenLobbyHost();

	std::string strPostData = j.dump();

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
				NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				CreateLobbyResponse resp = jsonObject.get<CreateLobbyResponse>();

				m_strTURNUsername = resp.turn_username;
				m_strTURNToken = resp.turn_token;
				NetworkLog(ELogVerbosity::LOG_DEBUG, "Got TURN username: %s, token: %s", m_strTURNUsername.c_str(), m_strTURNToken.c_str());


				if (resp.result == ECreateLobbyResponseResult::SUCCEEDED)
				{
					// for safety
					if (TheNGMPGame != nullptr)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "NGMP_OnlineServices_LobbyInterface::JoinLobby - Safety check - Expected NGMPGame to be null by now, it wasn't so forcefully destroying");
						delete TheNGMPGame;
						TheNGMPGame = nullptr;
					}


					// TODO_NGMP: Cleanup game + dont store 2 ptrs
					if (TheNGMPGame == nullptr)
					{
						TheNGMPGame = new NGMPGame();
					}

					// reset before copy
					pLobbyInterface->ResetCachedRoomData();

					// TODO: Do we need more info here? we kick off a lobby GET immediately, maybe that should be the response to creating

					// store the basic info (lobby id), we will immediately kick off a full get				
					m_CurrentLobby.lobbyID = resp.lobby_id;
					m_CurrentLobby.owner = pAuthInterface->GetUserID();

					AsciiString strName = AsciiString();

					m_CurrentLobby.name = to_utf8(strLobbyName.str());
					m_CurrentLobby.map_name = std::string(strMapName.str());
					m_CurrentLobby.map_path = std::string(sanitizedMapPath.str());
					m_CurrentLobby.current_players = 1;
					m_CurrentLobby.max_players = initialMaxSize;
					m_CurrentLobby.passworded = bPassworded;
					m_CurrentLobby.password = std::string(szPassword);

					LobbyMemberEntry me;

					me.user_id = m_CurrentLobby.owner;
					me.display_name = pAuthInterface->GetDisplayName();
					me.m_bIsReady = true; // host is always ready
					me.strIPAddress = "127.0.0.1"; // TODO_NGMP: use localhost for non-host players too that are local...
					me.preferredPort = NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort();

					m_CurrentLobby.members.push_back(me);

					// set in game, this actually means in lobby... not in game play, and is necessary to start the game
					TheNGMPGame->setInGame();

					TheNGMPGame->SyncWithLobby(m_CurrentLobby);
					TheNGMPGame->UpdateSlotsFromCurrentLobby();

					// we always need to get the enc key etc
					pLobbyInterface->OnJoinedOrCreatedLobby(false, [=]()
						{
							// TODO_NGMP: Impl
							pLobbyInterface->InvokeCreateLobbyCallback(resp.result == ECreateLobbyResponseResult::SUCCEEDED);

							// Set our properties
							pLobbyInterface->ApplyLocalUserPropertiesToCurrentNetworkRoom();
						});
				}
				else
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to create lobby!\n");

					// TODO_NGMP: Impl
					pLobbyInterface->InvokeCreateLobbyCallback(resp.result == ECreateLobbyResponseResult::SUCCEEDED);

					// Set our properties
					pLobbyInterface->ApplyLocalUserPropertiesToCurrentNetworkRoom();
				}

				
			}
			catch (...)
			{

			}

		});
	return;
}

void NGMP_OnlineServices_LobbyInterface::OnJoinedOrCreatedLobby(bool bAlreadyUpdatedDetails, std::function<void(void)> fnCallback)
{
	m_bMarkedGameAsFinished = false;

	// reset timer
	m_timeStartAutoReadyCountdown = -1;

	// TODO_NGMP: We need this on create, but this is a double call on join because we already got this info
	// must be done in a callback, this is an async function
	if (!bAlreadyUpdatedDetails)
	{
		UpdateRoomDataCache([=]()
			{
				// join the network mesh too
				if (m_pLobbyMesh == nullptr)
				{
					m_pLobbyMesh = new NetworkMesh();
					m_pLobbyMesh->ConnectToMesh(m_CurrentLobby);
				}

				fnCallback();
			});
	}


}