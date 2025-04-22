#pragma once

#include "NGMP_include.h"
#include "NetworkMesh.h"
#include "../RankPointValue.h"
#include "../GameSpy/PersistentStorageThread.h"

class PSPlayerStats;

class NGMP_OnlineServices_StatsInterface
{
public:
	NGMP_OnlineServices_StatsInterface();

	void GetLocalPlayerStats(std::function<void(void)> cb);
	void GetPlayerStats(int64_t userID, std::function<void(void)> cb);

	Int getPointsForRank(Int rank)
	{
		std::map<int, int> mapRankToXP =
		{
			{RANK_PRIVATE, 0},
			{RANK_CORPORAL, 5},
			{RANK_SERGEANT, 10},
			{RANK_LIEUTENANT, 20},
			{RANK_CAPTAIN, 50},
			{RANK_MAJOR, 100},
			{RANK_COLONEL, 2000},
			{RANK_BRIGADIER_GENERAL, 500},
			{RANK_GENERAL, 1000},
			{RANK_COMMANDER_IN_CHIEF, 2000}
		};

		if (rank < mapRankToXP.size())
		{
			return mapRankToXP[rank];
		}

		return 999999;
	}

	PSPlayerStats findPlayerStatsByID(int64_t userID);
	PSPlayerStats& getCachedLocalPlayerStats();

private:
	PSPlayerStats m_CachedLocalPlayerStats;
};