#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "../../GameSpy/PersistentStorageThread.h"
#include "../../RankPointValue.h"
#include "../OnlineServices_Init.h"
#include "../HTTP/HTTPManager.h"
//#include "../json.hpp"
#include "Common/PlayerTemplate.h"

NGMP_OnlineServices_StatsInterface::NGMP_OnlineServices_StatsInterface()
{
	TheRankPointValues = NEW RankPoints;
}

void NGMP_OnlineServices_StatsInterface::GetLocalPlayerStats(std::function<void(void)> cb)
{
	int a = ThePlayerTemplateStore->getPlayerTemplateCount();
	a = 4;
	int64_t localUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
	std::string strURI = std::format("{}/{}", NGMP_OnlineServicesManager::GetAPIEndpoint("Stats", true), localUserID);

	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody)
		{
			PSPlayerStats stats;
			stats.id = localUserID;

			//nlohmann::json jsonObjectRoot = nlohmann::json::parse(strBody);

			cb();
			//jsonObjectRoot["xp"].get_to(stats.poin);

			//auto lobbyEntryJSON = jsonObjectRoot["lobby"];
		});
}

void NGMP_OnlineServices_StatsInterface::GetPlayerStats(int64_t userID, std::function<void(void)> cb)
{
	cb();
}

PSPlayerStats NGMP_OnlineServices_StatsInterface::findPlayerStatsByID(int64_t userID)
{
	return PSPlayerStats();
}

PSPlayerStats NGMP_OnlineServices_StatsInterface::getCachedLocalPlayerStats()
{
	PSPlayerStats stats;
	stats.id = 1337;
	stats.locale = 1;
	stats.gamesAsRandom = 2;
	stats.options = "Options Here";
	stats.systemSpec = "System Specs";
	stats.lastFPS = 100;
	stats.lastGeneral = 5;
	stats.gamesInRowWithLastGeneral = 10;
	stats.challengeMedals = 1;
	stats.battleHonors = 2;
	stats.QMwinsInARow = 1;
	stats.maxQMwinsInARow = 5;



	stats.winsInARow = 1;
	stats.maxWinsInARow = 2;
	stats.lossesInARow = 3;
	stats.maxLossesInARow = 4;
	stats.disconsInARow = 5;
	stats.maxDisconsInARow = 6;
	stats.desyncsInARow = 7;
	stats.maxDesyncsInARow = 8;

	stats.builtParticleCannon = 1;
	stats.builtNuke = 2;
	stats.builtSCUD = 1;

	stats.lastLadderPort = 100;
	stats.lastLadderHost = "test ip";

	return stats;
}
