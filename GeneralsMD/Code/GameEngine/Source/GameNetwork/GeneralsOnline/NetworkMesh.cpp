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

// Called when a connection undergoes a state transition
void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* pInfo)
{
	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();

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

		if (pPlayerConnection != nullptr)
		{
			pPlayerConnection->SetDisconnected(pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally || pInfo->m_info.m_eEndReason != k_ESteamNetConnectionEnd_App_Generic, pMesh);
			
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

			assert(pPlayerConnection->m_hSteamConnection == k_HSteamNetConnection_Invalid); // not really a bug in this code, but a bug in the test

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Accepting\n", pInfo->m_info.m_szConnectionDescription);
			pPlayerConnection->m_hSteamConnection = pInfo->m_hConn;
			SteamNetworkingSockets()->AcceptConnection(pInfo->m_hConn);

			pPlayerConnection->UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
		}
		else
		{
			// Note that we will get notification when our own connection that
			// we initiate enters this state.
			assert(pPlayerConnection->m_hSteamConnection == pInfo->m_hConn);
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] Entered connecting state\n", pInfo->m_info.m_szConnectionDescription);

			pPlayerConnection->UpdateState(EConnectionState::CONNECTING_DIRECT, pMesh);
		}
		break;

	case k_ESteamNetworkingConnectionState_FindingRoute:
		// P2P connections will spend a brief time here where they swap addresses
		// and try to find a route.
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING][%s] finding route\n", pInfo->m_info.m_szConnectionDescription);

		pPlayerConnection->UpdateState(EConnectionState::FINDING_ROUTE, pMesh);
		break;

	case k_ESteamNetworkingConnectionState_Connected:
		// We got fully connected
		assert(pInfo->m_hConn == pPlayerConnection->m_hSteamConnection); // We don't initiate or accept any other connections, so this should be out own connection
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
		
		pPlayerConnection->UpdateState(EConnectionState::CONNECTED_DIRECT, pMesh);

		break;

	default:
		assert(false);
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
		std::string const m_sPeerIdentity; // Save off the string encoding of the identity we're talking to

		ConnectionSignaling(CSignalingClient* owner, const char* pszPeerIdentity)
			: m_pOwner(owner)
			, m_sPeerIdentity(pszPeerIdentity)
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

			// We'll use a dumb hex encoding.
			std::string signal;
			signal.reserve(m_sPeerIdentity.length() + cbMsg * 2 + 4);
			signal.append(m_sPeerIdentity);
			signal.push_back(' ');
			for (const uint8_t* p = (const uint8_t*)pMsg; cbMsg > 0; --cbMsg, ++p)
			{
				static const char hexdigit[] = "0123456789abcdef";
				signal.push_back(hexdigit[*p >> 4U]);
				signal.push_back(hexdigit[*p & 0xf]);
			}
			signal.push_back('\n');

			m_pOwner->Send(signal);
			return true;
		}

		// Self destruct.  This will be called by SteamNetworkingSockets when it's done with us.
		virtual void Release() override
		{
			delete this;
		}
	};

	ISteamNetworkingSockets* const m_pSteamNetworkingSockets;
	std::deque< std::string > m_queueSend;

	std::recursive_mutex sockMutex;

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
	void Send(const std::string& s)
	{
		assert(s.length() > 0 && s[s.length() - 1] == '\n'); // All of our signals are '\n'-terminated

		sockMutex.lock();

		// If we're getting backed up, delete the oldest entries.  Remember,
		// we are only required to do best-effort delivery.  And old signals are the
		// most likely to be out of date (either old data, or the client has already
		// timed them out and queued a retry).
		while (m_queueSend.size() > 32)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "Signaling send queue is backed up.  Discarding oldest signals\n");
			m_queueSend.pop_front();
		}

		m_queueSend.push_back(s);
		sockMutex.unlock();
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

		return new ConnectionSignaling(this, sIdentityPeer.c_str());
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
		WebSocket* pWS = NGMP_OnlineServicesManager::GetInstance()->GetWebSocket();

		// Drain the socket
		sockMutex.lock();

		// Flush send queue
		while (!m_queueSend.empty())
		{
			const std::string& s = m_queueSend.front();

			pWS->SendData_Signalling(s);
			m_queueSend.pop_front();
		}

		// Release the lock now.  See the notes below about why it's very important
		// to release the lock early and not hold it while we try to dispatch the
		// received callbacks.
		sockMutex.unlock();

		// Now dispatch any buffered signals
		if (!pWS->m_pendingSignals.empty())
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[SIGNAL] PROCESS SIGNAL!");
			while (!pWS->m_pendingSignals.empty())
			{
				// Get the next signal
				std::string m_sBufferedData = pWS->m_pendingSignals.front();
				pWS->m_pendingSignals.pop();

				size_t l = m_sBufferedData.length();
			
				// process signal
				// Locate the space that seperates [from] [payload]
				size_t spc = m_sBufferedData.find(' ');
				if (spc != std::string::npos && spc < l)
				{

					// Hex decode the payload.  As it turns out, we actually don't
					// need the sender's identity.  The payload has everything needed
					// to process the message.  Maybe we should remove it from our
					// dummy signaling protocol?  It might be useful for debugging, tho.
					std::string data; data.reserve((l - spc) / 2);
					for (size_t i = spc + 1; i + 2 <= l; i += 2)
					{
						int dh = HexDigitVal(m_sBufferedData[i]);
						int dl = HexDigitVal(m_sBufferedData[i + 1]);
						if ((dh | dl) & ~0xf)
						{
							// Failed hex decode.  Not a bug in our code here, but this is just example code, so we'll handle it this way
							assert(!"Failed hex decode from signaling server?!");
							continue;
						}
						data.push_back((char)(dh << 4 | dl));
					}

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
					m_pSteamNetworkingSockets->ReceivedP2PCustomSignal(data.c_str(), (int)data.length(), &context);
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
	TheWritableGlobalData->m_networkRunAheadSlack = 15;

	int64_t localUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();

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
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_STUN_ServerList, "stun:stun.cloudflare.com:53");

	// comma seperated setting lists
	const char* turnList = "turn:turn.cloudflare.com:3478?transport=udp";
	const char* userList = "TODO_STEAM";
	const char* passList = "TODO_STEAM";

	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_ServerList, turnList);
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_UserList, userList);
	SteamNetworkingUtils()->SetGlobalConfigValueString(k_ESteamNetworkingConfig_P2P_TURN_PassList, passList);

	// Allow sharing of any kind of ICE address.
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_All);

	m_hListenSock = k_HSteamListenSocket_Invalid;
	
	// create signalling service
	m_pSignaling = new CSignalingClient(SteamNetworkingSockets());
	if (m_pSignaling == nullptr)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "CreateTrivialSignalingClient failed.  %s", errMsg);
		return;
	}

	SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(OnSteamNetConnectionStatusChanged);


	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_LogLevel_P2PRendezvous, k_ESteamNetworkingSocketsDebugOutputType_Debug);
	SteamNetworkingUtils()->SetDebugOutputFunction(ESteamNetworkingSocketsDebugOutputType::k_ESteamNetworkingSocketsDebugOutputType_Debug, [](ESteamNetworkingSocketsDebugOutputType nType, const char* pszMsg)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[STEAM NETWORKING LOGFUNC] %s", pszMsg);
		});

	// create sockets
	SteamNetworkingConfigValue_t opt;
	opt.SetInt32(k_ESteamNetworkingConfig_SymmetricConnect, 1); // << Note we set symmetric mode on the listen socket
	m_hListenSock = SteamNetworkingSockets()->CreateListenSocketP2P(NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort(), 1, &opt);

	if (m_hListenSock == k_HSteamListenSocket_Invalid)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "CreateListenSocketP2P failed. Sock was invalid");
	}
}

void NetworkMesh::UpdateConnectivity(PlayerConnection* connection)
{
	nlohmann::json j;
	j["target"] = connection->m_userID;
	j["outcome"] = connection->GetState();
	std::string strPostData = j.dump();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("ConnectionOutcome", true);
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

void NetworkMesh::SyncConnectionListToLobbyMemberList(std::vector<LobbyMemberEntry> vecLobbyMembers)
{
	// TODO_STEAM
	/*
	std::vector<int64_t> vecConnectionsToRemove;
	for (auto& connectionData : m_mapConnections)
	{
		int64_t thisConnectionUserID = connectionData.first;

		// do we have a lobby member for it? otherwise disconnect
		bool bFoundLobbyMemberForConnection = false;
		for (LobbyMemberEntry& lobbyMember : vecLobbyMembers)
		{
			if (lobbyMember.IsHuman())
			{
				if (lobbyMember.user_id == thisConnectionUserID)
				{
					bFoundLobbyMemberForConnection = true;
					break;
				}
			}
		}

		if (!bFoundLobbyMemberForConnection)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "We have a connection open to user id %d, but no lobby member exists, disconnecting", thisConnectionUserID);
			vecConnectionsToRemove.push_back(thisConnectionUserID);
		}
	}

	// now delete + remove from map
	for (int64_t userIDToDisconnect : vecConnectionsToRemove)
	{
		if (m_mapConnections.find(userIDToDisconnect) != m_mapConnections.end())
		{
			if (m_mapConnections[userIDToDisconnect].m_peer != nullptr)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "[DC] enet_peer_disconnect_now %lld", userIDToDisconnect);

				uint32_t localUserID = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID();
				enet_peer_disconnect_now(m_mapConnections[userIDToDisconnect].m_peer, localUserID);
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE, "[ERASE 1] Removing user %lld", m_mapConnections[userIDToDisconnect].m_userID);
			m_mapConnections.erase(userIDToDisconnect);
		}
	}
	*/
}

void NetworkMesh::ConnectToSingleUser(LobbyMemberEntry& lobbyMember, bool bIsReconnect)
{
	// never connect to ourself
	if (lobbyMember.user_id == NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetUserID())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - Skipping connection to user %lld - user is local", lobbyMember.user_id);
		return;
	}

	SteamNetworkingIdentity identityRemote;
	identityRemote.Clear();
	identityRemote.SetGenericString(std::to_string(lobbyMember.user_id).c_str());

	if (identityRemote.IsInvalid())
	{
		// TODO_STEAM: Handle this better
		NetworkLog(ELogVerbosity::LOG_RELEASE, "NetworkMesh::ConnectToSingleUser - SteamNetworkingIdentity is invalid");
		return;
	}

	std::vector<SteamNetworkingConfigValue_t > vecOpts;

	int g_nVirtualPortRemote = lobbyMember.preferredPort;

	// Our remote and local port don't match, so we need to set it explicitly
	if (g_nVirtualPortRemote != NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort())
	{
		SteamNetworkingConfigValue_t opt;
		opt.SetInt32(k_ESteamNetworkingConfig_LocalVirtualPort, NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort());
		vecOpts.push_back(opt);
	}

	// Set symmetric connect mode
	SteamNetworkingConfigValue_t opt;
	opt.SetInt32(k_ESteamNetworkingConfig_SymmetricConnect, 1);
	vecOpts.push_back(opt);
	NetworkLog(ELogVerbosity::LOG_DEBUG, "Connecting to '%s' in symmetric mode, virtual port %d, from local virtual port %d.\n",
		SteamNetworkingIdentityRender(identityRemote).c_str(), g_nVirtualPortRemote,
		NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().GetOpenPort());

	// create a signaling object for this connection
	SteamNetworkingErrMsg errMsg;
	ISteamNetworkingConnectionSignaling* pConnSignaling = m_pSignaling->CreateSignalingForConnection( identityRemote, errMsg);

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
	m_mapConnections[lobbyMember.user_id] = PlayerConnection(lobbyMember.user_id, hSteamConnection);
}

bool NetworkMesh::ConnectToMesh(LobbyEntry& lobby)
{
	for (LobbyMemberEntry& lobbyMember : lobby.members)
	{
		if (lobbyMember.IsHuman())
		{
			ConnectToSingleUser(lobbyMember);
		}
	}

	return true;
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

	// Recv
	for (auto& connectionData : m_mapConnections)
	{
		connectionData.second.Recv();
	}
}


PlayerConnection::PlayerConnection(int64_t userID, HSteamNetConnection hSteamConnection)
{
	m_userID = userID;
	
	// no connection yet
	m_hSteamConnection = hSteamConnection;

	NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();
	if (pMesh != nullptr)
	{
		pMesh->UpdateConnectivity(this);
	}
}

int PlayerConnection::SendGamePacket(void* pBuffer, uint32_t totalDataSize)
{
	NetworkLog(ELogVerbosity::LOG_DEBUG, "[GAME PACKET] Sending msg of size %ld\n", totalDataSize);
	EResult r = SteamNetworkingSockets()->SendMessageToConnection(
		m_hSteamConnection, pBuffer, (int)totalDataSize, k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);

	if (r != k_EResultOK)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Failed to send, err code was %d", r);
	}

	return (int)r;
}


void PlayerConnection::Recv()
{
	if (m_hSteamConnection != k_HSteamNetConnection_Invalid)
	{
		// would be kinda weird for this to be null, since recv is called from NetworkMesh...
		NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetNetworkMesh();

		if (pMesh != nullptr)
		{
			SteamNetworkingMessage_t* pMsg[255];
			int r = SteamNetworkingSockets()->ReceiveMessagesOnConnection(m_hSteamConnection, pMsg, 255);

			// TODO_STEAM: Handle < 0, its an error
			if (r > 0) // received something
			{
				for (int i = 0; i < r; ++i)
				{
					NetworkLog(ELogVerbosity::LOG_RELEASE, "[GAME PACKET] Received message of size %d\n", pMsg[i]->m_cbSize);

					// assume game packet for now
					CBitStream* bitstream = new CBitStream(pMsg[i]->m_cbSize, pMsg[i]->m_pData, pMsg[i]->m_cbSize);
					
					pMesh->m_queueQueuedGamePackets.push(QueuedGamePacket{ bitstream, m_userID });

					// Free message struct and buffer.
					pMsg[i]->Release();
				}

			}
		}
	}
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


	std::string strDisplayName = "Unknown User";
	auto currentLobby = NGMP_OnlineServicesManager::GetInstance()->GetLobbyInterface()->GetCurrentLobby();
	for (const auto& member : currentLobby.members)
	{
		if (member.user_id == m_userID)
		{
			strDisplayName = member.display_name;
			break;
		}
	}

	if (pOwningMesh->m_cbOnConnected != nullptr)
	{
		pOwningMesh->m_cbOnConnected(m_userID, strDisplayName, this);
	}
}

void PlayerConnection::SetDisconnected(bool bWasError, NetworkMesh* pOwningMesh)
{
	if (bWasError)
	{
		m_State = EConnectionState::CONNECTION_FAILED;
	}
	else
	{
		m_State = EConnectionState::CONNECTION_DISCONNECTED;
	}
	UpdateState(m_State, pOwningMesh);

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
