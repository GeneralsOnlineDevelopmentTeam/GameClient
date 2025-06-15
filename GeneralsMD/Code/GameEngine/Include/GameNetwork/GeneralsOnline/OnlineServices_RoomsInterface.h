#pragma once

#include "NGMP_include.h"
#include "NetworkMesh.h"
#include "../GameSpy/PeerDefs.h"
#include "OnlineServices_Init.h"

enum class EChatMessageType
{
	CHAT_MESSAGE_TYPE_NETWORK_ROOM,
	CHAT_MESSAGE_TYPE_LOBBY
};
static GameSpyColors DetermineColorForChatMessage(EChatMessageType chatMessageType, Bool isPublic, Bool isAction)
{
	GameSpyColors style;

	// TODO_NGMP: Support owner chat again
	Bool isOwner = false;

	if (isPublic && isAction)
	{
		style = (isOwner) ? GSCOLOR_CHAT_OWNER_EMOTE : GSCOLOR_CHAT_EMOTE;
	}
	else if (isPublic)
	{
		style = (isOwner) ? GSCOLOR_CHAT_OWNER : GSCOLOR_CHAT_NORMAL;
	}
	else if (isAction)
	{
		style = (isOwner) ? GSCOLOR_CHAT_PRIVATE_OWNER_EMOTE : GSCOLOR_CHAT_PRIVATE_EMOTE;
	}
	else
	{
		style = (isOwner) ? GSCOLOR_CHAT_PRIVATE_OWNER : GSCOLOR_CHAT_PRIVATE;
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
		NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_LeaveNetworkRoom();
	}

	std::function<void(UnicodeString strMessage, GameSpyColors color)> m_OnChatCallback = nullptr;
	void RegisterForChatCallback(std::function<void(UnicodeString strMessage, GameSpyColors color)> cb)
	{
		m_OnChatCallback = cb;
	}

	std::function<void()> m_RosterNeedsRefreshCallback = nullptr;
	void RegisterForRosterNeedsRefreshCallback(std::function<void()> cb)
	{
		m_RosterNeedsRefreshCallback = cb;
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