#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"
#include "GameClient/MapUtil.h"
#include "GameLogic/GameLogic.h"

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
	UNUSED_1 = 9,
	UNUSED_2 = 10,
	HOST_ACTION_KICK_USER = 11,
	HOST_ACTION_SET_SLOT_STATE = 12,
	AI_SIDE = 13,
	AI_COLOR = 14,
	AI_TEAM = 15,
	AI_START_POS = 16,
	MAX_CAMERA_HEIGHT = 17,
	JOINABILITY = 18,
	HOST_ACTION_BULK_SLOT_UPDATE = 19
};

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_Map(AsciiString strMap, AsciiString strMapPath, bool bIsOfficial, int newMaxPlayers)
{
	// reset autostart if host changes anything (because ready flag will reset too)
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
	ClearAutoReadyCountdown();
#endif

	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
	ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
		strInform.format(L"Camera height: The host set the limit to %lu.", maxCameraHeight);

		SendAnnouncementMessageToCurrentLobby(strInform, true);

		// reset autostart if host changes anything (because ready flag will reset too)
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
        ClearAutoReadyCountdown();
#endif
		if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
			TheNGMPGame->StopCountdown();

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


void NGMP_OnlineServices_LobbyInterface::SetJoinability(ELobbyJoinability joinabilityFlag)
{
    if (IsHost())
    {
        UnicodeString strInform(joinabilityFlag == ELobbyJoinability::LobbyJoinability_FriendsOnly ? L"Lobby access: Friends only." : L"Lobby access: Anyone can join.");

		SendAnnouncementMessageToCurrentLobby(strInform, false);

        std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
        std::map<std::string, std::string> mapHeaders;

        nlohmann::json j;
        j["field"] = ELobbyUpdateField::JOINABILITY;
		j["joinability"] = (int)joinabilityFlag;
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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
    ClearAutoReadyCountdown();
#endif
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

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
			msg.format(L"Lobby: '%s' was removed by the host.", name.str());;
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
			UnicodeString msg = UnicodeString(L"Ready check: All players are now marked ready.");
			SendAnnouncementMessageToCurrentLobby(msg, true);
		});
}

void NGMP_OnlineServices_LobbyInterface::UpdateCurrentLobby_BulkSlotUpdate(NGMPGame* game)
{
	ClearAutoReadyCountdown();
	if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
		TheNGMPGame->StopCountdown();

	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), m_CurrentLobby.lobbyID);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["field"] = ELobbyUpdateField::HOST_ACTION_BULK_SLOT_UPDATE;

	nlohmann::json slotsArray = nlohmann::json::array();
	for (int i = 0; i < MAX_SLOTS; ++i)
	{
		const GameSlot *slot = game->getConstSlot(i);
		if (slot && slot->isOccupied())
		{
			nlohmann::json slotEntry;
			slotEntry["slot_index"] = i;
			slotEntry["side"] = slot->getPlayerTemplate();
			slotEntry["color"] = slot->getColor();
			slotEntry["start_pos"] = slot->getStartPos();
			slotEntry["team"] = slot->getTeamNumber();
			slotsArray.push_back(slotEntry);
		}
	}
	j["slots"] = slotsArray;
	std::string strPostData = j.dump();

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			if (!bSuccess || statusCode < 200 || statusCode >= 300)
			{
				DEBUG_LOG(("UpdateCurrentLobby_BulkSlotUpdate failed: success=%d, status=%d", bSuccess, statusCode));
			}
		});
}

void NGMP_OnlineServices_LobbyInterface::SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode, bool bIsAction)
{
	std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
	if (pWS != nullptr)
	{
		pWS->SendData_LobbyChatMessage(strChatMsgUnicode, bIsAction, false, false);
	}
}

// TODO_NGMP: Just send a separate packet for each announce, more efficient and less hacky
void NGMP_OnlineServices_LobbyInterface::SendAnnouncementMessageToCurrentLobby(UnicodeString& strAnnouncementMsgUnicode, bool bShowToHost)
{
	std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
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
	const bool lobbyListWasDirty = m_bLobbyListDirty.exchange(false);
	if (m_bSearchInProgress.exchange(true))
	{
		if (lobbyListWasDirty)
		{
			m_bLobbyListDirty.store(true);
		}
		return;
	}

	// A notification received after the exchange above stays set and triggers a later refresh.
	m_fnCallbackSearchForLobbiesComplete = onCompleteCallback;
	if (onStartCallback != nullptr)
	{
		onStartCallback();
	}

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			bool bSearchSucceeded = false;
			if (bSuccess && statusCode == 200)
			{
				try
				{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);
				std::vector<LobbyEntry> parsedLobbies;

				std::vector<int> vecLatencies;
				std::map<int64_t, int> mapPlayerLatencies;

				jsonObject["latencies"].get_to(vecLatencies);

				// player latencies
				for (const auto& playerLatencyEntryIter : jsonObject["playerlatencies"])
				{
					int64_t user_id = -1;
					int latency = -1;

					playerLatencyEntryIter["user_id"].get_to(user_id);
					playerLatencyEntryIter["latency"].get_to(latency);

					if (user_id != -1 && latency != -1)
					{
						mapPlayerLatencies[user_id] = latency;
					}
				}

				int latencyIndex = 0;
			for (const auto& lobbyEntryIter : jsonObject["lobbies"])
			{
				LobbyEntry lobbyEntry;
				lobbyEntryIter["LobbyID"].get_to(lobbyEntry.lobbyID);
				lobbyEntryIter["Owner"].get_to(lobbyEntry.owner);
				lobbyEntryIter["Name"].get_to(lobbyEntry.name);
				lobbyEntryIter["MapName"].get_to(lobbyEntry.map_name);
				lobbyEntryIter["MapPath"].get_to(lobbyEntry.map_path);
				lobbyEntryIter["IsMapOfficial"].get_to(lobbyEntry.map_official);
				lobbyEntryIter["NumCurrentPlayers"].get_to(lobbyEntry.current_players);
				lobbyEntryIter["MaxPlayers"].get_to(lobbyEntry.max_players);
				lobbyEntryIter["IsVanillaTeamsOnly"].get_to(lobbyEntry.vanilla_teams);
				lobbyEntryIter["StartingCash"].get_to(lobbyEntry.starting_cash);
				lobbyEntryIter["IsLimitSuperweapons"].get_to(lobbyEntry.limit_superweapons);
				lobbyEntryIter["IsTrackingStats"].get_to(lobbyEntry.track_stats);
				lobbyEntryIter["IsPassworded"].get_to(lobbyEntry.passworded);
				lobbyEntryIter["AllowObservers"].get_to(lobbyEntry.allow_observers);
				lobbyEntryIter["MaximumCameraHeight"].get_to(lobbyEntry.max_cam_height);
				lobbyEntryIter["ExeCRC"].get_to(lobbyEntry.exe_crc);
				lobbyEntryIter["IniCRC"].get_to(lobbyEntry.ini_crc);
				lobbyEntryIter["MatchID"].get_to(lobbyEntry.match_id);
				lobbyEntryIter["LobbyType"].get_to(lobbyEntry.lobby_type);
				lobbyEntryIter["Region"].get_to(lobbyEntry.region);

				// attach latency
				if (latencyIndex < vecLatencies.size())
				{
					lobbyEntry.latency = vecLatencies[latencyIndex];
				}
				else
				{
					// dummy value
					lobbyEntry.latency = 9001;
				}
				++latencyIndex;

				// correct map path
				if (lobbyEntry.map_official)
				{
					lobbyEntry.map_path = std::format("maps\\{}", lobbyEntry.map_path.c_str());
				}
				else
				{
					lobbyEntry.map_path = std::format("{}\\{}", TheMapCache->getUserMapDir(true).str(), lobbyEntry.map_path.c_str());
				}

				// NOTE: These fields won't be present becauase they're private properties
				//memberEntryIter["enc_key"].get_to(strEncKey);

				for (const auto& memberEntryIter : lobbyEntryIter["Members"])
				{
					LobbyMemberEntry memberEntry;

					memberEntryIter["UserID"].get_to(memberEntry.user_id);
					memberEntryIter["DisplayName"].get_to(memberEntry.display_name);
					memberEntryIter["IsReady"].get_to(memberEntry.m_bIsReady);
					memberEntryIter["SlotIndex"].get_to(memberEntry.m_SlotIndex);
					memberEntryIter["SlotState"].get_to(memberEntry.m_SlotState);
					memberEntryIter["Region"].get_to(memberEntry.region);

					// store latency
					if (mapPlayerLatencies.contains(memberEntry.user_id))
					{
						memberEntry.latency = mapPlayerLatencies[memberEntry.user_id];
					}
					else
					{
						memberEntry.latency = 0;
					}

					lobbyEntry.members.push_back(memberEntry);
				}

				parsedLobbies.push_back(std::move(lobbyEntry));
			}
					m_vecLobbies = std::move(parsedLobbies);
					bSearchSucceeded = true;
				}
				catch (...)
				{

				}
			}

			if (!bSearchSucceeded)
			{
				m_bLobbyListDirty.store(true);
			}

			const std::function<void(std::vector<LobbyEntry>)> completionCallback = m_fnCallbackSearchForLobbiesComplete;
			const std::vector<LobbyEntry> lobbies = m_vecLobbies;
			m_bSearchInProgress.store(false);
			if (completionCallback != nullptr)
			{
				completionCallback(lobbies);
			}
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


void NGMP_OnlineServices_LobbyInterface::Tick()
{
	// cheats
#if defined(GENERALS_ONLINE_ALL_SCIENCES)
	static bool GrantAllSciences = true;
	if (GrantAllSciences)
	{
		//GrantAllSciences = false;
		Player* player = ThePlayerList->getLocalPlayer();
		if (player)
		{
			// cheese festival: do NOT imitate this code. it is for debug purposes only.
			std::vector<AsciiString> v = TheScienceStore->friend_getScienceNames();
			for (int i = 0; i < v.size(); ++i)
			{
				ScienceType st = TheScienceStore->getScienceFromInternalName(v[i]);
				if (st != SCIENCE_INVALID && TheScienceStore->isScienceGrantable(st))
				{
					player->grantScience(st);
					TheInGameUI->message(UnicodeString(L"Granting all sciences!"));
				}
			}
		}

	}
#endif

#if defined(GENERALS_ONLINE_MAX_SCIENCES_POINTS)
	static bool GrantSciencePoints = false;
	if (GrantSciencePoints)
	{
		GrantSciencePoints = false;
		Player* player2 = ThePlayerList->getLocalPlayer();
		if (player2 && player2->isPlayerActive())
		{
			player2->setRankLevel(5);
			player2->addSciencePurchasePoints(100);
			TheInGameUI->message(UnicodeString(L"Adding a SciencePurchasePoint and setting to max general level"));
		}
	}
#endif

	if (m_pLobbyMesh != nullptr)
	{
		m_pLobbyMesh->Flush();
		m_pLobbyMesh->Tick();
	}

	// TODO_NGMP: Do we still need this safety measure?
	if (IsInLobby())
	{
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		if ((currTime - m_lastForceRefresh) > 5000)
		{
			//UpdateRoomDataCache();
			m_lastForceRefresh = currTime;
		}

		// do we have a pending start?
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
		if (IsHost())
		{
			if (m_timeStartAutoReadyCountdown > 0)
			{
				if ((currTime - m_timeStartAutoReadyCountdown) > 30000)
				{
					// TODO_NGMP: Don't do this clientside...
					UpdateCurrentLobby_ForceReady();
					ClearAutoReadyCountdown();
					if (TheNGMPGame && TheNGMPGame->IsCountdownStarted())
						TheNGMPGame->StopCountdown();
				}
			}
		}
#endif
	}

	// real time outcome upload
	if (TheVictoryConditions != nullptr && TheNGMPGame != nullptr && TheNGMPGame->isGameInProgress())
	{
		// is the game over for us?
		if (TheVictoryConditions->isLocalAlliedDefeat() || TheVictoryConditions->isLocalDefeat() || TheVictoryConditions->isLocalAlliedVictory())
		{
			NGMP_OnlineServices_StatsInterface* pStatsInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_StatsInterface>();
			if (pStatsInterface != nullptr)
			{
				Player* localPlayer = ThePlayerList->getLocalPlayer();

				if (localPlayer != nullptr)
				{
					if (!TheNGMPGame->HasCommittedOutcome())
					{
						TheNGMPGame->SetHasCommittedOutcome();
						pStatsInterface->CommitMyOutcome(localPlayer->getScoreKeeper(), TheVictoryConditions->isLocalAlliedVictory());
					}
				}
			}
		}
	}
}

void NGMP_OnlineServices_LobbyInterface::ApplyLocalUserPropertiesToCurrentNetworkRoom()
{
	// TODO_NGMP: Better detection of this, dont update always
	// are we ready?

	std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();;
	if (pWS != nullptr)
	{
		if (IsHost())
		{
			pWS->SendData_MarkReady(true);
		}
		else
		{
			if (TheNGMPGame == nullptr)
				return;

			GameSlot* pLocalSlot = TheNGMPGame->getSlot(TheNGMPGame->getLocalSlotNum());

			if (pLocalSlot != nullptr)
			{
				pWS->SendData_MarkReady(pLocalSlot->isAccepted());
			}
		}
	}
}

void NGMP_OnlineServices_LobbyInterface::UpdateRoomDataCache(std::function<void(bool)> fnCallback)
{
	// refresh lobby
	if (m_CurrentLobby.lobbyID != -1 && TheNGMPGame != nullptr)
	{
		const int64_t requestedLobbyID = m_CurrentLobby.lobbyID;
		std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), requestedLobbyID);
		std::map<std::string, std::string> mapHeaders;

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// safety, lobby could've been torn down by the time we get our response
				if (m_CurrentLobby.lobbyID == requestedLobbyID && TheNGMPGame != nullptr)
				{
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
								fnCallback(false);
							}

							LeaveCurrentLobby();
							return;
						}

						nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody);

						NetworkLog(ELogVerbosity::LOG_DEBUG, "LOBBY JSON");
						NetworkLog(ELogVerbosity::LOG_DEBUG, strBody.c_str());

						auto lobbyEntryIter = jsonObjectRoot["lobby"];

						LobbyEntry lobbyEntry;
						lobbyEntryIter["LobbyID"].get_to(lobbyEntry.lobbyID);
						lobbyEntryIter["Owner"].get_to(lobbyEntry.owner);
						lobbyEntryIter["Name"].get_to(lobbyEntry.name);
						lobbyEntryIter["MapName"].get_to(lobbyEntry.map_name);
						lobbyEntryIter["MapPath"].get_to(lobbyEntry.map_path);
						lobbyEntryIter["IsMapOfficial"].get_to(lobbyEntry.map_official);
						lobbyEntryIter["NumCurrentPlayers"].get_to(lobbyEntry.current_players);
						lobbyEntryIter["MaxPlayers"].get_to(lobbyEntry.max_players);
						lobbyEntryIter["IsVanillaTeamsOnly"].get_to(lobbyEntry.vanilla_teams);
						lobbyEntryIter["RNGSeed"].get_to(lobbyEntry.rng_seed);
						lobbyEntryIter["StartingCash"].get_to(lobbyEntry.starting_cash);
						lobbyEntryIter["IsLimitSuperweapons"].get_to(lobbyEntry.limit_superweapons);
						lobbyEntryIter["IsTrackingStats"].get_to(lobbyEntry.track_stats);
						lobbyEntryIter["IsPassworded"].get_to(lobbyEntry.passworded);
						lobbyEntryIter["AllowObservers"].get_to(lobbyEntry.allow_observers);
						lobbyEntryIter["MaximumCameraHeight"].get_to(lobbyEntry.max_cam_height);
						lobbyEntryIter["ExeCRC"].get_to(lobbyEntry.exe_crc);
						lobbyEntryIter["IniCRC"].get_to(lobbyEntry.ini_crc);
						lobbyEntryIter["MatchID"].get_to(lobbyEntry.match_id);
						lobbyEntryIter["LobbyType"].get_to(lobbyEntry.lobby_type);
						lobbyEntryIter["Region"].get_to(lobbyEntry.region);

						if (lobbyEntry.lobby_type == ELobbyType::QuickMatch)
						{
							TheNGMPGame->markGameAsQM();
						}

						// store, we'll need it later and lobby obj gets destroyed on leave
						m_CurrentMatchID = lobbyEntry.match_id;

						// correct map path
						if (lobbyEntry.map_official)
						{
							lobbyEntry.map_path = std::format("maps\\{}", lobbyEntry.map_path.c_str());
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
						if (strcasecmp(lobbyEntry.map_path.c_str(), TheNGMPGame->getMap().str()) != 0)
						{
							bNeedsHasMapUpdate = true;
						}

						// dont let the service be authoritative during gameplay over play list, the game host handles connections at this point
						// we do care about everything else up until members list though, because we need things like matchID which is determined as the game transitions to in progress
						if (TheNGMPGame->isGameInProgress() && !TheGameLogic->IsLoadScreenActive())
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "Ignoring lobby members update request during gameplay.");

							// retain the old members list
							lobbyEntry.members = m_CurrentLobby.members;

							// still store the lobby though
							m_CurrentLobby = lobbyEntry;
						}
						else
						{
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

							for (const auto& memberEntryIter : lobbyEntryIter["Members"])
							{
								LobbyMemberEntry memberEntry;

								memberEntryIter["UserID"].get_to(memberEntry.user_id);
								memberEntryIter["DisplayName"].get_to(memberEntry.display_name);
								memberEntryIter["IsReady"].get_to(memberEntry.m_bIsReady);
								memberEntryIter["Port"].get_to(memberEntry.preferredPort);
								memberEntryIter["Side"].get_to(memberEntry.side);
								memberEntryIter["Color"].get_to(memberEntry.color);
								memberEntryIter["Team"].get_to(memberEntry.team);
								memberEntryIter["StartingPosition"].get_to(memberEntry.startpos);
								memberEntryIter["HasMap"].get_to(memberEntry.has_map);
								memberEntryIter["SlotState"].get_to(memberEntry.m_SlotState);
								memberEntryIter["SlotIndex"].get_to(memberEntry.m_SlotIndex);
								memberEntryIter["Region"].get_to(memberEntry.region);
								memberEntryIter["MiddlewareUserID"].get_to(memberEntry.middlewareUserID);

								lobbyEntry.members.push_back(memberEntry);

								// TODO_NGMP: Much more robust system here
								// TODO_NGMP: If we lose connection to someone in the mesh, who is STILL in otehrs mesh, we need to disconnect or retry
								// TODO_NGMP: handle failure to connect to some users

								bool bMapOwnershipStateChanged = true;
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

											break;
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

							// did the host change?
							if (lobbyEntry.owner != m_CurrentLobby.owner)
							{
								// no host migration in QM since it doesn't even have a lobby
								if (lobbyEntry.lobby_type != ELobbyType::QuickMatch)
								{
									m_bHostMigrated = true;
								}
							}

							// stop countdown if player count actually decreased and we're the host
							if (IsHost()
								&& TheNGMPGame != nullptr
								&& TheNGMPGame->IsCountdownStarted()
								&& lobbyEntry.current_players < m_CurrentLobby.current_players)
							{
								TheNGMPGame->StopCountdown();
							}

							// store
							m_CurrentLobby = lobbyEntry;

							// inform game instance too
							if (TheNGMPGame != nullptr)
							{
								TheNGMPGame->SyncWithLobby(m_CurrentLobby);
								TheNGMPGame->UpdateSlotsFromCurrentLobby();

								if (bNeedsHasMapUpdate)
								{
									UpdateCurrentLobby_HasMap();
								}

								std::scoped_lock<std::mutex> lock(m_rosterCallbackMutex);
								if (m_RosterNeedsRefreshCallback != nullptr)
								{
									m_RosterNeedsRefreshCallback();
								}
							}
						}

						if (fnCallback != nullptr)
						{
							fnCallback(bSuccess);
						}
					}
					catch (...)
					{
						// TODO_NGMP: We still want to do this, but we need to send back that it failed and back out, proceeding to lobby crashes because mesh wasn't created
						if (fnCallback != nullptr)
						{
							fnCallback(false);
						}
					}
				}
				else
				{
					// TODO_NGMP: We still want to do this, but we need to send back that it failed and back out, proceeding to lobby crashes because mesh wasn't created
					if (fnCallback != nullptr)
					{
						fnCallback(false);
					}
				}
		});
	}

	return;
}

void NGMP_OnlineServices_LobbyInterface::JoinLobby(LobbyEntry lobbyInfo, std::string strPassword)
{
	if (m_bAttemptingToJoinLobby)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "Not attempting to join lobby because a join attempt is already in progress");
		return;
	}

	const uint64_t lobbyJoinGeneration = ++m_LobbyJoinGeneration;

    AnticheatPlugInterface::EndSession();

	m_bAttemptingToJoinLobby = true;
	m_CurrentLobby = LobbyEntry();

	NGMP_OnlineServicesManager::GetInstance()->GetAndParseServiceConfig([=]()
		{
			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
			if (pLobbyInterface == nullptr || pLobbyInterface != this || pLobbyInterface->m_LobbyJoinGeneration.load() != lobbyJoinGeneration)
			{
				return;
			}

			std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"), lobbyInfo.lobbyID);
			std::map<std::string, std::string> mapHeaders;

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Joining lobby with id %d", lobbyInfo.lobbyID);

			bool bHasMap = TheMapCache->findMap(AsciiString(lobbyInfo.map_path.c_str()));

			nlohmann::json j;

			// TODO_NGMP: Remove this and just hardcode it or provide from service
			j["preferred_port"] = 0;

			j["anticheat_id"] = AnticheatPlugInterface::GetAnticheatIdentifier();
			j["has_map"] = bHasMap;

			if (!strPassword.empty())
			{
				j["password"] = strPassword.c_str();
			}

			std::string strPostData = j.dump();

			// create our mesh
			if (m_pLobbyMesh == nullptr)
			{
				m_pLobbyMesh = new NetworkMesh();
			}

			// convert
			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
					if (pLobbyInterface == nullptr || pLobbyInterface != this || pLobbyInterface->m_LobbyJoinGeneration.load() != lobbyJoinGeneration)
					{
						return;
					}

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
                    else if (statusCode == 417)
                    {
                        JoinResult = EJoinLobbyResult::JoinLobbyResult_AnticheatMismatch;
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

							if (lobbyInfo.lobby_type == ELobbyType::QuickMatch)
							{
								TheNGMPGame->markGameAsQM();
							}

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

						OnJoinedOrCreatedLobby(false, [=](bool bSuccess)
							{
								NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
								if (pLobbyInterface == nullptr || pLobbyInterface != this || pLobbyInterface->m_LobbyJoinGeneration.load() != lobbyJoinGeneration)
								{
									return;
								}

								pLobbyInterface->m_bAttemptingToJoinLobby = false;
								if (pLobbyInterface->m_callbackJoinedLobby != nullptr)
								{
									pLobbyInterface->m_callbackJoinedLobby(bSuccess ? EJoinLobbyResult::JoinLobbyResult_Success : EJoinLobbyResult::JoinLobbyResult_JoinFailed);
								}
							});

						// get latest lobby info immediately
						UpdateRoomDataCache([=](bool bSuccess)
							{

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
                    else if (statusCode == 417)
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to join lobby: Anticheat mismatch");
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
		});
}

void NGMP_OnlineServices_LobbyInterface::LeaveCurrentLobby()
{
	// reset host migration flags
	ResetHostMigrationFlags();

	AnticheatPlugInterface::EndSession();

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

#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
	m_timeStartAutoReadyCountdown = -1;
#endif
	
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

void NGMP_OnlineServices_LobbyInterface::ResetForMatchmakingRequeue()
{
	// The service has already removed us from the failed temporary lobby. Tear down only
	// local state here; sending the normal DELETE would cancel the server-side requeue.
	++m_LobbyJoinGeneration;
	ResetHostMigrationFlags();
	AnticheatPlugInterface::EndSession();

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

#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
	m_timeStartAutoReadyCountdown = -1;
#endif

	m_bAttemptingToJoinLobby = false;
	ResetLobbyTryingToJoin();
	ResetCachedRoomData();
}


LobbyEntry NGMP_OnlineServices_LobbyInterface::GetLobbyFromID(int64_t lobbyID)
{
	// TODO_NGMP: Optimize for lookup
	for (LobbyEntry& lobbyEntry : m_vecLobbies)
	{
		if (lobbyEntry.lobbyID == lobbyID)
		{
			return lobbyEntry;
		}
	}

	return LobbyEntry();
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

void NGMP_OnlineServices_LobbyInterface::CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, bool bIsOfficial, int initialMaxSize, bool bVanillaTeamsOnly, bool bTrackStats, uint32_t startingCash, bool bPassworded, std::string strPassword, bool bAllowObservers)
{
	AnticheatPlugInterface::EndSession();

	NGMP_OnlineServicesManager::GetInstance()->GetAndParseServiceConfig([=]()
		{
			m_CurrentLobby = LobbyEntry();
			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Lobbies");
			std::map<std::string, std::string> mapHeaders;

			// convert
			std::string strMapName = to_utf8(strInitialMapName.str());

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
			j["map_name"] = strMapName;
			j["map_path"] = sanitizedMapPath.str();
			j["map_official"] = bIsOfficial;
			j["max_players"] = initialMaxSize;

			// TODO_NGMP: Remove this and just hardcode it or provide from service
			j["preferred_port"] = 0;

			j["vanilla_teams"] = bVanillaTeamsOnly;
			j["track_stats"] = bTrackStats;
			j["starting_cash"] = startingCash;
			j["passworded"] = bPassworded;
			j["password"] = strPassword;
			j["allow_observers"] = bAllowObservers;
			j["exe_crc"] = TheGlobalData->m_exeCRC;
			j["ini_crc"] = TheGlobalData->m_iniCRC;
			j["max_cam_height"] = NGMP_OnlineServicesManager::Settings.Camera_GetMaxHeight_WhenLobbyHost();
			j["anticheat_id"] = AnticheatPlugInterface::GetAnticheatIdentifier();

			std::string strPostData = j.dump();

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					try
					{
						NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
						NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

						if (pAuthInterface == nullptr || pLobbyInterface == nullptr)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] CreateLobby callback: required interface is null, aborting");
							return;
						}

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

								// dont need to mark as QM here, service marks it for us
							}

							// reset before copy
							pLobbyInterface->ResetCachedRoomData();

							// TODO: Do we need more info here? we kick off a lobby GET immediately, maybe that should be the response to creating

							// store the basic info (lobby id), we will immediately kick off a full get				
							m_CurrentLobby.lobbyID = resp.lobby_id;
							m_CurrentLobby.owner = pAuthInterface->GetUserID();

							AsciiString strName = AsciiString();

							m_CurrentLobby.name = to_utf8(strLobbyName.str());
							m_CurrentLobby.map_name = std::string(strMapName);
							m_CurrentLobby.map_path = std::string(sanitizedMapPath.str());
							m_CurrentLobby.current_players = 1;
							m_CurrentLobby.max_players = initialMaxSize;
							m_CurrentLobby.passworded = bPassworded;
							m_CurrentLobby.password = strPassword;

							LobbyMemberEntry me;

							me.user_id = m_CurrentLobby.owner;
							me.display_name = pAuthInterface->GetDisplayName();
							me.m_bIsReady = true; // host is always ready

							// TODO_NGMP: Remove this and just hardcode it or provide from service
							me.preferredPort = 0;

							m_CurrentLobby.members.push_back(me);

							// set in game, this actually means in lobby... not in game play, and is necessary to start the game
							TheNGMPGame->setInGame();

							TheNGMPGame->SyncWithLobby(m_CurrentLobby);
							TheNGMPGame->UpdateSlotsFromCurrentLobby();

							// we always need to get the enc key etc
							pLobbyInterface->OnJoinedOrCreatedLobby(false, [=](bool bSuccess)
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

							pLobbyInterface->InvokeCreateLobbyCallback(resp.result == ECreateLobbyResponseResult::SUCCEEDED);
						}


					}
					catch (...)
					{

					}

				});
		});
}

void NGMP_OnlineServices_LobbyInterface::OnJoinedOrCreatedLobby(bool bAlreadyUpdatedDetails, std::function<void(bool)> fnCallback)
{
	// begin AC
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Begin Session 0");

	AnticheatPlugInterface::BeginSession();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Begin Session End");

	// join the network mesh too
	if (m_pLobbyMesh == nullptr)
	{
		m_pLobbyMesh = new NetworkMesh();
	}

	m_bMarkedGameAsFinished = false;

	// reset timer
#if !defined(GENERALS_ONLINE_DISABLE_AUTO_ACCEPT)
	m_timeStartAutoReadyCountdown = -1;
#endif

	// TODO_NGMP: We need this on create, but this is a double call on join because we already got this info
	// must be done in a callback, this is an async function
	if (!bAlreadyUpdatedDetails)
	{
		UpdateRoomDataCache([=](bool bSuccess)
			{
				fnCallback(bSuccess);
			});
	}


}
