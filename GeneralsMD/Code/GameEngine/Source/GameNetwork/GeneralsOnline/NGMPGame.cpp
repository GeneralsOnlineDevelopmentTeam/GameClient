#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include "GameLogic/VictoryConditions.h"
#include "Common/PlayerList.h"
#include "GameLogic/GameLogic.h"
#include "GameNetwork/FileTransfer.h"
#include "GameClient/MapUtil.h"
#include "GameClient/GameText.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "Common/RandomValue.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "GameNetwork/NetworkInterface.h"
#include "Common/GlobalData.h"
#include "GameClient/View.h"
#include "../NextGenMP_defines.h"

NGMPGameSlot::NGMPGameSlot()
{
	GameSlot();
	m_profileID = 0;
	m_wins = 0;
	m_losses = 0;
	m_rankPoints = 0;
	m_favoriteSide = 0;
	m_pingInt = 0;
	m_profileID = 0;
	m_pingStr.clear();
}

// NGMPGame ----------------------------------------

NGMPGame::NGMPGame()
{
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		return;
	}

	cleanUpSlotPointers();

	setLocalIP(0);

	m_ladderIP.clear();
	m_ladderPort = 0;

	enterGame(); // this is done on join in the GS impl, and must be called before setMap

	// NGMP: Store map
	setMap(pLobbyInterface->GetCurrentLobbyMapPath());

	// init
	//init();

	// NGMP: Populate slots
	UpdateSlotsFromCurrentLobby();
}

NGMPGame::~NGMPGame()
{
	// Force camera to update from config
	TheTacticalView->setDefaultView(0.0f, 0.0f, 1.0f, true);
}

void NGMPGame::SyncWithLobby(LobbyEntry& lobby)
{
	// map
	// correct if custom (game needs full path, this is done in vanilla for CM only, and not QM, but our QM has custom maps, so just do it here for safety)

	AsciiString asciiMapOfficial(lobby.map_path.c_str());
	std::string correctedMapPath = std::format("{}{}", TheGlobalData->getPath_UserData().str(), lobby.map_path.c_str());
	AsciiString asciiMapCustom(correctedMapPath.c_str());
	//TheNGMPGame->setMap(asciiMap);
	asciiMapOfficial.toLower();
	asciiMapCustom.toLower();
	std::map<AsciiString, MapMetaData>::iterator itOfficial = TheMapCache->find(asciiMapOfficial);
	std::map<AsciiString, MapMetaData>::iterator itCustom = TheMapCache->find(asciiMapCustom);

	// is it official?


	if (itOfficial != TheMapCache->end())
	{
		TheNGMPGame->getGameSpySlot(0)->setMapAvailability(TRUE);
		TheNGMPGame->setMapCRC(itOfficial->second.m_CRC);
		TheNGMPGame->setMapSize(itOfficial->second.m_filesize);

		setMap(asciiMapOfficial);
	}
	else if (itCustom != TheMapCache->end())
	{
		TheNGMPGame->getGameSpySlot(0)->setMapAvailability(TRUE);
		TheNGMPGame->setMapCRC(itCustom->second.m_CRC);
		TheNGMPGame->setMapSize(itCustom->second.m_filesize);

		setMap(asciiMapCustom);
	}
	else // fallback
	{
		setMap(lobby.map_path.c_str());
	}

	// superweapon
	setSuperweaponRestriction(lobby.limit_superweapons);

	// vanilla teams
	setOldFactionsOnly(lobby.vanilla_teams);

	// stats
	setUseStats(lobby.track_stats);

	// rng seed
	setSeed(lobby.rng_seed);

	// observers
	setAllowObservers(lobby.allow_observers);

	setHasPassword(lobby.passworded);

	setExeCRC(lobby.exe_crc);
	setIniCRC(lobby.ini_crc);

	UnicodeString lobbyName = UnicodeString(from_utf8(lobby.name).c_str());
	setGameName(lobbyName);

	// starting cash
	Money startingCash;
	startingCash.deposit(lobby.starting_cash, FALSE);
	setStartingCash(startingCash);

}

void NGMPGame::UpdateSlotsFromCurrentLobby()
{
	// none of this should change while in-game, so ignore

	// NOTE: In progress means game has started, in-game just means in the lobby/fronend...
	if (m_inProgress)
	{
		return;
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		return;
	}

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		// this list is provided by the service, ordered by slot index, so we dont need to look up / use the slot index from the member
		LobbyMemberEntry pLobbyMember = pLobbyInterface->GetRoomMemberFromIndex(i);

		// TODO_NGMP: Support spectators
		int playerTemplate = -1;
		if (pLobbyMember.side == -1)
		{
			playerTemplate = PLAYERTEMPLATE_RANDOM;
		}
		else
		{
			playerTemplate = pLobbyMember.side;
		}

		// human or AI player
		if (pLobbyMember.m_SlotState != SlotState::SLOT_OPEN && pLobbyMember.m_SlotState != SlotState::SLOT_CLOSED)
		{
			bool bIsAI = (pLobbyMember.m_SlotState == SlotState::SLOT_EASY_AI || pLobbyMember.m_SlotState == SlotState::SLOT_MED_AI|| pLobbyMember.m_SlotState == SlotState::SLOT_BRUTAL_AI);

			NGMPGameSlot* slot = (NGMPGameSlot*)getSlot(pLobbyMember.m_SlotIndex);

			// NOTE: Internally generals uses 'local ip' to detect which user is local... we dont have an IP, so just use player index for ip
			slot->setState((SlotState)pLobbyMember.m_SlotState, UnicodeString(from_utf8(pLobbyMember.display_name).c_str()), pLobbyMember.m_SlotIndex);

			slot->setColor(pLobbyMember.color);
			slot->setTeamNumber(pLobbyMember.team);
			slot->setStartPos(pLobbyMember.startpos);
			slot->setPlayerTemplate(playerTemplate);

			if (!bIsAI)
			{
				// ready flag
				if (pLobbyMember.m_bIsReady)
				{
					slot->setAccept();
				}
				else
				{
					slot->unAccept();
				}

				// has map?
				slot->setMapAvailability(pLobbyMember.has_map);

				// store EOS ID
				slot->m_userID = pLobbyMember.user_id;
			}
			else
			{
				slot->setAccept();
				slot->setMapAvailability(true);
				slot->m_userID = -1;
			}
		}
		else
		{
			// handle open/closed
			NGMPGameSlot* slot = (NGMPGameSlot*)getSlot(i);
			slot->setState((SlotState)pLobbyMember.m_SlotState);
		}

		// dont need to handle else here, we set it up upon lobby creation
	}
}


void NGMPGame::cleanUpSlotPointers(void)
{
	for (Int i = 0; i < MAX_SLOTS; ++i)
		setSlotPointer(i, &m_Slots[i]);
}

NGMPGameSlot* NGMPGame::getGameSpySlot(Int index)
{
	GameSlot* slot = getSlot(index);
	DEBUG_ASSERTCRASH(slot && (slot == &(m_Slots[index])), ("Bad game slot pointer\n"));
	return (NGMPGameSlot*)slot;
}

void NGMPGame::init(void)
{
	GameInfo::init();

	UpdateSlotsFromCurrentLobby();
}

void NGMPGame::setPingString(AsciiString pingStr)
{
	m_pingStr = pingStr;
	m_pingInt = 0;
	//m_pingInt = TheGameSpyInfo->getPingValue(pingStr);
}

Bool NGMPGame::amIHost(void) const
{
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	return pLobbyInterface == nullptr ? false : pLobbyInterface->IsHost();
}

void NGMPGame::resetAccepted(void)
{
	GameInfo::resetAccepted();

	if (amIHost())
	{
		/*
		peerStateChanged(TheGameSpyChat->getPeer());
		m_hasBeenQueried = false;
		DEBUG_LOG(("resetAccepted() called peerStateChange()\n"));
		*/
	}
}

Int NGMPGame::getLocalSlotNum(void) const
{
	DEBUG_ASSERTCRASH(m_inGame, ("Looking for local game slot while not in game"));
	if (!m_inGame)
		return -1;

	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface == nullptr)
	{
		return -1;
	}

	Int64 localUserID = pAuthInterface->GetUserID();

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const NGMPGameSlot* slot = (const NGMPGameSlot*)getConstSlot(i);
		if (slot == NULL) {
			continue;
		}
		if (slot->m_userID == localUserID)
		{
			return i;
		}
	}

	return -1;
}

void NGMPGame::startGame(Int gameID)
{
	DEBUG_ASSERTCRASH(m_inGame, ("Starting a game while not in game"));
	DEBUG_LOG(("NGMPGame::startGame - game id = %d\n", gameID));
	//DEBUG_ASSERTCRASH(m_transport == NULL, ("m_transport is not NULL when it should be"));
	//DEBUG_ASSERTCRASH(TheNAT == NULL, ("TheNAT is not NULL when it should be"));

	//UnsignedInt localIP = TheGameSpyInfo->getInternalIP();
	UnsignedInt localIP = 1337; // dont care anymore
	setLocalIP(localIP);

	// fill in GS-specific info
	Int numHumans = 0;
	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		if (m_Slots[i].isHuman())
		{
			++numHumans;
			AsciiString gsName;
			gsName.translate(m_Slots[i].getName());

			if (m_isQM)
			{
				// TODO_NGMP: Does this matter anymore?
				if (getLocalSlotNum() == i)
					m_Slots[i].setProfileID(0);  // hehe - we know our own.  the rest, they'll tell us.
			}
			else
			{
				// TODO_NGMP
				/*
				PlayerInfoMap* pInfoMap = TheGameSpyInfo->getPlayerInfoMap();
				PlayerInfoMap::iterator it = pInfoMap->find(gsName);
				if (it != pInfoMap->end())
				{
					m_GameSpySlot[i].setProfileID(it->second.m_profileID);
					m_GameSpySlot[i].setLocale(it->second.m_locale);
					m_GameSpySlot[i].setSlotRankPoints(it->second.m_rankPoints);
					m_GameSpySlot[i].setFavoriteSide(it->second.m_side);
				}
				else
				{
					DEBUG_CRASH(("No player info for %s", gsName.str()));
				}
				*/
			}
		}
	}

	//#if defined(_DEBUG) || defined(_INTERNAL)
	if (numHumans < 2)
	{
		launchGame();
		
		
		// TODO_NGMP: LEave staging room? probably dont care anymore? its all one lobby nowadays
		//if (TheGameSpyInfo)
		//TheGameSpyInfo->leaveStagingRoom();
	}
	else
		//#endif defined(_DEBUG) || defined(_INTERNAL)
	{
		launchGame();
		// TODO_NGMP: We dont care about this anymore? we're already connected
		//TheNAT = NEW NAT();
		//TheNAT->attachSlotList(m_slot, getLocalSlotNum(), m_localIP);
		//TheNAT->establishConnectionPaths();
	}
}

AsciiString NGMPGame::generateGameSpyGameResultsPacket(void)
{
	return AsciiString();
}

AsciiString NGMPGame::generateLadderGameResultsPacket(void)
{
	// TODO_NGMP
	AsciiString results;
	return results;
}

void NGMPGame::launchGame(void)
{
	// TODO_NGMP: Better way of doing this, plus maybe load from file?
#if defined(RTS_DEBUG)
	TheWritableGlobalData->m_benchmarkTimer = 999999999;
	TheWritableGlobalData->m_debugShowGraphicalFramerate = true;
	TheWritableGlobalData->m_showMetrics = true;
#endif

	//TheWritableGlobalData->m_networkPlayerTimeoutTime = 60000;
	//TheWritableGlobalData->m_networkDisconnectScreenNotifyTime = 2500;

	// process service config
	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr != nullptr)
	{
		ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

		if (serviceConf.use_default_config)
		{
			TheWritableGlobalData->m_networkFPSHistoryLength = 30;
			TheWritableGlobalData->m_networkLatencyHistoryLength = 200;
			TheWritableGlobalData->m_networkRunAheadSlack = serviceConf.ra_slack_override_percent_in_default; // normally 10
			TheWritableGlobalData->m_networkRunAheadMetricsTime = 5000;
		}
		else
		{
			TheWritableGlobalData->m_networkFPSHistoryLength = 10;
			TheWritableGlobalData->m_networkLatencyHistoryLength = 10;

			MIN_RUNAHEAD = serviceConf.min_run_ahead_frames;

			TheWritableGlobalData->m_networkRunAheadSlack = serviceConf.ra_slack_percent;
			TheWritableGlobalData->m_networkRunAheadMetricsTime = serviceConf.ra_update_frequency_frames * (float)(1000.f / GENERALS_ONLINE_HIGH_FPS_LIMIT);

			FRAME_GROUPING_CAP = serviceConf.frame_grouping_frames * (float)(1000.f / GENERALS_ONLINE_HIGH_FPS_LIMIT);
		}
	}
	

#if defined(GENERALS_ONLINE_HIGH_FPS_RENDER)
	TheWritableGlobalData->m_horizontalScrollSpeedFactor = NGMP_OnlineServicesManager::Settings.Camera_MoveSpeedRatio();
	TheWritableGlobalData->m_verticalScrollSpeedFactor = NGMP_OnlineServicesManager::Settings.Camera_MoveSpeedRatio();
#endif

	setGameInProgress(TRUE);

	for (Int i = 0; i < MAX_SLOTS; ++i)
	{
		const NGMPGameSlot* slot = getGameSpySlot(i);
		if (slot->isHuman())
		{
			// TODO_NGMP:
			bool bPreordered = false;
			if (bPreordered)
				markPlayerAsPreorder(i);
		}
	}

	// Set up the game network
	AsciiString user;
	AsciiString userList;
	DEBUG_ASSERTCRASH(TheNetwork == NULL, ("For some reason TheNetwork isn't NULL at the start of this game.  Better look into that."));

	if (TheNetwork != NULL) {
		delete TheNetwork;
		TheNetwork = NULL;
	}

	// TODO_NGMP: do we care? we are already connected
	
	// Time to initialize TheNetwork for this game.
	TheNetwork = NetworkInterface::createNetwork();
	TheNetwork->init();
	
	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	if (pMesh != nullptr)
	{

		TheNetwork->SeedLatencyData(pMesh->getMaximumHistoricalLatency());
	}

	// TODO_NGMP: Do we really care about these values anymore
	TheNetwork->setLocalAddress(getLocalIP(), 8888);

	TheNetwork->initTransport();

	TheNetwork->parseUserList(this);

	if (TheGameLogic->isInGame()) {
		TheGameLogic->clearGameData();
	}

	Bool filesOk = DoAnyMapTransfers(this);

	// see if we really have the map.  if not, back out.
	TheMapCache->updateCache();
	if (!filesOk || TheMapCache->findMap(getMap()) == NULL)
	{
		DEBUG_LOG(("After transfer, we didn't really have the map.  Bailing...\n"));
		if (TheNetwork != NULL) {
			delete TheNetwork;
			TheNetwork = NULL;
		}
		GSMessageBoxOk(TheGameText->fetch("GUI:Error"), TheGameText->fetch("GUI:CouldNotTransferMap"));

		void PopBackToLobby(void);
		PopBackToLobby();
		return;
	}

	// Force camera to update from config
	TheTacticalView->setDefaultView(0.0f, 0.0f, 1.0f, false);


	// shutdown the top, but do not pop it off the stack
//		TheShell->hideShell();
	// setup the Global Data with the Map and Seed
	TheWritableGlobalData->m_pendingFile = getMap();

	// send a message to the logic for a new game
	GameMessage* msg = TheMessageStream->appendMessage(GameMessage::MSG_NEW_GAME);
	msg->appendIntegerArgument(GAME_INTERNET);

#if defined(GENERALS_ONLINE_HIGH_FPS_RENDER)

	if (NGMP_OnlineServicesManager::Settings.Graphics_LimitFramerate())
	{
		TheWritableGlobalData->m_framesPerSecondLimit = NGMP_OnlineServicesManager::Settings.Graphics_GetFPSLimit();
		TheWritableGlobalData->m_useFpsLimit = true;
	}
	else
	{
		TheWritableGlobalData->m_framesPerSecondLimit = 30000; // game does this... it's not great
		TheWritableGlobalData->m_useFpsLimit = false;
	}
	
#endif
	//TheWritableGlobalData->m_useFpsLimit = false;

	// Set the random seed
	InitRandom(getSeed());
	DEBUG_LOG(("InitGameLogicRandom( %d )\n", getSeed()));

	// mark us as "Loading" in the buddy list
	// TODO_NGMP
	/*
	BuddyRequest req;
	req.buddyRequestType = BuddyRequest::BUDDYREQUEST_SETSTATUS;
	req.arg.status.status = GP_PLAYING;
	strcpy(req.arg.status.statusString, "Loading");
	sprintf(req.arg.status.locationString, "%s", WideCharStringToMultiByte(TheGameSpyGame->getGameName().str()).c_str());
	TheGameSpyBuddyMessageQueue->addRequest(req);
	*/

    	// Show map name in the match start communicator hint notification
	{
        NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
		if (pLobbyInterface != nullptr)
		{
			LobbyEntry& currentLobby = pLobbyInterface->GetCurrentLobby();

			std::string strMapName = currentLobby.map_name; // NOTE: Includes .map (2) etc
			const std::string strExt = ".map";
			size_t pos = strMapName.find(strExt);
			if (pos != std::string::npos) { strMapName.erase(pos, strExt.size()); }

            UnicodeString msg;
            msg.format(L"Map: %hs\nPress F5 or INSERT to open the communicator.", strMapName.c_str());
            showNotificationBox(AsciiString::TheEmptyString, msg, false);
		}

		
	}
}

void NGMPGame::reset(void)
{
	GameInfo::reset();
}

void NGMPGame::StartCountdown()
{
	m_bCountdownStarted = true;
	m_countdownStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	m_countdownLastCheckTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
	if (pWS != nullptr)
	{
		pWS->SendData_CountdownStarted();
	}
}

