#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GameSpy/PersistentStorageThread.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/RankPointValue.h"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"

#include "Common/PlayerTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/GameSpy/LadderDefs.h"

#include <algorithm>
#include <unordered_set>

NGMP_OnlineServices_StatsInterface::NGMP_OnlineServices_StatsInterface()
{
	TheRankPointValues = NEW RankPoints;

	// populate ranks
	// TODO_NGMP: Perhaps get this from the service?
	TheRankPointValues->m_ranks[RANK_PRIVATE] = 0;
	TheRankPointValues->m_ranks[RANK_CORPORAL] = getPointsForRank(RANK_CORPORAL); // 5
	TheRankPointValues->m_ranks[RANK_SERGEANT] = getPointsForRank(RANK_SERGEANT); // 10
	TheRankPointValues->m_ranks[RANK_LIEUTENANT] = getPointsForRank(RANK_LIEUTENANT); // 20
	TheRankPointValues->m_ranks[RANK_CAPTAIN] = getPointsForRank(RANK_CAPTAIN); // 50
	TheRankPointValues->m_ranks[RANK_MAJOR] = getPointsForRank(RANK_MAJOR); // 100
	TheRankPointValues->m_ranks[RANK_COLONEL] = getPointsForRank(RANK_COLONEL); // 200
	TheRankPointValues->m_ranks[RANK_BRIGADIER_GENERAL] = getPointsForRank(RANK_BRIGADIER_GENERAL); // 500
	TheRankPointValues->m_ranks[RANK_GENERAL] = getPointsForRank(RANK_GENERAL); // 1000
	TheRankPointValues->m_ranks[RANK_COMMANDER_IN_CHIEF] = getPointsForRank(RANK_COMMANDER_IN_CHIEF); // 2000

	// TODO_NGMP: Better location
	TheLadderList = NEW LadderList;
}

NGMP_OnlineServices_StatsInterface::~NGMP_OnlineServices_StatsInterface()
{
	if (TheRankPointValues != nullptr)
	{
		delete TheRankPointValues;
		TheRankPointValues = nullptr;
	}

	if (TheLadderList != nullptr)
	{
		delete TheLadderList;
		TheLadderList = nullptr;
	}
}

void NGMP_OnlineServices_StatsInterface::GetGlobalStats(std::function<void(GlobalStats)> cb)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("GlobalStats");

	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			GlobalStats stats;

			try
			{
				if (bSuccess && !strBody.empty())
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					nlohmann::json jsonObjectRoot = jsonObject["globalstats"];

					int i = 0;

#define PROCESS_JSON_PER_GENERAL_RESULT(name) i = 0; for (const auto& iter : jsonObjectRoot[#name]) { iter.get_to(stats.##name[i++]); }
					PROCESS_JSON_PER_GENERAL_RESULT(wins);
					PROCESS_JSON_PER_GENERAL_RESULT(matches);
				}
			}
			catch (...)
			{

			}

			cb(stats);
		});
}

void NGMP_OnlineServices_StatsInterface::findPlayerStatsByID(int64_t userID, std::function<void(bool, PSPlayerStats)> cb, EStatsRequestPolicy requestPolicy)
{
	// TODO_NGMP: this could take a while...
	if (requestPolicy == EStatsRequestPolicy::CACHED_ONLY)
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsRequest] Getting stats for user %lld (cache only, not making request due to policy)", userID);
		PSPlayerStats stats;
		const bool found = getPlayerStatsFromCache(userID, &stats);
		cb(found, stats);
	}
	else
	{
		bool bDoRequest = false;

		if (requestPolicy == EStatsRequestPolicy::BYPASS_CACHE_FORCE_REQUEST)
		{
			bDoRequest = true;
			NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsRequest] Getting stats for user %lld (bypassing cache, making request due to policy)", userID);
		}
		else if (requestPolicy == EStatsRequestPolicy::RESPECT_CACHE_ALLOW_REQUEST)
		{
			if (!HasFreshPlayerStats(userID))
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsRequest] Getting stats for user %lld (cache is missing or stale)", userID);
				bDoRequest = true;
			}
		}

		if (bDoRequest)
		{
			std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("PlayerStats"), userID);
			const uint64_t cacheRevision = AdvancePlayerStatsCacheRevision(userID);

			std::map<std::string, std::string> mapHeaders;

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [this, userID, cb, cacheRevision](bool bSuccess, int statusCode, std::string strBody, HTTPRequest*)
				{
					PSPlayerStats stats;
					stats.id = userID;

					try
					{
						if (bSuccess && statusCode >= 200 && statusCode < 300 && !strBody.empty())
						{
							nlohmann::json jsonObject = nlohmann::json::parse(strBody);
							nlohmann::json jsonObjectRoot = jsonObject["stats"];

							// parse json
							int i = 0;

                            // get user id
							jsonObjectRoot["userID"].get_to(stats.id);

                            // GO extra data
							jsonObjectRoot["EloRating"].get_to(stats.elo_rating);
							jsonObjectRoot["EloMatches"].get_to(stats.elo_num_matches);
							jsonObjectRoot["MonthlyEloRating"].get_to(stats.monthly_elo_rating);

							#define PROCESS_JSON_PER_GENERAL_RESULT(name) i = 0; for (const auto& iter : jsonObjectRoot[#name]) { iter.get_to(stats.##name[i++]); }
							PROCESS_JSON_PER_GENERAL_RESULT(wins);
							PROCESS_JSON_PER_GENERAL_RESULT(losses);
							PROCESS_JSON_PER_GENERAL_RESULT(games);
							PROCESS_JSON_PER_GENERAL_RESULT(duration);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsKilled);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsLost);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsBuilt);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsKilled);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsLost);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsBuilt);
							PROCESS_JSON_PER_GENERAL_RESULT(earnings);
							PROCESS_JSON_PER_GENERAL_RESULT(techCaptured);
							PROCESS_JSON_PER_GENERAL_RESULT(discons);
							PROCESS_JSON_PER_GENERAL_RESULT(desyncs);
							PROCESS_JSON_PER_GENERAL_RESULT(surrenders);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf2p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf3p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf4p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf5p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf6p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf7p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf8p);
							PROCESS_JSON_PER_GENERAL_RESULT(customGames);
							PROCESS_JSON_PER_GENERAL_RESULT(QMGames);

#define PROCESS_JSON_STANDARD_RESULT(name) jsonObjectRoot[#name].get_to(stats.##name)
							PROCESS_JSON_STANDARD_RESULT(locale);
							PROCESS_JSON_STANDARD_RESULT(gamesAsRandom);
							PROCESS_JSON_STANDARD_RESULT(options);
							PROCESS_JSON_STANDARD_RESULT(systemSpec);
							PROCESS_JSON_STANDARD_RESULT(lastFPS);
							PROCESS_JSON_STANDARD_RESULT(lastGeneral);
							PROCESS_JSON_STANDARD_RESULT(gamesInRowWithLastGeneral);
							PROCESS_JSON_STANDARD_RESULT(challengeMedals);
							PROCESS_JSON_STANDARD_RESULT(battleHonors);
							PROCESS_JSON_STANDARD_RESULT(QMwinsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxQMwinsInARow);
							PROCESS_JSON_STANDARD_RESULT(winsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxWinsInARow);
							PROCESS_JSON_STANDARD_RESULT(lossesInARow);
							PROCESS_JSON_STANDARD_RESULT(maxLossesInARow);
							PROCESS_JSON_STANDARD_RESULT(disconsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxDisconsInARow);
							PROCESS_JSON_STANDARD_RESULT(desyncsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxDesyncsInARow);
							PROCESS_JSON_STANDARD_RESULT(builtParticleCannon);
							PROCESS_JSON_STANDARD_RESULT(builtNuke);
							PROCESS_JSON_STANDARD_RESULT(builtSCUD);
							PROCESS_JSON_STANDARD_RESULT(lastLadderPort);
							PROCESS_JSON_STANDARD_RESULT(lastLadderHost);

							if (stats.id != userID)
							{
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsRequest] Ignored mismatched stats for user %lld (received %d)", userID, stats.id);
								cb(false, PSPlayerStats());
								return;
							}

							if (!TryCachePlayerStats(stats, cacheRevision))
							{
								PSPlayerStats cachedStats;
								const bool found = getPlayerStatsFromCache(userID, &cachedStats);
								cb(found, cachedStats);
								return;
							}

							cb(true, stats);
						}
						else
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "Stats: Unparsable JSON 3: Empty body or failure");
							cb(false, stats);
						}
					}
					catch (nlohmann::json::exception& jsonException)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Stats: Unparsable JSON 1: %s (%s)", strBody.c_str(), jsonException.what());
						cb(false, stats);
					}
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "Stats: Unparsable JSON 2: %s", strBody.c_str());
						cb(false, stats);
					}
				});
		}
		else // cached data instead
		{
			PSPlayerStats stats;
			const bool found = getPlayerStatsFromCache(userID, &stats);
			cb(found, stats);
		}
		
	}
}

void NGMP_OnlineServices_StatsInterface::findPlayerStatsByBatch(std::vector<int64_t> vecUserIDs, std::function<void(bool)> cb)
{
	// If they asked for nothing, just invoke the callback
	if (vecUserIDs.empty())
	{
		cb(true);
		return;
	}

	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("PlayerStats/Batch");

    std::map<std::string, std::string> mapHeaders;
	std::unordered_map<int64_t, uint64_t> cacheRevisions;
	std::vector<int64_t> uniqueUserIDs;
	uniqueUserIDs.reserve(vecUserIDs.size());
	for (int64_t userID : vecUserIDs)
	{
		if (!cacheRevisions.contains(userID))
		{
			cacheRevisions.emplace(userID, AdvancePlayerStatsCacheRevision(userID));
			uniqueUserIDs.push_back(userID);
		}
	}

    nlohmann::json j;
	j["user_ids"] = uniqueUserIDs;
    std::string strPostData = j.dump();

    NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [this, cb, cacheRevisions](bool bSuccess, int statusCode, std::string strBody, HTTPRequest*)
        {
			if (!bSuccess || statusCode < 200 || statusCode >= 300)
			{
				cb(false);
			}
			else
			{
				// returns an array of PlayerStats
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					std::unordered_set<int64_t> receivedUserIDs;
					bool parsedAllResults = true;

					std::list<std::pair<int64_t, int64_t>> missingConnections;
					for (const auto& statsUserIter : jsonObject["stats"])
					{
						try
						{
							PSPlayerStats stats;

							// get user id
							statsUserIter["userID"].get_to(stats.id);

							// GO extra data
							statsUserIter["EloRating"].get_to(stats.elo_rating);
							statsUserIter["EloMatches"].get_to(stats.elo_num_matches);
							statsUserIter["MonthlyEloRating"].get_to(stats.monthly_elo_rating);

							// now get stats
							int i = 0;

#define PROCESS_JSON_PER_GENERAL_RESULT(name) i = 0; for (const auto& iter : statsUserIter[#name]) { iter.get_to(stats.##name[i++]); }
							PROCESS_JSON_PER_GENERAL_RESULT(wins);
							PROCESS_JSON_PER_GENERAL_RESULT(losses);
							PROCESS_JSON_PER_GENERAL_RESULT(games);
							PROCESS_JSON_PER_GENERAL_RESULT(duration);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsKilled);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsLost);
							PROCESS_JSON_PER_GENERAL_RESULT(unitsBuilt);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsKilled);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsLost);
							PROCESS_JSON_PER_GENERAL_RESULT(buildingsBuilt);
							PROCESS_JSON_PER_GENERAL_RESULT(earnings);
							PROCESS_JSON_PER_GENERAL_RESULT(techCaptured);
							PROCESS_JSON_PER_GENERAL_RESULT(discons);
							PROCESS_JSON_PER_GENERAL_RESULT(desyncs);
							PROCESS_JSON_PER_GENERAL_RESULT(surrenders);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf2p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf3p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf4p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf5p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf6p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf7p);
							PROCESS_JSON_PER_GENERAL_RESULT(gamesOf8p);
							PROCESS_JSON_PER_GENERAL_RESULT(customGames);
							PROCESS_JSON_PER_GENERAL_RESULT(QMGames);

#define PROCESS_JSON_STANDARD_RESULT(name) statsUserIter[#name].get_to(stats.##name)
							PROCESS_JSON_STANDARD_RESULT(locale);
							PROCESS_JSON_STANDARD_RESULT(gamesAsRandom);
							PROCESS_JSON_STANDARD_RESULT(options);
							PROCESS_JSON_STANDARD_RESULT(systemSpec);
							PROCESS_JSON_STANDARD_RESULT(lastFPS);
							PROCESS_JSON_STANDARD_RESULT(lastGeneral);
							PROCESS_JSON_STANDARD_RESULT(gamesInRowWithLastGeneral);
							PROCESS_JSON_STANDARD_RESULT(challengeMedals);
							PROCESS_JSON_STANDARD_RESULT(battleHonors);
							PROCESS_JSON_STANDARD_RESULT(QMwinsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxQMwinsInARow);
							PROCESS_JSON_STANDARD_RESULT(winsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxWinsInARow);
							PROCESS_JSON_STANDARD_RESULT(lossesInARow);
							PROCESS_JSON_STANDARD_RESULT(maxLossesInARow);
							PROCESS_JSON_STANDARD_RESULT(disconsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxDisconsInARow);
							PROCESS_JSON_STANDARD_RESULT(desyncsInARow);
							PROCESS_JSON_STANDARD_RESULT(maxDesyncsInARow);
							PROCESS_JSON_STANDARD_RESULT(builtParticleCannon);
							PROCESS_JSON_STANDARD_RESULT(builtNuke);
							PROCESS_JSON_STANDARD_RESULT(builtSCUD);
							PROCESS_JSON_STANDARD_RESULT(lastLadderPort);
							PROCESS_JSON_STANDARD_RESULT(lastLadderHost);

							auto revisionIt = cacheRevisions.find(stats.id);
							if (revisionIt == cacheRevisions.end())
							{
								parsedAllResults = false;
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsBatch] Ignored unexpected stats for user %d", stats.id);
							}
							else if (!receivedUserIDs.insert(stats.id).second)
							{
								parsedAllResults = false;
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsBatch] Ignored duplicate stats for user %d", stats.id);
							}
							else if (!TryCachePlayerStats(stats, revisionIt->second))
							{
								parsedAllResults = false;
							}
						}
						catch (nlohmann::json::exception& jsonException)
						{
							parsedAllResults = false;
							NetworkLog(ELogVerbosity::LOG_RELEASE, "StatsBatch: Unparsable JSON 1: %s (%s)", strBody.c_str(), jsonException.what());
						}
						catch (...)
						{
							parsedAllResults = false;
							NetworkLog(ELogVerbosity::LOG_RELEASE, "StatsBatch: Unparsable JSON 2: %s", strBody.c_str());
						}
					}

					if (receivedUserIDs.size() != cacheRevisions.size())
					{
						parsedAllResults = false;
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsBatch] Response omitted %zu requested user(s)", cacheRevisions.size() - receivedUserIDs.size());
					}

					cb(parsedAllResults);
				}
				catch (nlohmann::json::exception& jsonException)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "StatsBatchParent: Unparsable JSON 1: %s (%s)", strBody.c_str(), jsonException.what());

					cb(false);
				}
				catch (...)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "StatsBatchParent: Unparsable JSON 2: %s", strBody.c_str());

					cb(false);
				}
			}
        });
}

bool NGMP_OnlineServices_StatsInterface::getPlayerStatsFromCache(int64_t userID, PSPlayerStats* outStats)
{
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsRequest] Getting stats for user %lld (cache only, not making request due to policy)", userID);
	if (outStats == nullptr)
	{
		return false;
	}

	auto cacheIt = m_playerStatsCache.find(userID);
	if (cacheIt != m_playerStatsCache.end() && cacheIt->second.hasStats)
	{
		cacheIt->second.lastAccessAt = std::chrono::steady_clock::now();
		*outStats = cacheIt->second.stats;
		return true;
	}

	*outStats = PSPlayerStats();
	return false;
}

bool NGMP_OnlineServices_StatsInterface::HasFreshPlayerStats(int64_t userID)
{
	auto cacheIt = m_playerStatsCache.find(userID);
	if (cacheIt == m_playerStatsCache.end() || !cacheIt->second.hasStats
		|| cacheIt->second.lastRefreshAt == std::chrono::steady_clock::time_point{})
	{
		return false;
	}

	const auto currentTime = std::chrono::steady_clock::now();
	cacheIt->second.lastAccessAt = currentTime;
	return currentTime >= cacheIt->second.lastRefreshAt
		&& currentTime - cacheIt->second.lastRefreshAt < STATS_CACHE_TTL;
}

NGMP_OnlineServices_StatsInterface::PlayerStatsCacheEntry& NGMP_OnlineServices_StatsInterface::GetOrCreatePlayerStatsCacheEntry(int64_t userID)
{
	auto cacheIt = m_playerStatsCache.find(userID);
	if (cacheIt != m_playerStatsCache.end())
	{
		return cacheIt->second;
	}

	if (m_playerStatsCache.size() >= MAX_PLAYER_STATS_CACHE_ENTRIES)
	{
		auto oldestIt = std::min_element(m_playerStatsCache.begin(), m_playerStatsCache.end(), [](const auto& lhs, const auto& rhs)
		{
			return lhs.second.lastAccessAt < rhs.second.lastAccessAt;
		});
		if (oldestIt != m_playerStatsCache.end())
		{
			m_playerStatsCache.erase(oldestIt);
		}
	}

	return m_playerStatsCache.try_emplace(userID).first->second;
}

uint64_t NGMP_OnlineServices_StatsInterface::AdvancePlayerStatsCacheRevision(int64_t userID)
{
	PlayerStatsCacheEntry& entry = GetOrCreatePlayerStatsCacheEntry(userID);
	entry.lastAccessAt = std::chrono::steady_clock::now();
	entry.cacheRevision = ++m_nextStatsCacheRevision;
	return entry.cacheRevision;
}

uint64_t NGMP_OnlineServices_StatsInterface::AdvancePlayerStatsUpdateRevision(int64_t userID)
{
	PlayerStatsCacheEntry& entry = GetOrCreatePlayerStatsCacheEntry(userID);
	entry.lastAccessAt = std::chrono::steady_clock::now();
	entry.updateRevision = ++m_nextStatsUpdateRevision;
	return entry.updateRevision;
}

bool NGMP_OnlineServices_StatsInterface::TryCachePlayerStats(const PSPlayerStats& stats, uint64_t expectedRevision)
{
	auto cacheIt = m_playerStatsCache.find(stats.id);
	if (cacheIt == m_playerStatsCache.end() || cacheIt->second.cacheRevision != expectedRevision)
	{
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsCache] Ignored superseded stats for user %d", stats.id);
		return false;
	}

	PlayerStatsCacheEntry& entry = cacheIt->second;
	entry.stats = stats;
	entry.lastRefreshAt = std::chrono::steady_clock::now();
	entry.lastAccessAt = entry.lastRefreshAt;
	entry.hasStats = true;
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsCache] Cached stats for user %d", stats.id);
	return true;
}

void NGMP_OnlineServices_StatsInterface::MarkPlayerStatsCacheStale(int64_t userID)
{
	PlayerStatsCacheEntry& entry = GetOrCreatePlayerStatsCacheEntry(userID);
	entry.lastRefreshAt = {};
	entry.lastAccessAt = std::chrono::steady_clock::now();
	entry.cacheRevision = ++m_nextStatsCacheRevision;
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsCache] Marked stats stale for user %lld", userID);
}

void NGMP_OnlineServices_StatsInterface::UpdateMyStats(const PSPlayerStats& stats)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("PlayerStats");

	std::map<std::string, std::string> mapHeaders;

	// TODO_NGMP: Only serialize what exists, dont serialize null?
	std::string strJsonData = JSONSerialize(stats);
	const int statsID = stats.id;
	const uint64_t updateRevision = AdvancePlayerStatsUpdateRevision(statsID);

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strJsonData.c_str(), [this, statsID, updateRevision](bool bSuccess, int statusCode, std::string, HTTPRequest*)
		{
			auto cacheIt = m_playerStatsCache.find(statsID);
			if (cacheIt == m_playerStatsCache.end() || cacheIt->second.updateRevision != updateRevision)
			{
				NetworkLog(ELogVerbosity::LOG_DEBUG, "[StatsUpdate] Ignored superseded update result for user %d", statsID);
				return;
			}

			if (bSuccess && statusCode >= 200 && statusCode < 300)
			{
				// The deployed service does not return canonical stats from PUT.
				// Keep the last-known values available while refreshing them instead
				// of caching the submitted client values.
				MarkPlayerStatsCacheStale(statsID);
				findPlayerStatsByID(statsID, [](bool, PSPlayerStats) {}, EStatsRequestPolicy::BYPASS_CACHE_FORCE_REQUEST);
			}
			else
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsUpdate] Failed to upload stats for user %d (HTTP %d)", statsID, statusCode);
			}
		});
}

struct MatchOutcomeResponse
{
    std::string screenshot_url;
	std::string replay_url;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(MatchOutcomeResponse, screenshot_url, replay_url)
};

void NGMP_OnlineServices_StatsInterface::CommitMyOutcome(ScoreKeeper* pScoreKeeper, bool bWon)
{
	int buildingsBuilt = 0;
	int buildingsDestroyed = 0;
	int buildingsLost = 0;
	int unitsBuilt = 0;
	int unitsDestroyed = 0;
	int unitsLost = 0;
	int totalMoney = 0;
	if (pScoreKeeper != nullptr)
	{
        buildingsBuilt = pScoreKeeper->getTotalBuildingsBuilt();
        buildingsDestroyed = pScoreKeeper->getTotalBuildingsDestroyed();
        buildingsLost = pScoreKeeper->getTotalBuildingsLost();
        unitsBuilt = pScoreKeeper->getTotalUnitsBuilt();
        unitsDestroyed = pScoreKeeper->getTotalUnitsDestroyed();
        unitsLost = pScoreKeeper->getTotalUnitsLost();
        totalMoney = pScoreKeeper->getTotalMoneyEarned();
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{

		uint64_t currentMatchID = pLobbyInterface->GetCurrentMatchID();

		int resolvedSide = -1;
		NGMPGame* myGame = pLobbyInterface->GetCurrentGame();

		    if (myGame != nullptr)
			{
				GameSlot* pLocalSlot = myGame->getSlot(myGame->getLocalSlotNum());
				if (pLocalSlot != nullptr)
				{
					resolvedSide = pLocalSlot->getPlayerTemplate();
				}
            }
		
	    const bool desynced = TheNetwork->sawCRCMismatch();
		const uint32_t duration = TheGameLogic->getFrame() / LOGICFRAMES_PER_SECOND;

		nlohmann::json j;
		j["buildings_built"] = buildingsBuilt;
		j["buildings_killed"] = buildingsDestroyed;
		j["buildings_lost"] = buildingsLost;
		j["units_built"] = unitsBuilt;
		j["units_killed"] = unitsDestroyed;
		j["units_lost"] = unitsLost;
		j["total_money"] = totalMoney;
		j["won"] = bWon;
		j["match_id"] = currentMatchID;
		j["side"] = resolvedSide;
		j["desynced"] = desynced;
		j["duration"] = duration;

		std::string strPostData = j.dump();
	
		std::string strURI = std::format("{}/Outcome", NGMP_OnlineServicesManager::GetAPIEndpoint("Lobby"));
		std::map<std::string, std::string> mapHeaders;

		NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
			{
				if (bSuccess && !strBody.empty())
				{
					try
					{
                        nlohmann::json jsonObject = nlohmann::json::parse(strBody);
                        MatchOutcomeResponse matchOutcomeResp = jsonObject.get<MatchOutcomeResponse>();

                        NGMP_OnlineServicesManager::GetInstance()->SetScreenshotS3URI_EndMatch(currentMatchID, matchOutcomeResp.screenshot_url);
                        NGMP_OnlineServicesManager::GetInstance()->SetScreenshotS3URI_Replay(currentMatchID, matchOutcomeResp.replay_url);
					}
                    catch (nlohmann::json::exception&)
                    {
                       
                    }
                    catch (...)
                    {

                    }
				}
			});
	}
}

std::string NGMP_OnlineServices_StatsInterface::JSONSerialize(const PSPlayerStats& stats) const
{
	nlohmann::json j;
	PerGeneralMap::const_iterator it;

#define ITERATE_OVER_GREATER_THAN_ZERO(ENUMVAL, ARR) i = 0; for (it = ARR.begin(); it != ARR.end(); ++it) \
{ \
	if (it->second > 0) \
	{ \
		j[((int)ENUMVAL) + i]=it->second; \
	} \
	++i;\
}

#define ITERATE_OVER_ANY(ENUMVAL, ARR) i = 0; for (it = ARR.begin(); it != ARR.end(); ++it) \
{ \
		j[((int)ENUMVAL) + i]=it->second; \
	++i;\
}

	int i = 0;

	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::WINS_PER_GENERAL_0, stats.wins);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::LOSSES_PER_GENERAL_0, stats.losses);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMES_PER_GENERAL_0, stats.games);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::DURATION_PER_GENERAL_0, stats.duration);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::UNITSKILLED_PER_GENERAL_0, stats.unitsKilled);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::UNITSLOST_PER_GENERAL_0, stats.unitsLost);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::UNITSBUILT_PER_GENERAL_0, stats.unitsBuilt);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::BUILDINGSKILLED_PER_GENERAL_0, stats.buildingsKilled);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::BUILDINGSLOST_PER_GENERAL_0, stats.buildingsLost);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::BUILDINGSBUILT_PER_GENERAL_0, stats.buildingsBuilt);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::EARNINGS_PER_GENERAL_0, stats.earnings);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::TECHCAPTURED_PER_GENERAL_0, stats.techCaptured);
	
	// NOTE: This one doesn't check >0 in the original impl, not sure why
	ITERATE_OVER_ANY(EStatIndex::DISCONS_PER_GENERAL_0, stats.discons);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::DESYNCS_PER_GENERAL_0, stats.desyncs);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::SURRENDERS_PER_GENERAL_0, stats.surrenders);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF2P_PER_GENERAL_0, stats.gamesOf2p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF3P_PER_GENERAL_0, stats.gamesOf3p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF4P_PER_GENERAL_0, stats.gamesOf4p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF5P_PER_GENERAL_0, stats.gamesOf5p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF6P_PER_GENERAL_0, stats.gamesOf6p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF7P_PER_GENERAL_0, stats.gamesOf7p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::GAMESOF8P_PER_GENERAL_0, stats.gamesOf8p);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::CUSTOMGAMES_PER_GENERAL_0, stats.customGames);
	ITERATE_OVER_GREATER_THAN_ZERO(EStatIndex::QUICKMATCHES_PER_GENERAL_0, stats.QMGames);

	if (stats.locale > 0)
	{
		j[(int)EStatIndex::LOCALE] = stats.locale;
	}

	if (stats.gamesAsRandom > 0)
	{
		j[(int)EStatIndex::GAMES_AS_RANDOM] = stats.gamesAsRandom;
	}

	if (stats.options.length())
	{
		j[(int)EStatIndex::OPTIONS] = stats.options.c_str();
	}

	if (stats.systemSpec.length())
	{
		j[(int)EStatIndex::SYSTEM_SPEC] = stats.systemSpec.c_str();
	}

	if (stats.lastFPS > 0.0f)
	{
		j[(int)EStatIndex::LASTFPS] = stats.lastFPS;
	}
	if (stats.lastGeneral >= 0)
	{
		j[(int)EStatIndex::LASTGENERAL] = stats.lastGeneral;
	}
	if (stats.gamesInRowWithLastGeneral >= 0)
	{
		j[(int)EStatIndex::GAMESINROWWITHLASTGENERAL] = stats.gamesInRowWithLastGeneral;
	}
	if (stats.builtParticleCannon >= 0)
	{
		j[(int)EStatIndex::BUILTPARTICLECANNON] = stats.builtParticleCannon;
	}
	if (stats.builtNuke >= 0)
	{
		j[(int)EStatIndex::BUILTNUKE] = stats.builtNuke;
	}
	if (stats.builtSCUD >= 0)
	{
		j[(int)EStatIndex::BUILTSCUD] = stats.builtSCUD;
	}
	if (stats.challengeMedals > 0)
	{
		j[(int)EStatIndex::CHALLENGEMEDALS] = stats.challengeMedals;
	}
	if (stats.battleHonors > 0)
	{
		j[(int)EStatIndex::BATTLEHONORS] = stats.battleHonors;
	}

	//if (stats.winsInARow > 0) // NOTE: Was like this in base game
	{
		j[(int)EStatIndex::WINSINAROW] = stats.winsInARow;
	}
	if (stats.maxWinsInARow > 0)
	{
		j[(int)EStatIndex::MAXWINSINAROW] = stats.maxWinsInARow;
	}

	//if (stats.lossesInARow > 0) // NOTE: Was like this in base game
	{
		j[(int)EStatIndex::LOSSESINAROW] = stats.lossesInARow;
	}
	if (stats.maxLossesInARow > 0)
	{
		j[(int)EStatIndex::MAXLOSSESINAROW] = stats.maxLossesInARow;
	}

	//if (stats.disconsInARow > 0) // NOTE: Was like this in base game
	{
		j[(int)EStatIndex::DISCONSINAROW] = stats.disconsInARow;
	}
	if (stats.maxDisconsInARow > 0)
	{
		j[(int)EStatIndex::MAXDISCONSINAROW] = stats.maxDisconsInARow;
	}

	//if (stats.desyncsInARow > 0) // NOTE: Was like this in base game
	{
		j[(int)EStatIndex::DESYNCSINAROW] = stats.desyncsInARow;
	}
	if (stats.maxDesyncsInARow > 0)
	{
		j[(int)EStatIndex::MAXDESYNCSINAROW] = stats.maxDesyncsInARow;
	}

	if (stats.lastLadderPort > 0)
	{
		j[(int)EStatIndex::LASTLADDERPORT] = stats.lastLadderPort;
	}
	if (stats.lastLadderHost.length())
	{
		j[(int)EStatIndex::LASTLADDERHOST] = stats.lastLadderHost.c_str();
	}

	return j.dump();
}
