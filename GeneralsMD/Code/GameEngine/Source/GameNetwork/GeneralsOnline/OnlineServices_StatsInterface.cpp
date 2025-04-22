#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "../../GameSpy/PersistentStorageThread.h"
#include "../../RankPointValue.h"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"

#include "Common/PlayerTemplate.h"

NGMP_OnlineServices_StatsInterface::NGMP_OnlineServices_StatsInterface()
{
	TheRankPointValues = NEW RankPoints;
}

void NGMP_OnlineServices_StatsInterface::GetLocalPlayerStats(std::function<void(void)> cb)
{
	int64_t localUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Stats", true), localUserID);

	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody)
		{
			PSPlayerStats stats;
			stats.id = localUserID;

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

			m_CachedLocalPlayerStats = stats;

			// cb
			cb();
		});
}

void NGMP_OnlineServices_StatsInterface::GetPlayerStats(int64_t userID, std::function<void(void)> cb)
{
	cb();
}

PSPlayerStats NGMP_OnlineServices_StatsInterface::findPlayerStatsByID(int64_t userID)
{
	// TODO_NGMP_STATS
	return PSPlayerStats();
}

PSPlayerStats& NGMP_OnlineServices_StatsInterface::getCachedLocalPlayerStats()
{
	return m_CachedLocalPlayerStats;
}
