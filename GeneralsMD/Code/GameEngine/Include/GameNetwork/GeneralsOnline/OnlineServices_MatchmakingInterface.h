#pragma once

#include "NGMP_include.h"

class NGMP_OnlineServices_MatchmakingInterface
{
public:
	NGMP_OnlineServices_MatchmakingInterface();


	void StartMatchmaking(int playlistID, std::function<void(bool)> fnCallback);

	void CancelMatchmaking();
private:

};