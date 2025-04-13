#pragma once

#include "NGMP_include.h"
#include "OnlineServices_RoomsInterface.h"
#include "../GameInfo.h"
#include <chrono>

struct LobbyMemberEntry : public NetworkMemberBase
{
	int64_t user_id = -1;
	std::string display_name;

	std::string strIPAddress;
	uint16_t preferredPort;
	// NOTE: NetworkMemberBase is not deserialized

	bool IsValid() const { return user_id != -1; }
};

struct LobbyEntry
{
	int64_t lobbyID = -1;

	int64_t owner;
	std::string name;
	std::string map_name;
	std::string map_path;
	int current_players;
	int max_players;
	std::vector<LobbyMemberEntry> members;
};

struct LobbyMemberEntry;
struct LobbyEntry;

class NGMP_OnlineServices_LobbyInterface
{
public:
	NGMP_OnlineServices_LobbyInterface();

	void SearchForLobbies(std::function<void()> onStartCallback, std::function<void(std::vector<LobbyEntry>)> onCompleteCallback);

	NextGenTransport* m_transport = nullptr;
	void InitGameTransport()
	{
		if (m_transport != nullptr)
		{
			delete m_transport;
			m_transport = nullptr;
		}
		m_transport = new NextGenTransport;
		m_transport->reset();
		m_transport->init(0, 0); // we dont care about ip/port anymore

		// reuse our socket from here, it already has all the connections formed, its safer + quicker
		//m_transport->SetSocket(m_pLobbyMesh->GetSocketID());
	}

	// updates
	void UpdateCurrentLobby_Map(AsciiString strMap, AsciiString strMapPath, int newMaxPlayers);

	void SetLobbyListDirty()
	{
		m_bLobbyListDirty = true;
	}

	void ConsumeLobbyListDirtyFlag()
	{
		m_bLobbyListDirty = false;
	}

	bool IsLobbyListDirty()
	{
		return m_bLobbyListDirty;
	}

	NextGenTransport* GetGameTransport()
	{
		return m_transport;
	}

	// TODO_NGMP: We dont join right now (other than host)

	UnicodeString m_PendingCreation_LobbyName;
	UnicodeString m_PendingCreation_InitialMapDisplayName;
	AsciiString m_PendingCreation_InitialMapPath;
	void CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, int initialMaxSize);

	void OnJoinedOrCreatedLobby(bool bAlreadyUpdatedDetails = false);

	UnicodeString GetCurrentLobbyDisplayName();
	UnicodeString GetCurrentLobbyMapDisplayName();
	AsciiString GetCurrentLobbyMapPath();

	void SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode);

	void InvokeCreateLobbyCallback(bool bSuccess)
	{
		for (auto cb : m_vecCreateLobby_PendingCallbacks)
		{
			// TODO_NGMP: Support failure
			cb(bSuccess);
		}
		m_vecCreateLobby_PendingCallbacks.clear();
	}

	NGMPGame* GetCurrentGame()
	{
		return m_pGameInst;
	}

	// lobby roster
	std::function<void()> m_RosterNeedsRefreshCallback = nullptr;
	void RegisterForRosterNeedsRefreshCallback(std::function<void()> cb)
	{
		m_RosterNeedsRefreshCallback = cb;
	}

	// TODO_NGMP: Better support for packet callbacks
	std::function<void()> m_callbackStartGamePacket = nullptr;
	void RegisterForGameStartPacket(std::function<void()> cb)
	{
		m_callbackStartGamePacket = cb;
	}

	// periodically force refresh the lobby for data accuracy
	int64_t m_lastForceRefresh = 0;
	void Tick()
	{
		if (m_pLobbyMesh != nullptr)
		{
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
		}
	}

	int64_t GetCurrentLobbyOwnerID()
	{
		return m_CurrentLobby.owner;
	}

	LobbyMemberEntry GetRoomMemberFromIndex(int index)
	{
		// TODO_NGMP: Optimize data structure
		if (index < m_CurrentLobby.members.size())
		{
			return m_CurrentLobby.members.at(index);
		}

		return LobbyMemberEntry();
	}

	LobbyMemberEntry GetRoomMemberFromID(int64_t userid)
	{
		for (const LobbyMemberEntry& lobbyMember : m_CurrentLobby.members)
		{
			if (lobbyMember.user_id == userid)
			{
				return lobbyMember;
			}
		}

		return LobbyMemberEntry();
	}

	std::vector<LobbyMemberEntry>& GetMembersListForCurrentRoom()
	{
		NetworkLog("[NGMP] Refreshing network room roster");
		return m_CurrentLobby.members;
	}

	void RegisterForCreateLobbyCallback(std::function<void(bool)> callback)
	{
		m_vecCreateLobby_PendingCallbacks.push_back(callback);
	}

	void ApplyLocalUserPropertiesToCurrentNetworkRoom();

	void SetCurrentLobby_AcceptState(bool bAccepted)
	{

	}

	bool IsHost();

	void UpdateRoomDataCache(std::function<void(void)> fnCallback = nullptr);

	std::function<void(UnicodeString strMessage)> m_OnChatCallback = nullptr;
	void RegisterForChatCallback(std::function<void(UnicodeString strMessage)> cb)
	{
		m_OnChatCallback = cb;
	}

	void RegisterForJoinLobbyCallback(std::function<void(bool)> cb)
	{
		m_callbackJoinedLobby = cb;
	}

	void ResetCachedRoomData()
	{
		m_CurrentLobby = LobbyEntry();

		if (m_RosterNeedsRefreshCallback != nullptr)
		{
			m_RosterNeedsRefreshCallback();
		}
	}

	bool IsInLobby() const { return m_CurrentLobby.lobbyID != -1; }

	void SendToMesh(NetworkPacket& packet)
	{
		// TODO_NGMP: Respect vecTargetUsers again
		std::vector<int64_t> vecUsers;
		/*
		std::vector<uint64_t> vecUsers;
		for (auto kvPair : m_CurrentLobby.members)
		{
			vecUsers.push_back(kvPair.first);
		}

		if (m_pLobbyMesh != nullptr)
		{
			m_pLobbyMesh->SendToMesh(packet, vecUsers);
		}
		*/

		if (m_pLobbyMesh != nullptr)
		{
			m_pLobbyMesh->SendToMesh(packet, vecUsers);
		}
	}

	NetworkMesh* GetNetworkMesh() { return m_pLobbyMesh; }

	void JoinLobby(int index);

	LobbyEntry GetLobbyFromIndex(int index);

	std::vector<LobbyEntry> m_vecLobbies;

private:
	std::vector<std::function<void(bool)>> m_vecCreateLobby_PendingCallbacks = std::vector<std::function<void(bool)>>();

	std::function<void(bool)> m_callbackJoinedLobby = nullptr;

	LobbyEntry m_CurrentLobby;

	// TODO_NGMP: cleanup
	NetworkMesh* m_pLobbyMesh = nullptr;

	NGMPGame* m_pGameInst = nullptr;

	bool m_bLobbyListDirty = false;
};