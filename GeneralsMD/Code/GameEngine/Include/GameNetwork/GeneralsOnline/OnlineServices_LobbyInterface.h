#pragma once

#include "NGMP_include.h"
#include "OnlineServices_RoomsInterface.h"
#include "GameNetwork/GameInfo.h"
#include <chrono>
#include "Common/PlayerList.h"
#include "Common/Player.h"
#include "GameClient/InGameUI.h"
#include "GameLogic/VictoryConditions.h"

extern NGMPGame* TheNGMPGame;

struct LobbyMemberEntry : public NetworkMemberBase
{

	uint16_t preferredPort;
	// NOTE: NetworkMemberBase is not deserialized

	int side = -1;
	int color = -1;
	int team = -1;
	int startpos = -1;
	bool has_map = false;

	uint16_t m_SlotIndex = 999999;
	uint16_t m_SlotState = SlotState::SLOT_OPEN;

	std::string region;
	int latency = 0;

	bool IsHuman() const
	{
		return user_id != -1 && m_SlotState == SlotState::SLOT_PLAYER;
	}
};

enum class ELobbyType
{
	UNKNOWN = -1,
	CustomGame = 0,
	QuickMatch = 1
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
	bool allow_observers;
	uint16_t max_cam_height;

	uint32_t exe_crc;
	uint32_t ini_crc;

	uint64_t match_id = 0;

	ELobbyType lobby_type = ELobbyType::UNKNOWN;

	int rng_seed = -1;

	bool passworded;
	std::string password;

	std::vector<LobbyMemberEntry> members;

	std::string region;
	int latency;
};

enum class EJoinLobbyResult
{
	JoinLobbyResult_Success, // The room was joined.
	JoinLobbyResult_FullRoom,       // The room is full.
	JoinLobbyResult_BadPassword,    // An incorrect password (or none) was given for a passworded room.
	JoinLobbyResult_JoinFailed // Generic failure.
};

enum class ELobbyJoinability
{
	LobbyJoinability_Public,
	LobbyJoinability_FriendsOnly,
};

struct LobbyMemberEntry;
struct LobbyEntry;

class NGMP_OnlineServices_LobbyInterface
{
public:
	NGMP_OnlineServices_LobbyInterface();
	void StopMatchStartCountdownIfRunning();
	void SearchForLobbies(std::function<void()> onStartCallback, std::function<void(std::vector<LobbyEntry>)> onCompleteCallback);
	void DeregisterForSearchForLobbiesCallback()
	{
		m_fnCallbackSearchForLobbiesComplete = nullptr;
	}
	std::function<void(std::vector<LobbyEntry>)> m_fnCallbackSearchForLobbiesComplete = nullptr;

	std::function<void(std::string)> m_fnCallbackMatchmakingMessage = nullptr;
	void RegisterForMatchmakingMessageCallback(std::function<void(std::string)> cb)
	{
		m_fnCallbackMatchmakingMessage = cb;
	}

	void DeregisterForMatchmakingMessageCallback()
	{
		m_fnCallbackMatchmakingMessage = nullptr;
	}

	void InvokeMatchmakingMessageCallback(std::string str)
	{
		if (m_fnCallbackMatchmakingMessage != nullptr)
		{
			m_fnCallbackMatchmakingMessage(str);
		}
	}

	void InvokeMatchmakingStartGameCallback()
	{
		if (m_fnCallbackMatchmakingStartGame != nullptr)
		{
			m_fnCallbackMatchmakingStartGame();
		}
	}

	std::function<void()> m_fnCallbackMatchmakingStartGame = nullptr;
	void RegisterForMatchmakingStartGameCallback(std::function<void()> cb)
	{
		m_fnCallbackMatchmakingStartGame = cb;
	}

	void DeregisterForMatchmakingStartGameCallback()
	{
		m_fnCallbackMatchmakingStartGame = nullptr;
	}

	std::function<void()> m_fnCallbackMatchmakingMatchFound = nullptr;
	void RegisterForMatchmakingMatchFoundCallback(std::function<void()> cb)
	{
		m_fnCallbackMatchmakingMatchFound = cb;
	}

	void DeRegisterForMatchmakingMatchFoundCallback()
	{
		m_fnCallbackMatchmakingMatchFound = nullptr;
	}

	void InvokeMatchmakingMatchFoundCallback()
	{
		if (m_fnCallbackMatchmakingMatchFound != nullptr)
		{
			m_fnCallbackMatchmakingMatchFound();
		}
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

	// AI
	void UpdateCurrentLobby_AIColor(int slot, int color);
	void UpdateCurrentLobby_AISide(int slot, int side, int updatedStartPos);
	void UpdateCurrentLobby_AITeam(int slot, int team);
	void UpdateCurrentLobby_AIStartPos(int slot, int startpos);

	void UpdateCurrentLobbyMaxCameraHeight(uint16_t maxCameraHeight);

	void SetJoinability(ELobbyJoinability joinabilityFlag);

	void UpdateCurrentLobby_KickUser(int64_t userID, UnicodeString name);
	void UpdateCurrentLobby_SetSlotState(uint16_t slotIndex, uint16_t slotState);

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

	UnicodeString m_PendingCreation_LobbyName;
	UnicodeString m_PendingCreation_InitialMapDisplayName;
	AsciiString m_PendingCreation_InitialMapPath;
	void CreateLobby(UnicodeString strLobbyName, UnicodeString strInitialMapName, AsciiString strInitialMapPath, bool bIsOfficial, int initialMaxSize, bool bVanillaTeamsOnly, bool bTrackStats, uint32_t startingCash, bool bPassworded, std::string strPassword, bool bAllowObservers);

	void OnJoinedOrCreatedLobby(bool bAlreadyUpdatedDetails, std::function<void(bool)> fnCallback);

	UnicodeString GetCurrentLobbyDisplayName();
	UnicodeString GetCurrentLobbyMapDisplayName();
	AsciiString GetCurrentLobbyMapPath();

	void SendChatMessageToCurrentLobby(UnicodeString& strChatMsgUnicode, bool bIsAction);
	void SendAnnouncementMessageToCurrentLobby(UnicodeString& strAnnouncementMsgUnicode, bool bShowToHost);

	void InvokeCreateLobbyCallback(bool bSuccess)
	{
		if (m_cb_CreateLobbyPendingCallback != nullptr)
		{
			m_cb_CreateLobbyPendingCallback(bSuccess);
		}
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

	void DeregisterForRosterNeedsRefreshCallback()
	{
		m_RosterNeedsRefreshCallback = nullptr;
	}

	// TODO_NGMP: Better support for packet callbacks
	std::function<void()> m_callbackStartGamePacket = nullptr;
	void RegisterForGameStartPacket(std::function<void()> cb)
	{
		m_callbackStartGamePacket = cb;
	}

	void DeregisterForGameStartPacket()
	{
		m_callbackStartGamePacket = nullptr;
	}

	// periodically force refresh the lobby for data accuracy
	int64_t m_lastForceRefresh = 0;

	void Tick();

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
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Refreshing network room roster");
		return m_CurrentLobby.members;
	}

	void RegisterForCreateLobbyCallback(std::function<void(bool)> callback)
	{
		m_cb_CreateLobbyPendingCallback = callback;
	}

	void DeregisterForCreateLobbyCallback()
	{
		m_cb_CreateLobbyPendingCallback = nullptr;
	}

	void ApplyLocalUserPropertiesToCurrentNetworkRoom();

	void SetCurrentLobby_AcceptState(bool bAccepted)
	{

	}

	bool IsHost();

	void UpdateRoomDataCache(std::function<void(bool)> fnCallback = nullptr);

	std::function<void(LobbyMemberEntry)> m_cbPlayerDoesntHaveMap = nullptr;
	void RegisterForPlayerDoesntHaveMapCallback(std::function<void(LobbyMemberEntry)> cb)
	{
		m_cbPlayerDoesntHaveMap = cb;
	}

	void DeregisterForPlayerDoesntHaveMapCallback()
	{
		m_cbPlayerDoesntHaveMap = nullptr;
	}

	std::function<void(void)> m_OnCannotConnectToLobbyCallback = nullptr;
	void RegisterForCannotConnectToLobbyCallback(std::function<void(void)> cb)
	{
			m_OnCannotConnectToLobbyCallback = cb;
	}

	void DeregisterForCannotConnectToLobbyCallback()
	{
		m_OnCannotConnectToLobbyCallback = nullptr;
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

	void RegisterForJoinLobbyCallback(std::function<void(EJoinLobbyResult)> cb)
	{
		m_callbackJoinedLobby = cb;
	}

	void DeregisterForJoinLobbyCallback()
	{
		m_callbackJoinedLobby = nullptr;
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

	NetworkMesh* GetNetworkMeshForLobby() { return m_pLobbyMesh; }

	void JoinLobby(LobbyEntry lobby, std::string strPassword);

	void LeaveCurrentLobby();

	void ResetHostMigrationFlags()
	{
		m_bHostMigrated = false;
		m_bPendingHostHasLeft = false;
	}

	LobbyEntry GetLobbyFromID(int64_t lobbyID);

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

	std::string GetLobbyTurnUsername() { return m_strTURNUsername; }
	std::string GetLobbyTurnToken() { return m_strTURNToken; }

	uint64_t GetCurrentMatchID()
	{
		return m_CurrentMatchID;
	}

private:
	std::function<void(bool)> m_cb_CreateLobbyPendingCallback = nullptr;

	std::function<void(EJoinLobbyResult)> m_callbackJoinedLobby = nullptr;

	LobbyEntry m_CurrentLobby;

	std::string m_strTURNUsername = "";
	std::string m_strTURNToken = "";

	// TODO_NGMP: cleanup
	NetworkMesh* m_pLobbyMesh = nullptr;

	bool m_bLobbyListDirty = false;

	int64_t m_timeStartAutoReadyCountdown = -1;

	bool m_bAttemptingToJoinLobby = false;
	LobbyEntry m_LobbyTryingToJoin;

	bool m_bSearchInProgress = false;

	bool m_bMarkedGameAsFinished = false;

	uint64_t m_CurrentMatchID = 0;
};
