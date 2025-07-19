#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "../../GameSpy/PersistentStorageThread.h"
#include "../../RankPointValue.h"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"

#include "Common/PlayerTemplate.h"
#include "../../GameSpy/LadderDefs.h"

NGMP_OnlineServices_StatsInterface::NGMP_OnlineServices_StatsInterface()
{
	TheRankPointValues = NEW RankPoints;

	// TODO_NGMP: Better location
	TheLadderList = NEW LadderList;
}

void NGMP_OnlineServices_StatsInterface::GetGlobalStats(std::function<void(GlobalStats)> cb)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("GlobalStats", true);

	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			GlobalStats stats;

			try
			{
				nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody)["globalstats"];

				int i = 0;

	#define PROCESS_JSON_PER_GENERAL_RESULT(name) i = 0; for (const auto& iter : jsonObjectRoot[#name]) { iter.get_to(stats.##name[i++]); }
				PROCESS_JSON_PER_GENERAL_RESULT(wins);
				PROCESS_JSON_PER_GENERAL_RESULT(matches);
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
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsRequest] Getting stats for user %lld (cache only, not making request due to policy)", userID);
		// is it cached?
		if (m_mapCachedStats.contains(userID))
		{
			cb(true, m_mapCachedStats[userID]);
		}
		else
		{
			cb(false, m_mapCachedStats[userID]);
		}
	}
	else
	{
		bool bDoRequest = false;

		if (requestPolicy == EStatsRequestPolicy::BYPASS_CACHE_FORCE_REQUEST)
		{
			bDoRequest = true;
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsRequest] Getting stats for user %lld (bypassing cache, making request due to policy)", userID);
		}
		else if (requestPolicy == EStatsRequestPolicy::RESPECT_CACHE_ALLOW_REQUEST)
		{
			// do we have a cache time? if not, we'll need to retrieve regardless
			if (!m_mapStatsLastRefresh.contains(userID))
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsRequest] Getting stats for user %lld (respecting cache, but the user has no cached data)", userID);
				bDoRequest = true;
			}
			else
			{
				int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
				int64_t lastCacheTime = m_mapStatsLastRefresh[userID];

				if ((currTime - lastCacheTime) >= m_cacheTTL)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[StatsRequest] Getting stats for user %lld (respecting cache, but the cache is older then the TTL)", userID);
					bDoRequest = true;
				}
			}
		}

		if (bDoRequest)
		{
			std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("PlayerStats", true), userID);

			std::map<std::string, std::string> mapHeaders;

			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					PSPlayerStats stats;
					stats.id = userID;

					try
					{
						nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody)["stats"];

						// parse json
						int i = 0;

#define PROCESS_JSON_PER_GENERAL_RESULT(name) i = 0; for (const auto& iter : jsonObjectRoot[#name]) { iter.get_to(stats.##name[i++]); }
						PROCESS_JSON_PER_GENERAL_RESULT(wins);
						PROCESS_JSON_PER_GENERAL_RESULT(losses);
						PROCESS_JSON_PER_GENERAL_RESULT(games);
						PROCESS_JSON_PER_GENERAL_RESULT(duration);
						PROCESS_JSON_PER_GENERAL_RESULT(unitsKilled);
						PROCESS_JSON_PER_GENERAL_RESULT(unitsLost);
						PROCESS_JSON_PER_GENERAL_RESULT(unitsBuilt);
						PROCESS_JSON_PER_GENERAL_RESULT(buildingsKilled);
						PROCESS_JSON_PER_GENERAL_RESULT(wins);
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

						NetworkLog(ELogVerbosity::LOG_RELEASE, "Cached stats for user %lld", userID);
						m_mapCachedStats[userID] = stats;
						m_mapStatsLastRefresh[userID] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

						// cb
						cb(true, stats);
					}
					catch (...)
					{
						// cb
						cb(false, stats);
					}
				});
		}
		else // cached data instead
		{
			cb(true, m_mapCachedStats[userID]);
		}
		
	}
}

void NGMP_OnlineServices_StatsInterface::UpdateMyStats(PSPlayerStats stats)
{
	int64_t userID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("PlayerStats", true), userID);

	std::map<std::string, std::string> mapHeaders;

	// TODO_NGMP: Only serialize what exists, dont serialize null?
	std::string strJsonData = JSONSerialize(stats);

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strJsonData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{

		});
}

std::string NGMP_OnlineServices_StatsInterface::JSONSerialize(PSPlayerStats stats)
{
	nlohmann::json j;
	PerGeneralMap::iterator it;

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
