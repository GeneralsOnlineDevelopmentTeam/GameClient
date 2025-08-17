#pragma once

#include "NGMP_include.h"
#include "NetworkMesh.h"
#include "../GameSpy/PeerDefs.h"
#include "OnlineServices_Init.h"

extern Color PlayerSlotColors[MAX_SLOTS];

enum class EChatMessageType
{
	CHAT_MESSAGE_TYPE_NETWORK_ROOM,
	CHAT_MESSAGE_TYPE_LOBBY
};
static Color DetermineColorForChatMessage(EChatMessageType chatMessageType, Bool isPublic, Bool isAction, int lobbySlot = -1)
{
	Color style;

	// TODO_NGMP: Support owner chat again
	Bool isOwner = false;

	if (isPublic && isAction)
	{
		style = (isOwner) ? GameSpyColor[GSCOLOR_CHAT_OWNER_EMOTE] : GameSpyColor[GSCOLOR_CHAT_EMOTE];
	}
	else if (isPublic)
	{
		// use lobby colors
		if (chatMessageType == EChatMessageType::CHAT_MESSAGE_TYPE_LOBBY)
		{
			if (lobbySlot == -1)
			{
				return GameMakeColor(255, 255, 255, 255);
			}
			else
			{
				style = PlayerSlotColors[lobbySlot];
			}
		}
		else
		{
			style = (isOwner) ? GameSpyColor[GSCOLOR_CHAT_OWNER] : GameSpyColor[GSCOLOR_CHAT_NORMAL];
		}
	}
	else if (isAction)
	{
		style = (isOwner) ? GameSpyColor[GSCOLOR_CHAT_PRIVATE_OWNER_EMOTE] : GameSpyColor[GSCOLOR_CHAT_PRIVATE_EMOTE];
	}
	else
	{
		style = (isOwner) ? GameSpyColor[GSCOLOR_CHAT_PRIVATE_OWNER] : GameSpyColor[GSCOLOR_CHAT_PRIVATE];
	}

	// filters language
//  if( TheGlobalData->m_languageFilterPref )
//  {
	//TheLanguageFilter->filterLine(msg);
	//	}

	return style;
}

struct NGMP_RoomInfo
{
	int numMembers;
	int maxMembers;
};

class NetworkRoomMember : public NetworkMemberBase
{

};

class NGMP_OnlineServices_RoomsInterface
{
public:
	NGMP_OnlineServices_RoomsInterface();

	void GetRoomList(std::function<void(void)> cb);

	std::function<void()> m_PendingRoomJoinCompleteCallback = nullptr;
	void JoinRoom(int roomIndex, std::function<void()> onStartCallback, std::function<void()> onCompleteCallback);

	void LeaveRoom()
	{
		WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();
		if (pWS != nullptr)
		{
			pWS->SendData_LeaveNetworkRoom();
		}
	}

	std::function<void(UnicodeString strMessage, Color color)> m_OnChatCallback = nullptr;
	void RegisterForChatCallback(std::function<void(UnicodeString strMessage, Color color)> cb)
	{
		m_OnChatCallback = cb;
	}

	void DeregisterForChatCallback()
	{
		m_OnChatCallback = nullptr;
	}

	std::function<void()> m_RosterNeedsRefreshCallback = nullptr;
	void RegisterForRosterNeedsRefreshCallback(std::function<void()> cb)
	{
		m_RosterNeedsRefreshCallback = cb;
	}

	void DeregisterForRosterNeedsRefreshCallback()
	{
		m_RosterNeedsRefreshCallback = nullptr;
	}

	NetworkRoomMember* GetRoomMemberFromIndex(int index)
	{
		if (m_mapMembers.size() > index)
		{
			auto it = m_mapMembers.begin();
			std::advance(it, index);
			return &it->second;
		}

		return nullptr;
	}

	NetworkRoomMember* GetRoomMemberFromID(int64_t puid)
	{
		if (m_mapMembers.contains(puid))
		{
			return &m_mapMembers[puid];
		}

		return nullptr;
	}

	std::map<uint64_t, NetworkRoomMember>& GetMembersListForCurrentRoom();

	// Chat
	void SendChatMessageToCurrentRoom(UnicodeString& strChatMsg, bool bIsAction);

	void ResetCachedRoomData()
	{
		m_mapMembers.clear();
	
		if (m_RosterNeedsRefreshCallback != nullptr)
		{
			m_RosterNeedsRefreshCallback();
		}
	}

	void Tick()
	{
		if (m_pNetRoomMesh != nullptr)
		{
			m_pNetRoomMesh->Tick();
		}
	}

	std::vector<NetworkRoom> GetGroupRooms()
	{
		return m_vecRooms;
	}

	void OnRosterUpdated(std::vector<std::string> vecUsers, std::vector<int64_t> vecIDs);

	int GetCurrentRoomID() const { return m_CurrentRoomID; }


private:
	int m_CurrentRoomID = -1;
	
	// TODO_NGMP: cleanup
	NetworkMesh* m_pNetRoomMesh = nullptr;

	std::vector<NetworkRoom> m_vecRooms;

	std::map<uint64_t, NetworkRoomMember> m_mapMembers = std::map<uint64_t, NetworkRoomMember>();
};