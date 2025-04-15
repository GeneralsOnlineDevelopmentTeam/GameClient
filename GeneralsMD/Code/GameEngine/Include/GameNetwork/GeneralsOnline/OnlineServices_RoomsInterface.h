#pragma once

#include "NGMP_include.h"
#include "NetworkMesh.h"

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

	void UpdateRoomDataCache();
	
	std::function<void()> m_PendingRoomJoinCompleteCallback = nullptr;
	void JoinRoom(int roomIndex, std::function<void()> onStartCallback, std::function<void()> onCompleteCallback);

	void LeaveRoom()
	{
		NGMP_OnlineServicesManager::GetInstance()->GetWebSocket()->SendData_LeaveNetworkRoom();
	}

	std::function<void(UnicodeString strMessage)> m_OnChatCallback = nullptr;
	void RegisterForChatCallback(std::function<void(UnicodeString strMessage)> cb)
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

	void CreateRoom(int roomIndex);

	std::map<uint64_t, NetworkRoomMember>& GetMembersListForCurrentRoom();

	//EOS_HLobbyDetails m_currentRoomDetailsHandle = nullptr;

	// Chat
	void SendChatMessageToCurrentRoom(UnicodeString& strChatMsg);

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

	void OnRosterUpdated(std::vector<std::string> vecUsers, std::vector<int64_t> vecIDs);

	int GetCurrentRoomID() const { return m_CurrentRoomID; }

private:
	void ApplyLocalUserPropertiesToCurrentNetworkRoom();

private:
	int m_CurrentRoomID = -1;
	
	// TODO_NGMP: cleanup
	NetworkMesh* m_pNetRoomMesh = nullptr;

	std::map<uint64_t, NetworkRoomMember> m_mapMembers = std::map<uint64_t, NetworkRoomMember>();
};