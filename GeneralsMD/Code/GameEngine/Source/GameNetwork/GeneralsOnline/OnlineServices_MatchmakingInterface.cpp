#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"

NGMP_OnlineServices_MatchmakingInterface::NGMP_OnlineServices_MatchmakingInterface()
{

}

void NGMP_OnlineServices_MatchmakingInterface::StartMatchmaking(int playlistID, std::function<void(bool)> fnCallback)
{
	nlohmann::json j;
	j["playlist"] = playlistID;

	std::map<std::string, std::string> mapHeaders;
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking", true);
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
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Matchmaking", true);
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendDELETERequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
		
		});
}
