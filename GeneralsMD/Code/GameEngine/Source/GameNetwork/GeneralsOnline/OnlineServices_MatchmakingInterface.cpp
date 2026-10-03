#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/JsonHelpers.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"

NGMP_OnlineServices_MatchmakingInterface::NGMP_OnlineServices_MatchmakingInterface()
{

}

void NGMP_OnlineServices_MatchmakingInterface::RetrievePlaylists(std::function<void(std::vector<PlaylistEntry>)> callbackOnComplete)
{
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking/Playlists");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// TODO_NGMP: Error handling
			try
			{
				nlohmann::json jsonObject = nlohmann::json::parse(strBody);

				std::vector<PlaylistEntry> vecPlaylists;
				for (const auto& playlistEntryIter : jsonObject["playlists"])
				{
					try
					{
					PlaylistEntry playlistEntry;
					JsonGetRequired(playlistEntryIter, "PlaylistID", playlistEntry.PlaylistID);
					JsonGetOptional(playlistEntryIter, "Name", playlistEntry.Name);
					JsonGetOptional(playlistEntryIter, "MinPlayers", playlistEntry.MinPlayers);
					JsonGetOptional(playlistEntryIter, "DesiredPlayers", playlistEntry.DesiredPlayers);
					JsonGetOptional(playlistEntryIter, "MinSelectedMaps", playlistEntry.MinSelectedMaps);
					JsonGetOptional(playlistEntryIter, "AllowTeams", playlistEntry.AllowTeams);
					JsonGetOptional(playlistEntryIter, "TeamSize", playlistEntry.TeamSize);
					JsonGetOptional(playlistEntryIter, "AllowArmySelection", playlistEntry.AllowArmySelection);
					JsonGetOptional(playlistEntryIter, "GracePeriodAtMinPlayersMSec", playlistEntry.GracePeriodAtMinPlayersMSec);

					// maps
					for (const auto& mapEntryIter : playlistEntryIter.at("Maps"))
					{
						PlaylistMapEntry mapEntry;

						JsonGetOptional(mapEntryIter, "Name", mapEntry.Name);
						JsonGetRequired(mapEntryIter, "Path", mapEntry.Path);
						mapEntry.Path = utf8_to_local(mapEntry.Path); // used as a local file path
						JsonGetOptional(mapEntryIter, "Custom", mapEntry.Custom);

						playlistEntry.Maps.push_back(mapEntry);
					}

					vecPlaylists.push_back(playlistEntry);
					}
					catch (...)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Skipping malformed playlist entry");
					}
				}

				// cache
				m_vecCachedPlaylists = vecPlaylists;

				// invoke callback
				if (callbackOnComplete != nullptr)
				{
					callbackOnComplete(vecPlaylists);
				}
			}
			catch (...)
			{

			}

		});
}

void NGMP_OnlineServices_MatchmakingInterface::WidenSearch()
{
	std::map<std::string, std::string> mapHeaders;
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking/Widen");
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, "", [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			
		});
}

void NGMP_OnlineServices_MatchmakingInterface::StartMatchmaking(uint16_t playlistID, std::vector<int> vecSelectedMapIndexes, std::function<void(bool)> fnCallback)
{
	nlohmann::json j;
	j["playlist"] = playlistID;
	j["maps"] = vecSelectedMapIndexes;
	j["exe_crc"] = TheGlobalData->m_exeCRC;
	j["ini_crc"] = TheGlobalData->m_iniCRC;
	j["anticheat_id"] = AnticheatPlugInterface::GetAnticheatIdentifier();

	std::map<std::string, std::string> mapHeaders;
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking");
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPUTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			try
			{
				if (bSuccess && statusCode == 201)
				{
					
				}
				else
				{
					// TODO_QUICKMATCH
				}

				if (fnCallback != nullptr)
				{
					fnCallback(bSuccess);
				}
			}
			catch (...)
			{

			}
		});
}

void NGMP_OnlineServices_MatchmakingInterface::CancelMatchmaking()
{
	nlohmann::json j;

	std::map<std::string, std::string> mapHeaders;
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking");
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
		
		});

	// also reset host migration flags on lobby
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface != nullptr)
	{
		pLobbyInterface->LeaveCurrentLobby();

		pLobbyInterface->ResetHostMigrationFlags();
	}
}
