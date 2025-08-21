#include "GameNetwork/GeneralsOnline/NetworkMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include <ws2ipdef.h>
#include "../../NetworkDefs.h"
#include "../../NetworkInterface.h"
#include "GameLogic/GameLogic.h"
#include "../OnlineServices_RoomsInterface.h"
#include "../json.hpp"
#include "../HTTP/HTTPManager.h"
#include "../OnlineServices_Init.h"
#include "ValveNetworkingSockets/steam/isteamnetworkingutils.h"
#include "ValveNetworkingSockets/steam/steamnetworkingcustomsignaling.h"

bool g_bForceRelay = false;
UnsignedInt m_exeCRCOriginal = 0;

// Called when a connection undergoes a state transition
void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();

	if (pMesh == nullptr)
	{
		return;
	}

	// find player connection
	PlayerConnection* pPlayerConnection = nullptr;
	std::map<int64_t, PlayerConnection>& connections = pMesh->GetAllConnections();
	for (auto& kvPair : connections)
	{
		if (kvPair.second.m_hSteamConnection == pInfo->m_hConn)
		{
			pPlayerConnection = &kvPair.second;
			break;
		}
	}


	//if (pPlayerConnection != nullptr)
	{
		//NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Player Connection was null", pInfo->m_info.m_szConnectionDescription);
		//return;
	}

	// What's the state of the connection?
	switch (pInfo->m_info.m_eState)
	{
	case k_ESteamNetworkingConnectionState_ClosedByPeer:
	case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] %s, reason %d: %s\n",
			pInfo->m_info.m_szConnectionDescription,
			(pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer ? "closed by peer" : "problem detected locally"),
			pInfo->m_info.m_eEndReason,
			pInfo->m_info.m_szEndDebug
		);

		// Close our end
		SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);

		if (pPlayerConnection != nullptr && pInfo != nullptr)
		{
			ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
			const int numSignallingAttempts = 3;
			bool bShouldRetry = pPlayerConnection->m_SignallingAttempts < numSignallingAttempts && serviceConf.retry_signalling;

			bool bWasError = pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || pInfo->m_info.m_eEndReason != k_ESteamNetConnectionEnd_App_Generic;
			pPlayerConnection->SetDisconnected(bWasError, pMesh, bShouldRetry && bWasError);
			
			// the highest slot player, should leave. In most cases, this is the most recently joined player, but this may not be 100% accurate due to backfills.
			// TODO_NGMP: In the future, we should pick the most recently joined by timestamp
			if (bWasError) // only if it wasn't a clean disconnect (e.g. lobby leave)
			{
				int myLobbySlot = -1;
				int disconnectedLobbySlot = -1;

				NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
				NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
				if (pLobbyInterface != nullptr && pAuthInterface != nullptr)
				{
					int64_t myUserID = pAuthInterface->GetUserID();

					auto lobbyMembers = pLobbyInterface->GetMembersListForCurrentRoom();
					for (const auto& lobbyMember : lobbyMembers)
					{
						if (lobbyMember.user_id == pPlayerConnection->m_userID)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Determined target player slot to be %d\n", lobbyMember.m_SlotIndex);
							disconnectedLobbySlot = lobbyMember.m_SlotIndex;
						}
						else if (lobbyMember.user_id == myUserID)
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Determined my slot to be %d\n", lobbyMember.m_SlotIndex);
							myLobbySlot = lobbyMember.m_SlotIndex;
						}

						if (myLobbySlot != -1 && disconnectedLobbySlot != -1)
						{
							break; // we are done
						}
					}
				}
				

				NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Determined we didn't connect due to an error, Retrying: %d (currently at %d/%d attempts)", bShouldRetry, pPlayerConnection->m_SignallingAttempts, numSignallingAttempts);
				
				// should we retry signaling?
				if (bShouldRetry)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Retrying...");
					WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();
					if (pWS != nullptr)
					{
						// Behavior:
						// disconnected slot is higher than ours, do nothing, they will signal
						// disconnected slot is lower than ours, we signal
						// -1, meaning we didnt determine slots properly, we signal anyway
						if ((myLobbySlot == -1 || disconnectedLobbySlot == -1) || (myLobbySlot > disconnectedLobbySlot))
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Send signal start request...");

							pWS->SendData_RequestSignalling(pPlayerConnection->m_userID);
						}
						else
						{
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Not sending signal start request, other player should");
						}

					}
					else
					{
						// Should always have a websocket... so lets just fail
						bShouldRetry = false;
					}
				}

				if (!bShouldRetry)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Not retrying, handling disconnect as failure...");

					

					// Behavior:
					// disconnected slot is higher than ours, do nothing, they will leave
					// disconnected slot is lower than ours, we leave
					// -1, meaning we didnt determine slots properly, we leave
					if ((myLobbySlot == -1 || disconnectedLobbySlot == -1) || (myLobbySlot > disconnectedLobbySlot))
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] My Lobby slot is %d, target lobby slot is %d, performing local removal from lobby due to failure to connect\n", myLobbySlot, disconnectedLobbySlot);
						if (pLobbyInterface->m_OnCannotConnectToLobbyCallback != nullptr)
						{
							pLobbyInterface->m_OnCannotConnectToLobbyCallback();
						}
					}
				}
			}


			// In this example, we will bail the test whenever this happens.
			// Was this a normal termination?
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING]DISCONNECTED OR PROBLEM DETECTED %d\n", pInfo->m_info.m_eEndReason);
		}
		else
		{
			// Why are we hearing about any another connection?
			assert(false);
		}

		break;

	case k_ESteamNetworkingConnectionState_None:
		// Notification that a connection was destroyed.  (By us, presumably.)
		// We don't need this, so ignore it.
		break;

	case k_ESteamNetworkingConnectionState_Connecting:

		// Is this a connection we initiated, or one that we are receiving?
		if (pMesh->GetListenSocketHandle() != k_HSteamListenSocket_Invalid && pInfo->m_info.m_hListenSocket == pMesh->GetListenSocketHandle())
		{
			// Somebody's knocking
			// Note that we assume we will only ever receive a single connection

#if _DEBUG
			if (pPlayerConnection != nullptr)
				assert(pPlayerConnection->m_hSteamConnection == k_HSteamNetConnection_Invalid); // not really a bug in this code, but a bug in the test
#endif

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Considering Accepting\n", pInfo->m_info.m_szConnectionDescription);

			if (pPlayerConnection != nullptr && pInfo != nullptr)
			{
				pPlayerConnection->UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
				pPlayerConnection->m_hSteamConnection = pInfo->m_hConn;
			}

			// check user is in the lobby, otherwise reject
			NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
			if (pLobbyInterface == nullptr)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Rejecting - Lobby interface is null\n", pInfo->m_info.m_szConnectionDescription);
				SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 1000, "Lobby interface is null (Rejected)", false);
				return;
			}

			auto currentLobby = pLobbyInterface->GetCurrentLobby();
			bool bPlayerIsInLobby = false;
			for (const auto& member : currentLobby.members)
			{
				// TODO_NGMP: Use bytes or SteamID instead... string compare is nasty
				if (std::to_string(member.user_id) == pInfo->m_info.m_identityRemote.GetGenericString())
				{
					bPlayerIsInLobby = true;
					break;
				}
			}

			if (bPlayerIsInLobby)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Accepting - Player is in lobby\n", pInfo->m_info.m_szConnectionDescription);
				SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);
			}
			else
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Rejecting - Player is not in lobby\n", pInfo->m_info.m_szConnectionDescription);
				SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 1000, "Player is not in lobby (Rejected)", false);
			}
			
		}
		else
		{
			// Note that we will get notification when our own connection that
			// we initiate enters this state.
#if _DEBUG
			if (pPlayerConnection != nullptr)
			assert(pPlayerConnection->m_hSteamConnection == pInfo->m_hConn);
#endif
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Entered connecting state\n", pInfo->m_info.m_szConnectionDescription);

			if (pPlayerConnection != nullptr)
			{
				pPlayerConnection->UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
			}
		}
		break;

	case k_ESteamNetworkingConnectionState_FindingRoute:
		// P2P connections will spend a brief time here where they swap addresses
		// and try to find a route.
		if (pPlayerConnection != nullptr && pInfo != nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] finding route\n", pInfo->m_info.m_szConnectionDescription);

			pPlayerConnection->UpdateState(EConnectionState::FINDING_ROUTE, pMesh);
		}
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		// We got fully connected
#if _DEBUG
		//assert(pInfo->m_hConn == pPlayerConnection->m_hSteamConnection); // We don't initiate or accept any other connections, so this should be out own connection
#endif

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] connected\n", pInfo->m_info.m_szConnectionDescription);

		if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unauthenticated)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_Unauthenticated");
		}
		else if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Unencrypted)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_Unencrypted");
		}
		else if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_LoopbackBuffers");
		}
		else if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Fast)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_Fast");
		}
		else if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_Relayed");
		}
		else if (pInfo->m_info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_DualWifi)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[CONNECTION FLAGS]: has k_nSteamNetworkConnectionInfoFlags_DualWifi");
		}

		if (pPlayerConnection != nullptr)
		{
			pPlayerConnection->UpdateState(EConnectionState::CONNECTED_DIRECT, pMesh);
		}

		break;

	default:
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM CALLBACK] Unhandled case");
		break;
	}
}

/// Implementation of ITrivialSignalingClient
class CSignalingClient : public ISignalingClient
{

	// This is the thing we'll actually create to send signals for a particular
	// connection.
	struct ConnectionSignaling : ISteamNetworkingConnectionSignaling
	{
		CSignalingClient* const m_pOwner;
		int64_t const m_targetUserID;

		ConnectionSignaling(CSignalingClient* owner, int64_t target_user_id)
			: m_pOwner(owner)
			, m_targetUserID(target_user_id)
		{
		}

		//
		// Implements ISteamNetworkingConnectionSignaling
		//

		// This is called from SteamNetworkingSockets to send a signal.  This could be called from any thread,
		// so we need to be threadsafe, and avoid duoing slow stuff or calling back into SteamNetworkingSockets
		virtual bool SendSignal(HSteamNetConnection hConn, const SteamNetConnectionInfo_t& info, const void* pMsg, int cbMsg) override
		{
			// Silence warnings
			(void)info;
			(void)hConn;

			std::vector<uint8_t> vecPayload(cbMsg);
			memcpy_s(vecPayload.data(), vecPayload.size(), pMsg, cbMsg);

			m_pOwner->Send(m_targetUserID, vecPayload);
			return true;
		}

		// Self destruct.  This will be called by SteamNetworkingSockets when it's done with us.
		virtual void Release() override
		{
			delete this;
		}
	};

	struct QueuedSend
	{
		int64_t target_user_id;
		std::vector<uint8_t> vecPayload;
	};
	ISteamNetworkingSockets* const m_pSteamNetworkingSockets;
	std::deque<QueuedSend> m_queueSend;

	void CloseSocket()
	{
		m_queueSend.clear();
	}

public:
	CSignalingClient(ISteamNetworkingSockets* pSteamNetworkingSockets)
		:  m_pSteamNetworkingSockets(pSteamNetworkingSockets)
	{
		// Save off our identity
		SteamNetworkingIdentity identitySelf; identitySelf.Clear();
		pSteamNetworkingSockets->GetIdentity(&identitySelf);

		if (identitySelf.IsInvalid())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "CSignalingClient: Local identity is invalid\n");
		}

		if (identitySelf.IsLocalHost())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "CSignalingClient: Local identity is localhost\n");
		}

	}

	// Send the signal.
	void Send(int64_t target_user_id, std::vector<uint8_t>& vecPayload)
	{
		WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();
		if (pWS != nullptr)
		{
			std::scoped_lock<std::recursive_mutex> lock(pWS->GetLock());

			// If we're getting backed up, delete the oldest entries.  Remember,
			// we are only required to do best-effort delivery.  And old signals are the
			// most likely to be out of date (either old data, or the client has already
			// timed them out and queued a retry).
			while (m_queueSend.size() > 128)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "Signaling send queue is backed up.  Discarding oldest signals\n");
				m_queueSend.pop_front();
			}

			QueuedSend newEntry = QueuedSend();
			newEntry.target_user_id = target_user_id;
			newEntry.vecPayload = vecPayload;
			m_queueSend.push_back(newEntry);
		}
	}

	ISteamNetworkingConnectionSignaling* CreateSignalingForConnection(
		const SteamNetworkingIdentity& identityPeer,
		SteamNetworkingErrMsg& errMsg
	) override {
		SteamNetworkingIdentityRender sIdentityPeer(identityPeer);

		// FIXME - here we really ought to confirm that the string version of the
		// identity does not have spaces, since our protocol doesn't permit it.
		NetworkLog(ELogVerbosity::LOG_DEBUG, "Creating signaling session for peer '%s'\n", sIdentityPeer.c_str());

		// Silence warnings
		(void)errMsg;
		int64_t user_id = std::stoll(identityPeer.GetGenericString());
		return new ConnectionSignaling(this, user_id);
	}

	inline int HexDigitVal(char c)
	{
		if ('0' <= c && c <= '9')
			return c - '0';
		if ('a' <= c && c <= 'f')
			return c - 'a' + 0xa;
		if ('A' <= c && c <= 'F')
			return c - 'A' + 0xa;
		return -1;
	}

	virtual void Poll() override
	{
		WebSocket* pWS = NGMP_OnlineServicesManager::GetWebSocket();
		if (pWS != nullptr)
		{
			pWS->GetLock().lock();

			// Drain the socket
			// Flush send queue
			while (!m_queueSend.empty())
			{
				QueuedSend sendData = m_queueSend.front();

				pWS->SendData_Signalling(sendData.target_user_id, sendData.vecPayload);
				m_queueSend.pop_front();
			}

			// TODO_NGMP: Avoid copy
			std::queue<std::vector<uint8_t>> pendingSignals = pWS->m_pendingSignals;
			pWS->m_pendingSignals = std::queue<std::vector<uint8_t>>();
			pWS->GetLock().unlock();

			// Now dispatch any buffered signals
			if (!pendingSignals.empty())
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] PROCESS SIGNAL!");
				while (!pendingSignals.empty())
				{
					// NOTE: outbound msg doesnt need sender ID, we only need that to determine target on the server, everything else is included in the payload
					// 
					// Get the next signal
					std::vector<uint8_t> signalData = pendingSignals.front();
					pendingSignals.pop();

					// Setup a context object that can respond if this signal is a connection request.
					struct Context : ISteamNetworkingSignalingRecvContext
					{
						CSignalingClient* m_pOwner;

						virtual ISteamNetworkingConnectionSignaling* OnConnectRequest(
							HSteamNetConnection hConn,
							const SteamNetworkingIdentity& identityPeer,
							int nLocalVirtualPort
						) override {
							// Silence warnings
							(void)hConn;
							;						(void)nLocalVirtualPort;

							// We will just always handle requests through the usual listen socket state
							// machine.  See the documentation for this function for other behaviour we
							// might take.

							// Also, note that if there was routing/session info, it should have been in
							// our envelope that we know how to parse, and we should save it off in this
							// context object.
							SteamNetworkingErrMsg ignoreErrMsg;
							return m_pOwner->CreateSignalingForConnection(identityPeer, ignoreErrMsg);
						}
						
						virtual void SendRejectionSignal(
							const SteamNetworkingIdentity& identityPeer,
							const void* pMsg, int cbMsg
						) override {

							// We'll just silently ignore all failures.  This is actually the more secure
							// Way to handle it in many cases.  Actively returning failure might allow
							// an attacker to just scrape random peers to see who is online.  If you know
							// the peer has a good reason for trying to connect, sending an active failure
							// can improve error handling and the UX, instead of relying on timeout.  But
							// just consider the security implications.

							// Silence warnings
							(void)identityPeer;
							(void)pMsg;
							(void)cbMsg;
						}
					};
					Context context;
					context.m_pOwner = this;

					// Dispatch.
					// Remember: From inside this function, our context object might get callbacks.
					// And we might get asked to send signals, either now, or really at any time
					// from any thread!  If possible, avoid calling this function while holding locks.
					// To process this call, SteamnetworkingSockets will need take its own internal lock.
					// That lock may be held by another thread that is asking you to send a signal!  So
					// be warned that deadlocks are a possibility here.
					m_pSteamNetworkingSockets->ReceivedP2PCustomSignal(signalData.data(), (int)signalData.size(), &context);
				}
			}
		}
		}


	virtual void Release() override
	{
		// NOTE: Here we are assuming that the calling code has already cleaned
		// up all the connections, to keep the example simple.
		CloseSocket();
	}
};


NetworkMesh::NetworkMesh()
{
	// try a shutdown
	GameNetworkingSockets_Kill();

	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "pOnlineServicesMgr is invalid");
		return;
	}

	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
	if (pAuthInterface == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "pAuthInterface is invalid");
		return;
	}

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "pLobbyInterface is invalid");
		return;
	}


	int64_t localUserID = pAuthInterface->GetUserID();

	SteamNetworkingIdentity identityLocal;
	identityLocal.Clear();
	identityLocal.SetGenericString(std::to_string(localUserID).c_str());

	if (identityLocal.IsInvalid())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "SteamNetworkingIdentity is invalid");
		return;
	}

	// initialize Steam Sockets
	SteamDatagramErrMsg errMsg;
	if (!GameNetworkingSockets_Init(&identityLocal, errMsg))
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init failed.  %s", errMsg);
		return;
	}

	// TODO_STEAM: Dont hardcode, get everything from service
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, "stun:stun.playgenerals.online:53,stun:stun.playgenerals.online:3478,stun.l.google.com:19302,stun1.l.google.com:19302,stun2.l.google.com:19302,stun3.l.google.com:19302,stun4.l.google.com:19302");

	// comma seperated setting lists
	const char* turnList = "turn:turn.playgenerals.online:53?transport=udp,turn:turn.playgenerals.online:3478?transport=udp";

	m_strTurnUsername = pLobbyInterface->GetLobbyTurnUsername();
	m_strTurnToken = pLobbyInterface->GetLobbyTurnToken();

	//const char* szUsername = "g04024f26713bae6e055295b6887b7007533f6c236534b725734b37e26ec15cd,g04024f26713bae6e055295b6887b7007533f6c236534b725734b37e26ec15cd";
	//const char* szToken = "9ea6a5e60216c09a1fa7512987b2ce0514e3204f863f04f70fa870a100db740f,9ea6a5e60216c09a1fa7512987b2ce0514e3204f863f04f70fa870a100db740f";

	//strUsername = "g04024f26713bae6e055295b6887b7007533f6c236534b725734b37e26ec15cd";
	//strToken = "9ea6a5e60216c09a1fa7512987b2ce0514e3204f863f04f70fa870a100db740f";

	m_strTurnUsernameString = std::format("{},{}", m_strTurnUsername.c_str(), m_strTurnUsername.c_str());
	m_strTurnTokenString = std::format("{},{}", m_strTurnToken.c_str(), m_strTurnToken.c_str());

	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, turnList);
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_UserList, m_strTurnUsernameString.c_str());
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_PassList, m_strTurnTokenString.c_str());

	ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

	// Allow sharing of any kind of ICE address.
	if (g_bForceRelay || serviceConf.relay_all_traffic)
	{
		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Relay);
	}
	else
	{
		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);
	}

	m_hListenSock = k_HSteamListenSocket_Invalid;
	
	// create signalling service
	m_pSignaling = new CSignalingClient(SteamNetworkingSockets());
	if (m_pSignaling == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "CreateTrivialSignalingClient failed.  %s", errMsg);
		return;
	}

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChanged);

	ESteamNetworkingSocketsDebugOutputType logType =
#if defined(_DEBUG)
		ESteamNetworkingSocketsDebugOutputType::k_ESteamNetworkingSocketsDebugOutputType_Debug
#else
		NGMP_OnlineServicesManager::Settings.Debug_VerboseLogging() ? ESteamNetworkingSocketsDebugOutputType::k_ESteamNetworkingSocketsDebugOutputType_Debug : ESteamNetworkingSocketsDebugOutputType::k_ESteamNetworkingSocketsDebugOutputType_Msg
#endif;
		;

	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, logType);
	SteamNetworkingUtils()->SetDebugOutputFunction(logType, [](ESteamNetworkingSocketsDebugOutputType nType, const char* pszMsg)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING LOGFUNC] %s", pszMsg);
		});

	int localPort = serviceConf.use_mapped_port ? NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort() : 0;

	// create sockets
	SteamNetworkingConfigValue_t opt;
	opt.SetInt32(k_ESteamNetworkingConfig_SymmetricConnect, 1); // << Note we set symmetric mode on the listen socket
	m_hListenSock = SteamNetworkingSockets()->CreateListenSocketP2P(localPort, 1, &opt);

	if (m_hListenSock == k_HSteamListenSocket_Invalid)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "CreateListenSocketP2P failed. Sock was invalid");
	}
}


void NetworkMesh::Flush()
{
	for (auto& connectionData : m_mapConnections)
	{
		SteamNetworkingSockets()->FlushMessagesOnConnection(connectionData.second.m_hSteamConnection);
	}
}


void NetworkMesh::RegisterConnectivity(int64_t userID)
{
	nlohmann::json j;
	j["target"] = userID;
	j["direct"] = false;
	j["outcome"] = EConnectionState::NOT_CONNECTED;
	j["ipv4"] = true;
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ConnectionOutcome");
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// dont care about the response
		});
}

void NetworkMesh::UpdateConnectivity(PlayerConnection* connection)
{
	nlohmann::json j;
	j["target"] = connection->m_userID;
	j["direct"] = connection->IsDirect();
	j["outcome"] = connection->GetState();
	j["ipv4"] = connection->IsIPV4();
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ConnectionOutcome");
	std::map<std::string, std::string> mapHeaders;
	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			// dont care about the response
		});
}


bool NetworkMesh::HasGamePacket()
{
	return !m_queueQueuedGamePackets.empty();
}

QueuedGamePacket NetworkMesh::RecvGamePacket()
{
	if (HasGamePacket())
	{
		QueuedGamePacket frontPacket = m_queueQueuedGamePackets.front();
		m_queueQueuedGamePackets.pop();
		return frontPacket;
	}

	return QueuedGamePacket();
}

int NetworkMesh::SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t user_id)
{
	if (m_mapConnections.contains(user_id))
	{
		return m_mapConnections[user_id].SendGamePacket(pBuffer, totalDataSize);
	}
	
	return -2;
}


void NetworkMesh::StartConnectionSignalling(int64_t remoteUserID, uint16_t preferredPort)
{
	// if we already have a connection to this use, drop it, having a single-direction connection will break signalling
	if (m_mapConnections.find(remoteUserID) != m_mapConnections.end())
	{
		if (m_mapConnections[remoteUserID].m_hSteamConnection != k_HSteamNetConnection_Invalid)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection %lld, new connection is being negotiated", remoteUserID);
			SteamNetworkingSockets()->CloseConnection(m_mapConnections[remoteUserID].m_hSteamConnection, 0, "Client Disconnecting Gracefully (new connection being negotiated)", false);
		}

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[ERASE 3] Removing user %lld", m_mapConnections[remoteUserID].m_userID);
		m_mapConnections.erase(remoteUserID);
	}

	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

	if (pAuthInterface == nullptr || pOnlineServicesMgr == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - Auth or OSM interface is null");
		return;
	}

	// never connect to ourself
	if (remoteUserID == pAuthInterface->GetUserID())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - Skipping connection to user %lld - user is local", remoteUserID);
		return;
	}

	SteamNetworkingIdentity identityRemote;
	identityRemote.Clear();
	identityRemote.SetGenericString(std::to_string(remoteUserID).c_str());

	if (identityRemote.IsInvalid())
	{
		// TODO_STEAM: Handle this better
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - SteamNetworkingIdentity is invalid");
		return;
	}

	std::vector<SteamNetworkingConfigValue_t > vecOpts;

	ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

	int g_nLocalPort = serviceConf.use_mapped_port ? pOnlineServicesMgr->GetPortMapper().GetOpenPort() : 0;
	int g_nVirtualPortRemote = serviceConf.use_mapped_port ? preferredPort : 0;

	// Our remote and local port don't match, so we need to set it explicitly
	if (g_nVirtualPortRemote != g_nLocalPort)
	{
		SteamNetworkingConfigValue_t opt;
		opt.SetInt32(k_ESteamNetworkingConfig_LocalVirtualPort, g_nLocalPort);
		vecOpts.push_back(opt);
	}

	// Set symmetric connect mode
	SteamNetworkingConfigValue_t opt;
	opt.SetInt32(k_ESteamNetworkingConfig_SymmetricConnect, 1);
	vecOpts.push_back(opt);
	NetworkLog(ELogVerbosity::LOG_DEBUG, "Connecting to '%s' in symmetric mode, virtual port %d, from local virtual port %d.\n",
		SteamNetworkingIdentityRender(identityRemote).c_str(), g_nVirtualPortRemote, g_nLocalPort);

	// create a signaling object for this connection
	SteamNetworkingErrMsg errMsg;
	ISteamNetworkingConnectionSignaling* pConnSignaling = m_pSignaling->CreateSignalingForConnection(identityRemote, errMsg);

	if (pConnSignaling == nullptr)
	{
		// TODO_STEAM: Handle this better
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - Could not create signalling object, error was %s", errMsg);
		return;
	}

	// make a steam connection obj
	HSteamNetConnection hSteamConnection = SteamNetworkingSockets()->ConnectP2PCustomSignaling(pConnSignaling, &identityRemote, g_nVirtualPortRemote, (int)vecOpts.size(), vecOpts.data());

	if (hSteamConnection == k_HSteamNetConnection_Invalid)
	{
		// TODO_STEAM: Handle this better
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - Steam network connection obj was k_HSteamNetConnection_Invalid");
		return;
	}

	// create a local user type
	m_mapConnections[remoteUserID] = PlayerConnection(remoteUserID, hSteamConnection);

	// add attempt
	++m_mapConnections[remoteUserID].m_SignallingAttempts;
}


void NetworkMesh::DisconnectUser(int64_t remoteUserID)
{
	if (m_mapConnections.find(remoteUserID) != m_mapConnections.end())
	{
		if (m_mapConnections[remoteUserID].m_hSteamConnection != k_HSteamNetConnection_Invalid)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection %lld", remoteUserID);
			SteamNetworkingSockets()->CloseConnection(m_mapConnections[remoteUserID].m_hSteamConnection, 0, "Client Disconnecting Gracefully (not in lobby list)", false);
		}

		NetworkLog(ELogVerbosity::LOG_RELEASE, "[ERASE 1] Removing user %lld", m_mapConnections[remoteUserID].m_userID);
		m_mapConnections.erase(remoteUserID);
	}
}

void NetworkMesh::Disconnect()
{
	// close every connection
	for (auto& connectionData : m_mapConnections)
	{
		SteamNetworkingSockets()->CloseConnection(connectionData.second.m_hSteamConnection, 0, "Client Disconnecting Gracefully", false);
	}

	// invalidate socket
	m_hListenSock = k_HSteamNetConnection_Invalid;

	// clear map
	m_mapConnections.clear();
 
	// tear down steam sockets
	GameNetworkingSockets_Kill();
}

void NetworkMesh::Tick()
{
	// Check for incoming signals, and dispatch them
	if (m_pSignaling != nullptr)
	{
		m_pSignaling->Poll();
	}

	// Check callbacks
	if (SteamNetworkingSockets())
	{
		SteamNetworkingSockets()->RunCallbacks();
	}

	// update connection histograms
	for (auto& kvPair : m_mapConnections)
	{
		PlayerConnection& conn = kvPair.second;
		conn.UpdateLatencyHistogram();
	}
}


PlayerConnection::PlayerConnection(int64_t userID, HSteamNetConnection hSteamConnection)
{
	m_userID = userID;
	
	// no connection yet
	m_hSteamConnection = hSteamConnection;

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	if (pMesh != nullptr)
	{
		pMesh->RegisterConnectivity(userID);
	}
}

int PlayerConnection::SendGamePacket(void* pBuffer, uint32_t totalDataSize)
{
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[GAME PACKET] Sending msg of size %ld\n", totalDataSize);
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		m_hSteamConnection, pBuffer, (int)totalDataSize, k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession, nullptr);

	if (r != k_EResultOK)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Failed to send, err code was %d", r);
	}

	return (int)r;
}


void PlayerConnection::UpdateLatencyHistogram()
{
	// update latency history
	int currLatency = GetLatency();
#if defined(GENERALS_ONLINE_HIGH_FPS_SERVER)
	const int connectionHistoryLength = 120000/16; // ~2 min worth of frames
#else
	const int connectionHistoryLength = 120000/33; // ~2 min worth of frames
#endif

	if (m_vecLatencyHistory.size() >= connectionHistoryLength)
	{
		m_vecLatencyHistory.erase(m_vecLatencyHistory.begin());
	}
	m_vecLatencyHistory.push_back(currLatency);
}

bool PlayerConnection::IsIPV4()
{
	SteamNetConnectionInfo_t info;
	SteamNetworkingSockets()->GetConnectionInfo(m_hSteamConnection, &info);

	return info.m_addrRemote.IsIPv4();
}

int PlayerConnection::Recv(SteamNetworkingMessage_t** pMsg)
{
	int r = -1;
	if (m_hSteamConnection != k_HSteamNetConnection_Invalid)
	{
		r = SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_hSteamConnection, pMsg, 255);
	}

	return r;
}


std::string PlayerConnection::GetStats()
{
	char szBuf[2048] = { 0 };
	int ret = SteamNetworkingSockets()->GetDetailedConnectionStatus(m_hSteamConnection, szBuf, 2048);

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM] PlayerConnection::GetStats returned %d", ret);
	return std::string(szBuf);
}


std::string PlayerConnection::GetConnectionType()
{
	char szBuf[2048] = { 0 };
	int ret = SteamNetworkingSockets()->GetConnectionType(m_hSteamConnection, szBuf, 2048);
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM] PlayerConnection::GetConnectionType returned %d", ret);
	return std::string(szBuf);
}

void PlayerConnection::UpdateState(EConnectionState newState, NetworkMesh* pOwningMesh)
{
	m_State = newState;
	pOwningMesh->UpdateConnectivity(this);

	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
	if (pLobbyInterface == nullptr)
	{
		return;
	}

	std::wstring strDisplayName = L"Unknown User";
	auto currentLobby = pLobbyInterface->GetCurrentLobby();
	for (const auto& member : currentLobby.members)
	{
		if (member.user_id == m_userID)
		{
			strDisplayName = from_utf8(member.display_name);
			break;
		}
	}

	if (pOwningMesh->m_cbOnConnected != nullptr)
	{
		pOwningMesh->m_cbOnConnected(m_userID, strDisplayName, this);
	}
}

void PlayerConnection::SetDisconnected(bool bWasError, NetworkMesh* pOwningMesh, bool bIsRetrying)
{
	if (bWasError)
	{
		if (bIsRetrying)
		{
			m_State = EConnectionState::NOT_CONNECTED;
		}
		else
		{
			m_State = EConnectionState::CONNECTION_FAILED;
		}
	}
	else
	{
		m_State = EConnectionState::CONNECTION_DISCONNECTED;
	}

	// Dont update backend until we're actually done
	if (!bIsRetrying)
	{
		UpdateState(m_State, pOwningMesh);
	}

	m_hSteamConnection = k_HSteamNetConnection_Invalid; // invalidate connection handle
}

int PlayerConnection::GetLatency()
{
	// TODO_STEAM: consider using lanes
	if (m_hSteamConnection != k_HSteamNetConnection_Invalid)
	{
		const int k_nLanes = 1;
		SteamNetConnectionRealTimeStatus_t status;
		SteamNetConnectionRealTimeLaneStatus_t laneStatus[k_nLanes];

		

		EResult res = SteamNetworkingSockets()->GetConnectionRealTimeStatus(m_hSteamConnection, &status, k_nLanes, laneStatus);
		if (res == k_EResultOK)
		{
			return status.m_nPing;
		}
	}

	return -1;
}
