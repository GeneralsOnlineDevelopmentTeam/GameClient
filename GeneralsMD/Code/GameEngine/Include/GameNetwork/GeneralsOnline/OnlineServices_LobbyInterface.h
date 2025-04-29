#pragma once

#include "NGMP_include.h"
#include "OnlineServices_RoomsInterface.h"
#include "../GameInfo.h"
#include <chrono>

extern NGMPGame* TheNGMPGame;

struct LobbyMemberEntry : public NetworkMemberBase
{

	std::string strIPAddress;
	uint16_t preferredPort;
	// NOTE: NetworkMemberBase is not deserialized

	int side = -1;
	int color = -1;
	int team = -1;
	int startpos = -1;
	bool has_map = false;

	uint16_t m_SlotIndex = 999999;
	uint16_t m_SlotState = SlotState::SLOT_OPEN;

	//bool IsValid() const { return user_id != -1; }
};

struct LobbyEntry
{
	int64_t lobbyID = -1;

	int64_t owner;
	std::string name;
	std::string map_name;
	std::string map_path;
	bool map_official;
	int current_players;
	int max_players;
	bool vanilla_teams;
	uint32_t starting_cash;
	bool limit_superweapons;
	bool track_stats;

	bool passworded;
	std::string password;

	std::vector<BYTE> EncKey;
	std::vector<BYTE> EncIV;
	std::vector<LobbyMemberEntry> members;
};

enum class EJoinLobbyResult
{
	JoinLobbyResult_Success, // The room was joined.
	JoinLobbyResult_FullRoom,       // The room is full.
	JoinLobbyResult_BadPassword,    // An incorrect password (or none) was given for a passworded room.
	JoinLobbyResult_JoinFailed // Generic failure.
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
	void UpdateCurrentLobby_Map(AsciiString strMap, AsciiString strMapPath, bool bIsOfficial, int newMaxPlayers);
	void UpdateCurrentLobby_LimitSuperweapons(bool bLimitSuperweapons);
	void UpdateCurrentLobby_StartingCash(UnsignedInt startingCashValue);

	void UpdateCurrentLobby_HasMap();

	void UpdateCurrentLobby_MySide(int side, int updatedStartPos);
	void UpdateCurrentLobby_MyColor(int side);
	void UpdateCurrentLobby_MyStartPos(int side);
	void UpdateCurrentLobby_MyTeam(int side);

	void UpdateCurrentLobby_KickUser(int64_t userID, UnicodeString name);

	void UpdateCurrentLobby_ForceReady();

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
	void CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, bool bIsOfficial, int initialMaxSize, bool bVanillaTeamsOnly, bool bTrackStats, uint32_t startingCash, bool bPassworded, const char* szPassword);

	void OnJoinedOrCreatedLobby(bool bAlreadyUpdatedDetails, std::function<void(void)> fnCallback);

	void MarkCurrentGameAsStarted();
	void MarkCurrentGameAsFinished();

	UnicodeString GetCurrentLobbyDisplayName();
	UnicodeString GetCurrentLobbyMapDisplayName();
	AsciiString GetCurrentLobbyMapPath();

	void SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode);
	void SendAnnouncementMessageToCurrentLobby(UnicodeString& strAnnouncementMsgUnicode, bool bShowToHost);

	void InvokeCreateLobbyCallback(bool bSuccess)
	{
		for (auto cb : m_vecCreateLobby_PendingCallbacks)
		{
			// TODO_NGMP: Support failure
			cb(bSuccess);
		}
		m_vecCreateLobby_PendingCallbacks.clear();
	}


	LobbyEntry& GetCurrentLobby()
	{
		return m_CurrentLobby;
	}

	NGMPGame* GetCurrentGame()
	{
		return TheNGMPGame;
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

			// do we have a pending start?
			if (IsHost())
			{
				if (m_timeStartAutoReadyCountdown > 0)
				{
					if ((currTime - m_timeStartAutoReadyCountdown) > 30000)
					{
						// TODO_NGMP: Don't do this clientside...
						UpdateCurrentLobby_ForceReady();
						ClearAutoReadyCountdown();
					}
				}
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

	std::function<void(LobbyMemberEntry)> m_cbPlayerDoesntHaveMap = nullptr;
	void RegisterForPlayerDoesntHaveMapCallback(std::function<void(LobbyMemberEntry)> cb)
	{
		m_cbPlayerDoesntHaveMap = cb;
	}

	std::function<void(UnicodeString strMessage)> m_OnChatCallback = nullptr;
	void RegisterForChatCallback(std::function<void(UnicodeString strMessage)> cb)
	{
		m_OnChatCallback = cb;
	}

	void RegisterForJoinLobbyCallback(std::function<void(EJoinLobbyResult)> cb)
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

	void JoinLobby(int index, const char* szPassword);
	void JoinLobby(LobbyEntry lobby, const char* szPassword);

	void LeaveCurrentLobby();

	LobbyEntry GetLobbyFromIndex(int index);

	std::vector<LobbyEntry> m_vecLobbies;

	bool m_bHostMigrated = false;
	bool m_bPendingHostHasLeft = false;

	void StartAutoReadyCountdown()
	{
		m_timeStartAutoReadyCountdown = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	}

	void ClearAutoReadyCountdown()
	{
		m_timeStartAutoReadyCountdown = -1;
	}

	bool HasAutoReadyCountdown()
	{
		return m_timeStartAutoReadyCountdown != -1;
	}

	void SetLobbyTryingToJoin(LobbyEntry lobby)
	{
		m_LobbyTryingToJoin = lobby;
	}

	void ResetLobbyTryingToJoin()
	{
		m_LobbyTryingToJoin = LobbyEntry();
	}

	LobbyEntry GetLobbyTryingToJoin()
	{
		return m_LobbyTryingToJoin;
	}

private:
	std::vector<std::function<void(bool)>> m_vecCreateLobby_PendingCallbacks = std::vector<std::function<void(bool)>>();

	std::function<void(EJoinLobbyResult)> m_callbackJoinedLobby = nullptr;

	LobbyEntry m_CurrentLobby;

	// TODO_NGMP: cleanup
	NetworkMesh* m_pLobbyMesh = nullptr;

	bool m_bLobbyListDirty = false;

	int64_t m_timeStartAutoReadyCountdown = -1;

	LobbyEntry m_LobbyTryingToJoin;

	bool m_bSearchInProgress = false;

	bool m_bMarkedGameAsFinished = false;
};