#include "GameNetwork/GeneralsOnline/NetworkMesh.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include <ws2ipdef.h>
#include "GameNetwork/NetworkDefs.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameLogic/GameLogic.h"
#include "../OnlineServices_RoomsInterface.h"
#include "../json.hpp"
#include "../HTTP/HTTPManager.h"
#include "../OnlineServices_Init.h"
#include "ValveNetworkingSockets/steam/isteamnetworkingutils.h"
#include "ValveNetworkingSockets/steam/steamnetworkingcustomsignaling.h"
#include "../PluginInterfaces.h"
#include "ValveNetworkingSockets/steam/isteamnetworkingsockets.h"
#include "ValveNetworkingSockets/steam/steamnetworkingsockets.h"

bool g_bForceRelay = false;
UnsignedInt m_exeCRCOriginal = 0;

// Static flag to track if NetworkMesh is being destroyed to prevent callback re-entry
static std::atomic<bool> g_bNetworkMeshDestroying = false;

// SECURITY FIX: Thread-safe pool for deferred deletion of ConnectionSignaling objects
// to prevent "delete this" races during async Steam callbacks
static std::mutex g_pendingDeletionMutex;
static std::vector<void*> g_pendingConnSignalingDeletions;

// Clean up pending ConnectionSignaling objects that were deferred during Release()
// Forward declaration needed since ConnectionSignaling is nested inside CSignalingClient
struct ISteamNetworkingConnectionSignaling;

static void CleanupPendingConnSignalingDeletions()
{
	std::vector<void*> objectsToDelete;
	{
		std::scoped_lock<std::mutex> lock(g_pendingDeletionMutex);
		objectsToDelete.swap(g_pendingConnSignalingDeletions);
	}
	
	for (void* pObj : objectsToDelete)
	{
		// SECURITY: Delete through base interface to avoid nested class visibility issues
		delete static_cast<ISteamNetworkingConnectionSignaling*>(pObj);
	}
}

// Called when a connection undergoes a state transition
void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	// Clean up any pending ConnectionSignaling deletions from previous callbacks
	CleanupPendingConnSignalingDeletions();
	
	// Early exit if NetworkMesh is being destroyed to prevent use-after-free
	if (g_bNetworkMeshDestroying.load())
	{
		return;
	}

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();

	if (pMesh == nullptr)
	{
		return;
	}

	// find player connection
	int64_t connectionID = -1;
	std::map<int64_t, PlayerConnection>& connections = pMesh->GetAllConnections();
	for (auto& kvPair : connections)
	{
		if (kvPair.second.m_hSteamConnection == pInfo->m_hConn)
		{
			connectionID = kvPair.first;
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
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing in callback");
		SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 0, nullptr, false);

		if (connectionID != -1 && pInfo != nullptr)
		{
			PlayerConnection& plrConnection = connections[connectionID];

			if (TheNetwork != nullptr)
			{
				TheNetwork->GetConnectionManager()->disconnectPlayer(plrConnection.m_userID);
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection %lld", plrConnection.m_userID);

			ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
			const int numSignallingAttempts = 3;
			bool bShouldRetry = plrConnection.m_SignallingAttempts < numSignallingAttempts && serviceConf.retry_signalling;

			bool bWasError = pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || pInfo->m_info.m_eEndReason != k_ESteamNetConnectionEnd_App_Generic;
			plrConnection.SetDisconnected(bWasError, pMesh, bShouldRetry && bWasError);
			
			// the highest slot player, should leave. In most cases, this is the most recently joined player, but this may not be 100% accurate due to backfills.
			// TODO_NGMP: In the future, we should pick the most recently joined by timestamp
			if (bWasError) // only if it wasn't a clean disconnect (e.g. lobby leave)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Determined we didn't connect due to an error, Retrying: %d (currently at %d/%d attempts)", bShouldRetry, plrConnection.m_SignallingAttempts, numSignallingAttempts);
				
				// should we retry signaling?
				if (bShouldRetry)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Retrying...");
					std::shared_ptr<WebSocket> pWS = NGMP_OnlineServicesManager::GetWebSocket();
					if (pWS)
					{
						NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
						NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
						if (pLobbyInterface != nullptr && pAuthInterface != nullptr)
						{
							int64_t myUserID = pAuthInterface->GetUserID();

							// Behavior:
							// disconnected slot userID is higher than ours, do nothing, they will signal
							// disconnected slot userID is lower than ours, we signal
							if ((myUserID > plrConnection.m_userID))
							{
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Send signal start request...");

								pWS->SendData_RequestSignalling(plrConnection.m_userID);
							}
							else
							{
								NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Not sending signal start request, other player should");
							}
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

					NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
					if (pLobbyInterface != nullptr)
					{
						NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][DISCONNECT HANDLER] Performing local removal for user %lld from lobby due to failure to connect\n", plrConnection.m_userID);
						// Local copy to avoid TOCTOU race: check-then-use window
						auto callbackCopy = pLobbyInterface->m_OnCannotConnectToLobbyCallback;
						if (callbackCopy != nullptr)
						{
							callbackCopy();
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
			//assert(false);
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

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Considering Accepting\n", pInfo->m_info.m_szConnectionDescription);

			if (connectionID != -1)
			{
				PlayerConnection& plrConnection = connections[connectionID];

#if _DEBUG
				if (connectionID != -1)
					assert(plrConnection.m_hSteamConnection == k_HSteamNetConnection_Invalid); // not really a bug in this code, but a bug in the test
#endif

				if (pInfo != nullptr)
				{


					plrConnection.UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM CONNECTION] Updating connection from %u to %u on user %lld", plrConnection.m_hSteamConnection, pInfo->m_hConn, plrConnection.m_userID);
					SteamNetworkingSockets()->SetConnectionName(pInfo->m_hConn, std::format("Steam Connection User{}", plrConnection.m_userID).c_str());
					plrConnection.m_hSteamConnection = pInfo->m_hConn;
				}

				// check user is in the lobby, otherwise reject
				NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
				if (pLobbyInterface == nullptr)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Rejecting - Lobby interface is null\n", pInfo->m_info.m_szConnectionDescription);

					NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection 2 %lld", plrConnection.m_userID);
					SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 1000, "Lobby interface is null (Rejected)", false);

					if (TheNetwork != nullptr)
					{
						TheNetwork->GetConnectionManager()->disconnectPlayer(plrConnection.m_userID);
					}

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

					NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection not in lobby %lld", plrConnection.m_userID);
					SteamNetworkingSockets()->CloseConnection(pInfo->m_hConn, 1000, "Player is not in lobby (Rejected)", false);

					if (TheNetwork != nullptr)
					{
						TheNetwork->GetConnectionManager()->disconnectPlayer(plrConnection.m_userID);
					}
				}
			}
			
		}
		else
		{
			// Note that we will get notification when our own connection that
			// we initiate enters this state.
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Entered connecting state\n", pInfo->m_info.m_szConnectionDescription);

			if (connectionID != -1)
			{
				PlayerConnection& plrConnection = connections[connectionID];

#if _DEBUG
				if (connectionID != -1)
					assert(plrConnection.m_hSteamConnection == pInfo->m_hConn);
#endif

				plrConnection.UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
			}
		}
		break;

	case k_ESteamNetworkingConnectionState_FindingRoute:
		// P2P connections will spend a brief time here where they swap addresses
		// and try to find a route.
		if (connectionID != -1 && pInfo != nullptr)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] finding route\n", pInfo->m_info.m_szConnectionDescription);

			PlayerConnection& plrConnection = connections[connectionID];
			plrConnection.UpdateState(EConnectionState::FINDING_ROUTE, pMesh);
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

		if (connectionID != -1)
		{
			PlayerConnection& plrConnection = connections[connectionID];

			plrConnection.UpdateState(EConnectionState::CONNECTED_DIRECT, pMesh);
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
			// SECURITY FIX: Avoid immediate "delete this" which can cause use-after-free
			// when called from async Steam callbacks. Instead, defer deletion to prevent
			// races where CSignalingClient might be destroyed while this object is still
			// being accessed or its owner pointer is being used.
			std::scoped_lock<std::mutex> lock(g_pendingDeletionMutex);
			g_pendingConnSignalingDeletions.push_back(static_cast<void*>(this));
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
		std::shared_ptr<WebSocket> pWS = NGMP_OnlineServicesManager::GetWebSocket();
		if (pWS)
		{
			if (!pWS->AcquireLock())
			{
				return;
			}

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

			pWS->ReleaseLock();
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
		std::shared_ptr<WebSocket> pWS = NGMP_OnlineServicesManager::GetWebSocket();
		if (pWS)
		{
			if (!pWS->AcquireLock())
			{
				return;
			}

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
			pWS->ReleaseLock();

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
							NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING] Sending rejection signal");
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
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Error);

	// Block the status-changed callback from firing while the library is
	// torn down and re-initialized.  Without this guard the callback can
	// be dispatched (e.g. from a previous Tick's RunCallbacks queue) after
	// GameNetworkingSockets_Kill() has freed its internal mutexes but
	// before GameNetworkingSockets_Init() has rebuilt them, resulting in
	// an EXCEPTION_ACCESS_VIOLATION_READ on a null mutex pointer inside
	// mtx_do_lock.
	g_bNetworkMeshDestroying.store(true);

	// try a shutdown
	g_bNetworkMeshDestroying.store(true);
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
	std::string localUserIDStr = std::to_string(localUserID);
	identityLocal.SetGenericString(localUserIDStr.c_str());

	if (identityLocal.IsInvalid())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "SteamNetworkingIdentity is invalid");
		return;
	}

	// WORKAROUND: Set IP_AllowWithoutAuth to bypass problematic X.509 certificate processing
	// This works around an OpenSSL bug in i2d_X509_CERT_AUX that causes division by zero
	// when processing certificates with empty or malformed auxiliary data
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 2);

	// initialize Steam Sockets
	SteamDatagramErrMsg errMsg;
	bool bInitSuccess = false;

	// Use Structured Exception Handling to catch division by zero in OpenSSL
	__try
	{
		bInitSuccess = GameNetworkingSockets_Init(&identityLocal, errMsg);
	}
	__except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init caught EXCEPTION_INT_DIVIDE_BY_ZERO in OpenSSL certificate processing. Attempting fallback with unencrypted mode.");
		bInitSuccess = false;
		strcpy_s(errMsg, "Division by zero in OpenSSL certificate processing");
	}

	// If initialization failed, try fallback with unencrypted mode
	if (!bInitSuccess)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init failed with error: %s. Attempting fallback initialization with unencrypted mode.", errMsg);
		
		// Enable unencrypted mode to completely bypass certificate processing
		SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_Unencrypted, 3); // 3 = Required (fail if peer requires encryption)
		
		__try
		{
			bInitSuccess = GameNetworkingSockets_Init(&identityLocal, errMsg);
		}
		__except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init fallback also caught EXCEPTION_INT_DIVIDE_BY_ZERO. Unable to initialize networking.");
			bInitSuccess = false;
		}
	}

	if (!bInitSuccess)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init failed after all attempts. Error: %s", errMsg);
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "GameNetworkingSockets_Init succeeded");

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
	g_bNetworkMeshDestroying.store(false);

	// Library is fully re-initialized and the callback is registered;
	// it is now safe to allow OnSteamNetConnectionStatusChanged to run.
	g_bNetworkMeshDestroying.store(false);

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

	int localPort = 0;

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
	ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
	bool bDoImmediateFlushPerFrame = serviceConf.network_do_immediate_flush_per_frame;

	if (bDoImmediateFlushPerFrame)
	{
		for (auto& connectionData : m_mapConnections)
		{
			SteamNetworkingSockets()->FlushMessagesOnConnection(connectionData.second.m_hSteamConnection);
		}
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

int NetworkMesh::SendGamePacket(void* pBuffer, uint32_t totalDataSize, int64_t user_id)
{
	if (!pBuffer || totalDataSize == 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[SendGamePacket] CRITICAL: Received null pBuffer or zero size from user %lld, size=%u", static_cast<long long>(user_id), totalDataSize);
		return -3;  // Invalid buffer
	}

	// Thread safety: Lock connection map during access
	std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);
	
	auto it = m_mapConnections.find(user_id);
	if (it != m_mapConnections.end())
	{
		return it->second.SendGamePacket(pBuffer, totalDataSize);
	}
	
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[SendGamePacket] Connection not found for user %lld", static_cast<long long>(user_id));
	return -2;
}


void NetworkMesh::SendACPacket(uint32_t userID, const void* pData, uint32_t dataLen)
{
    if (dataLen == 0)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Cannot send empty AC packet to user %u", userID);
        return;
    }

    if (pData == nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Cannot send AC packet with null data to user %u", userID);
        return;
    }

	// Thread safety: Lock connection map during access
	std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);

    if (m_mapConnections.contains(userID))
    {
        m_mapConnections[userID].SendACPacket(pData, dataLen);
    }
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Send Packet ERR - user %u not found in connections", userID);
	}
}

void NetworkMesh::StartConnectionSignalling(const char* szMiddlewareID, int64_t remoteUserID, uint16_t preferredPort)
{
	// Thread safety: Lock connection map during access
	std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);

	if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
	{
		// TODO_EOS: if we already have a connection to this use, drop it, having a single-direction connection will break signalling
		AnticheatPlugInterface::StartSignalling(szMiddlewareID, remoteUserID);

        // create a local user type
        {
            std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);
            m_mapConnections[remoteUserID] = PlayerConnection(remoteUserID, szMiddlewareID);

            // add attempt
            ++m_mapConnections[remoteUserID].m_SignallingAttempts;
        }
	}
	else
	{
        // if we already have a connection to this use, drop it, having a single-direction connection will break signalling
        auto it = m_mapConnections.find(remoteUserID);
        if (it != m_mapConnections.end())
        {
            if (it->second.m_hSteamConnection != k_HSteamNetConnection_Invalid)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection %lld, new connection is being negotiated", remoteUserID);
                SteamNetworkingSockets()->CloseConnection(it->second.m_hSteamConnection, 0, "Client Disconnecting Gracefully (new connection being negotiated)", false);

                if (TheNetwork != nullptr)
                {
                    TheNetwork->GetConnectionManager()->disconnectPlayer(remoteUserID);
                }
            }

            NetworkLog(ELogVerbosity::LOG_RELEASE, "[ERASE 3] Removing user %lld", it->second.m_userID);
            m_mapConnections.erase(it);
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
        std::string remoteUserIDStr = std::to_string(remoteUserID);
        identityRemote.SetGenericString(remoteUserIDStr.c_str());

        if (identityRemote.IsInvalid())
        {
            // TODO_STEAM: Handle this better
            NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - SteamNetworkingIdentity is invalid");
            return;
        }

        std::vector<SteamNetworkingConfigValue_t > vecOpts;

        ServiceConfig& serviceConf = pOnlineServicesMgr->GetServiceConfig();

        int g_nLocalPort = 0;

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
        {
            std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);
            m_mapConnections[remoteUserID] = PlayerConnection(remoteUserID, hSteamConnection);

            // add attempt
            ++m_mapConnections[remoteUserID].m_SignallingAttempts;
        }
	}
	
}


void NetworkMesh::DisconnectUser(int64_t remoteUserID)
{
	std::lock_guard<std::recursive_mutex> lock(m_mapConnectionsMutex);
	
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Dumping all Steam connections");
	for (auto& kvPair : m_mapConnections)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Dumped steam connection, Handle %u User %lld (%lld)", kvPair.second.m_hSteamConnection, kvPair.second.m_userID, kvPair.first);
	}

	if (m_mapConnections.find(remoteUserID) != m_mapConnections.end())
	{
		if (m_mapConnections[remoteUserID].m_hSteamConnection != k_HSteamNetConnection_Invalid)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Closing connection %lld", remoteUserID);
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] Steam connection handle is %u", m_mapConnections[remoteUserID].m_hSteamConnection);

			SteamNetworkingSockets()->CloseConnection(m_mapConnections[remoteUserID].m_hSteamConnection, 0, "Client Disconnecting Gracefully (Got EWebSocketMessageID::NETWORK_CONNECTION_DISCONNECT_PLAYER from service)", false);
			if (TheNetwork != nullptr)
			{
				TheNetwork->GetConnectionManager()->disconnectPlayer(remoteUserID);
			}
		}


		if (TheNGMPGame && !TheNGMPGame->isGameInProgress())
		{
			for (auto it = m_mapConnections.begin(); it != m_mapConnections.end(); )
			{
				if (it->second.m_userID == remoteUserID)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[ERASE] Removing user %lld", it->second.m_userID);
					it = m_mapConnections.erase(it);
					break;
				}
				else
				{
					++it;
				}
			}
		}
	}
}

void NetworkMesh::Disconnect()
{
	if (m_bDisconnected)
		return;

	m_bDisconnected = true;

	// Set flag to prevent callbacks from executing during teardown
	g_bNetworkMeshDestroying.store(true);

    // close every connection
    for (auto& connectionData : m_mapConnections)
    {
		connectionData.second.Close();
    }

    // clear map
    m_mapConnections.clear();

	if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
	{
		// Nothing to do here, Close above calls AnticheatPlugInterface::DisconnectPlayer
	}
	else
	{
		// Unregister the global callback to prevent new callbacks from being queued
		if (SteamNetworkingUtils())
		{
			SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(nullptr);
		}

		if (SteamNetworkingSockets())
		{
			SteamNetworkingSockets()->CloseListenSocket(m_hListenSock);
		}

		// invalidate socket
		m_hListenSock = k_HSteamNetConnection_Invalid;

		// tear down steam sockets
		GameNetworkingSockets_Kill();
	}

	// Reset flag after teardown is complete
	g_bNetworkMeshDestroying.store(false);
}

void NetworkMesh::Tick()
{
	if (!AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
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
	}

	// update connection histograms
	for (auto& kvPair : m_mapConnections)
	{
		PlayerConnection& conn = kvPair.second;
		conn.UpdateLatencyHistogram();
	}

	// the game transport isn't created until the game begins, but we want to transfer AC packets in the lobby first, so consider this a liteupdate
	if (TheNGMPGame != nullptr && !TheNGMPGame->isGameInProgress())
	{
		for (auto& kvPair : m_mapConnections)
		{
			kvPair.second.LiteUpdateForAC();
		}
	}
}

void PlayerConnection::LiteUpdateForAC()
{
	if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
	{
		// EOS: Nothing to do here, AC packets are handled internally when MW is handling it
	}
	else
	{
		SteamNetworkingMessage_t* pMsg[255] = { nullptr };
		int numPackets = Recv(pMsg);

		if (numPackets <= 0)
			return;

		if (numPackets > static_cast<int>(std::size(pMsg)))
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE,
				"Game Packet Recv: numPackets (%d) > pMsg capacity (%zu), clamping",
				numPackets, std::size(pMsg));
			numPackets = static_cast<int>(std::size(pMsg));
		}

		for (int iPacket = 0; iPacket < numPackets; ++iPacket)
		{
			SteamNetworkingMessage_t* msg = pMsg[iPacket];
			if (!msg)
			{
				// CRITICAL BUG FIX: Don't return early - continue loop to release remaining messages
				// Skipping null entry but continue processing others
				NetworkLog(ELogVerbosity::LOG_DEBUG, "[AC PACKET] Received null message at index %d", iPacket);
				continue;
			}

			const uint32_t numBytes = msg->m_cbSize;

			// is it an AC packet?
			// TODO_AC: Improve detection, just add a 'msg type' to the start of the packet
			std::vector<byte> vecData;
			vecData.resize(numBytes);
			memcpy(vecData.data(), msg->GetData(), numBytes);

			// Check minimum packet size for AC header
			if (numBytes >= sizeof(ENetworkChannel))
			{
				ENetworkChannel netChannel = (ENetworkChannel)vecData[0];
				if (netChannel == ENetworkChannel::NETWORK_CHANNEL_AC)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Received AC message of size %u from user %lld", numBytes, static_cast<long long>(m_userID));


					// remove header
					// TODO_AC: Optimize this
					std::vector<byte> vecDataAC;
					vecDataAC.resize(numBytes - sizeof(ENetworkChannel));
					memcpy(vecDataAC.data(), (char*)msg->GetData() + sizeof(ENetworkChannel), numBytes - sizeof(ENetworkChannel));

					AnticheatPlugInterface::AC_NetworkMessageArrived(m_userID, vecDataAC.data(), numBytes - sizeof(ENetworkChannel));
					msg->Release();
					continue;
				}
			}
			else if (numBytes != -1 && numBytes < sizeof(ENetworkChannel))
			{
				// Malformed AC packet - too small for header
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Dropping malformed AC packet - size %u is less than header size 3 from user %lld", numBytes, static_cast<long long>(m_userID));
				msg->Release();
				continue;
			}

			// not an AC packet, we dont care
			NetworkLog(ELogVerbosity::LOG_DEBUG, "[AC PACKET] Received NON AC message");
			msg->Release();
		}
	}
}

PlayerConnection::PlayerConnection(int64_t userID, HSteamNetConnection hSteamConnection)
{
	m_userID = userID;
	m_ConnectionType = EConnectionType::BuiltIn_ValveSockets;
	
	// no connection yet
	m_hSteamConnection = hSteamConnection;
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM CONNECTION] Attaching connection %u to user %lld", hSteamConnection, userID);

	SteamNetworkingSockets()->SetConnectionName(hSteamConnection, std::format("Steam Connection User{}", userID).c_str());

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
	if (pMesh != nullptr)
	{
		pMesh->RegisterConnectivity(userID);
	}
}

PlayerConnection::PlayerConnection(int64_t userID, const char* szMiddlewareID)
{
    m_userID = userID;
    m_ConnectionType = EConnectionType::MiddlewarePluginGeneric;

    // no connection yet
    m_hSteamConnection = k_HSteamNetConnection_Invalid;
	m_strMiddlewareID = std::string(szMiddlewareID);

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[MIDDLEWARE CONNECTION] Attaching connection %s to user %lld", szMiddlewareID, userID);

    NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
    if (pMesh != nullptr)
    {
        pMesh->RegisterConnectivity(userID);
    }
}

int PlayerConnection::SendGamePacket(void* pBuffer, uint32_t totalDataSize)
{
    if (totalDataSize == 0)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Cannot send empty game packet to user %lld", m_userID);
        return (int)k_EResultFail;
    }

    if (pBuffer == nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Cannot send game packet with null buffer to user %lld", m_userID);
        return (int)k_EResultFail;
    }

	if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
	{
		// TODO_EOS: Determine best reliability
		AnticheatPlugInterface::SendPacket(m_strMiddlewareID.c_str(), m_userID, pBuffer, totalDataSize, ENetworkChannels::Game, EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED);
	}
	else
	{
        if (m_hSteamConnection == k_HSteamNetConnection_Invalid)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Cannot send game packet - connection is invalid for user %lld", m_userID);
            return (int)k_EResultFail;
        }



	}

    ENetworkChannel netChannel = ENetworkChannel::NETWORK_CHANNEL_GAME;
    std::vector<BYTE> vecData;
    vecData.resize(totalDataSize + sizeof(ENetworkChannel));
    memcpy(vecData.data() + sizeof(ENetworkChannel), pBuffer, totalDataSize);
    vecData[0] = (BYTE)netChannel;

    int sendFlags = k_nSteamNetworkingSend_Reliable | k_nSteamNetworkingSend_AutoRestartBrokenSession; // default from last patch

    ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
    int netSendFlags = serviceConf.network_send_flags;

	NetworkLog(ELogVerbosity::LOG_DEBUG, "[GAME PACKET] Sending msg of size %ld to user %lld\n", totalDataSize, m_userID);
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		m_hSteamConnection, vecData.data(), vecData.size(), sendFlags, nullptr);

	if (r != k_EResultOK)
	{
        if (netSendFlags != -1)
        {
            if (netSendFlags == 0)
            {
                sendFlags = k_nSteamNetworkingSend_Unreliable;
            }
            else if (netSendFlags == 1)
            {
                sendFlags = k_nSteamNetworkingSend_UnreliableNoNagle;
            }
            else if (netSendFlags == 2)
            {
                sendFlags = k_nSteamNetworkingSend_UnreliableNoDelay;
            }
            else if (netSendFlags == 3)
            {
                sendFlags = k_nSteamNetworkingSend_Reliable;
            }
            else if (netSendFlags == 4)
            {
                sendFlags = k_nSteamNetworkingSend_ReliableNoNagle;
            }
        }

        NetworkLog(ELogVerbosity::LOG_DEBUG, "[GAME PACKET] Sending msg of size %ld to user %lld\n", totalDataSize, m_userID);
        EResult r = SteamNetworkingSockets()->SendMessageToConnection(
            m_hSteamConnection, pBuffer, (int)totalDataSize, sendFlags, nullptr);

        if (r != k_EResultOK)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Failed to send, err code was %d", r);
        }

        return (int)r;
	}

	return (int)k_EResultFail;
}


void PlayerConnection::SendACPacket(const void* pData, uint32_t dataLen)
{
	if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
	{
		// nothing to do, handled internally in plugin

	}
	else
	{
        if (m_hSteamConnection == k_HSteamNetConnection_Invalid)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Cannot send AC packet - connection is invalid for user %ld", m_userID);
            return;
        }

        ENetworkChannel netChannel = ENetworkChannel::NETWORK_CHANNEL_AC;
        std::vector<BYTE> vecData;
        vecData.resize(dataLen + sizeof(ENetworkChannel));
        memcpy(vecData.data() + sizeof(ENetworkChannel), pData, dataLen);
        vecData[0] = (BYTE)netChannel;

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Sending AC msg of size %ld to user %ld\n", dataLen, m_userID);
        EResult r = SteamNetworkingSockets()->SendMessageToConnection(m_hSteamConnection, vecData.data(), vecData.size(), k_nSteamNetworkingSend_Reliable, nullptr);

        if (r != k_EResultOK)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Failed to send, err code was %d", r);
        }
	}
}

void PlayerConnection::UpdateLatencyHistogram()
{
	int histogram_duration = 20000;

	if (NGMP_OnlineServicesManager::GetInstance() != nullptr)
	{
		ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
		histogram_duration = serviceConf.network_mesh_histogram_duration;
	}


	// update latency history
	int currLatency = GetLatency();
#if defined(GENERALS_ONLINE_HIGH_FPS_SERVER)
	const int connectionHistoryLength = histogram_duration / 16; // ~20 sec worth of frames at 60fps (default)
#else
	const int connectionHistoryLength = histogram_duration / 33; // ~20 sec worth of frames at 30fps (default)
#endif

	if (m_vecLatencyHistory.size() >= connectionHistoryLength)
	{
		m_vecLatencyHistory.erase(m_vecLatencyHistory.begin());
	}
	m_vecLatencyHistory.push_back(currLatency);

	// Sample connection quality into rolling history.
	// Prefer SNS local quality, then remote quality, then fall back to manual in/out packet rate ratio.
	if (m_hSteamConnection != k_HSteamNetConnection_Invalid)
	{
		SteamNetConnectionRealTimeStatus_t status;
		if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(m_hSteamConnection, &status, 0, nullptr) == k_EResultOK)
		{
			float sample = -1.0f;
			if (status.m_flConnectionQualityLocal >= 0.0f)
			{
				sample = status.m_flConnectionQualityLocal;
			}
			else if (status.m_flConnectionQualityRemote >= 0.0f)
			{
				sample = status.m_flConnectionQualityRemote;
			}
			else if (status.m_flOutPacketsPerSec > 0.0f)
			{
				sample = status.m_flInPacketsPerSec / status.m_flOutPacketsPerSec;
			}

			if (sample > 1.0f) sample = 1.0f;

			if (sample >= 0.0f)
			{
				if (m_vecQualityHistory.size() >= connectionHistoryLength)
					m_vecQualityHistory.erase(m_vecQualityHistory.begin());
				m_vecQualityHistory.push_back(sample);
			}
		}
	}
}

void PlayerConnection::Close()
{
	if (m_ConnectionType == EConnectionType::BuiltIn_ValveSockets)
	{
        if (SteamNetworkingSockets())
        {
            SteamNetworkingSockets()->CloseConnection(m_hSteamConnection, 0, "Client Disconnecting Gracefully", false);
        }
	}
	else
	{
		if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
		{
			AnticheatPlugInterface::DisconnectPlayer(m_strMiddlewareID.c_str(), m_userID);
        }
	}
    
    if (TheNetwork != nullptr)
    {
        TheNetwork->GetConnectionManager()->disconnectPlayer(m_userID);
    }
}

bool PlayerConnection::IsIPV4()
{
	if (m_hSteamConnection == k_HSteamNetConnection_Invalid)
		return false;

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
		NetworkLog(ELogVerbosity::LOG_DEBUG, "[DISC] Recv Result %d from user %lld", r, m_userID);
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[DISC] Recv Failed 1 from user %lld", m_userID);
	}

	return r;
}


std::string PlayerConnection::GetStats()
{
	if (m_hSteamConnection == k_HSteamNetConnection_Invalid)
		return "(disconnected)";

	char szBuf[2048] = { 0 };
	int ret = SteamNetworkingSockets()->GetDetailedConnectionStatus(m_hSteamConnection, szBuf, 2048);

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM] PlayerConnection::GetStats returned %d", ret);
	return std::string(szBuf);
}


std::string PlayerConnection::GetConnectionType()
{
	if (m_hSteamConnection == k_HSteamNetConnection_Invalid)
		return "(disconnected)";

	char szBuf[2048] = { 0 };
	int ret = SteamNetworkingSockets()->GetConnectionType(m_hSteamConnection, szBuf, 2048);
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[STEAM] PlayerConnection::GetConnectionType returned %d", ret);
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
	// TODO_EOS
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

	// Save values we need after the callback: the callback can erase this
	// PlayerConnection from the mesh's map (UAF), so we must not access
	// member variables after UpdateState fires the external callback.
	const HSteamNetConnection savedHandle = m_hSteamConnection;
	const int64_t savedUserID = m_userID;

	// Invalidate the handle before firing the callback so any re-entrant
	// query sees the connection as already gone.
	m_hSteamConnection = k_HSteamNetConnection_Invalid;

	// Dont update backend until we're actually done
	if (!bIsRetrying)
	{
		UpdateState(m_State, pOwningMesh);  // may erase *this from the map
	}

	// Use saved stack values � do NOT touch any member after this point.
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM CONNECTION] Setting connection %u to disconnected/invalid on user %lld", savedHandle, savedUserID);
	if (SteamNetworkingSockets())
	{
		SteamNetworkingSockets()->SetConnectionName(savedHandle, std::format("Steam Connection User{}", savedUserID).c_str());
	}
}

int PlayerConnection::GetLatency()
{
	if (m_ConnectionType == EConnectionType::MiddlewarePluginGeneric)
	{
		return AnticheatPlugInterface::GetConnectionLatencyForUser(m_strMiddlewareID.c_str(), m_userID);
	}
	else
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
	}

	return -1;
}

int PlayerConnection::GetJitter()
{
	int sumDelta = 0;
	int count = 0;
	int prev = -1;
	for (int sample : m_vecLatencyHistory)
	{
		if (sample >= 0)
		{
			if (prev >= 0)
			{
				sumDelta += std::abs(sample - prev);
				++count;
			}
			prev = sample;
		}
		else
		{
			prev = -1; // gap in valid data
		}
	}

	if (count < 10)
		return -1;

	return sumDelta / count;
}

float PlayerConnection::GetConnectionQuality()
{
	if (!m_vecQualityHistory.empty())
	{
		float sum = 0.0f;
		for (float r : m_vecQualityHistory)
			sum += r;
		return sum / static_cast<float>(m_vecQualityHistory.size());
	}

	return -1.0f;
}

int PlayerConnection::ComputeConnectionScore()
{
	// TODO_EOS: need to impl jitter etc again
	const int latency = GetLatency();
	const int jitter = GetJitter();
	const float quality = GetConnectionQuality();   // packet delivery ratio [0..1]

	// Stability-first weighting
	static constexpr float k_subScoreFloor = 0.01f;
	static constexpr float k_latencyWeight = 0.22f;
	static constexpr float k_jitterWeight = 0.38f;
	static constexpr float k_reliabilityWeight = 0.40f;

	float weightedLogSum = 0.0f;
	float activeWeightSum = 0.0f;

	if (latency >= 0)
	{
		// 10ms and below are treated as full score. Above that, roughly:
		// 400ms -> composite 75, 800ms -> composite 50 when other metrics are perfect.
		int effectiveLatency = (std::max)(latency - 10, 0);
		float latFactor = std::clamp(1.0f - static_cast<float>(effectiveLatency) / 1590.0f, 0.0f, 1.0f);
		float latencyScore = (std::max)(std::powf(latFactor, 4.545f), k_subScoreFloor);
		weightedLogSum += k_latencyWeight * std::logf(latencyScore);
		activeWeightSum += k_latencyWeight;
	}

	if (jitter >= 0)
	{
		// 50ms -> composite 75, 100ms -> composite 50 when other metrics are perfect.
		float jitFactor = std::clamp(1.0f - static_cast<float>(jitter) / 200.0f, 0.0f, 1.0f);
		float jitterScore = (std::max)(std::powf(jitFactor, 2.632f), k_subScoreFloor);
		weightedLogSum += k_jitterWeight * std::logf(jitterScore);
		activeWeightSum += k_jitterWeight;
	}

	if (quality >= 0.0f)
	{
		// 90% -> composite 75, 80% -> composite 50 when other metrics are perfect.
		float relFactor = std::clamp(2.5f * quality - 1.5f, 0.0f, 1.0f);
		float reliabilityScore = (std::max)(std::powf(relFactor, 2.5f), k_subScoreFloor);
		weightedLogSum += k_reliabilityWeight * std::logf(reliabilityScore);
		activeWeightSum += k_reliabilityWeight;
	}

	if (activeWeightSum <= 0.0f)
	{
		return -1;
	}

	float composite = std::expf(weightedLogSum / activeWeightSum);
	return static_cast<int>(std::round(composite * 100.0f));
}
