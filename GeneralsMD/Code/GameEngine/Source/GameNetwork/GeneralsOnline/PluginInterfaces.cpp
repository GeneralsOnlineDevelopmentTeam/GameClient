#include "GameNetwork/GeneralsOnline/PluginInterfaces.h"
#include "../NGMP_include.h"
#include "../NetworkMesh.h"
#include "../OnlineServices_Init.h"
#include "../OnlineServices_Auth.h"
#include <atomic>

#define AC_PLUGIN_LOAD_FUNCTION(funcName) \
    AnticheatPlugInterface::Functions.fn##funcName = (FuncDef##funcName)GetProcAddress(g_hACPluginModule, #funcName); \
    if (!AnticheatPlugInterface::Functions.fn##funcName) \
    { \
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Failed to find " #funcName " function"); \
        FreeLibrary(g_hACPluginModule); \
        g_hACPluginModule = nullptr; \
        return; \
    }

// Tracks whether an anti-cheat session is currently active. The Set* callbacks
// registered below are invoked asynchronously by EOS; once EndSession() clears
// this flag (which LeaveCurrentLobby() does before deleting the lobby mesh),
// they must not dereference lobby/mesh objects that are about to be freed.
static std::atomic<bool> g_bACSessionActive{ false };

bool AnticheatPlugInterface::IsExternalProcessRunning()
{
    if (IsPluginLoaded() && Functions.fnIsExternalProcessRunning != nullptr)
    {
        return Functions.fnIsExternalProcessRunning();
    }

    return false;
}

int AnticheatPlugInterface::GetAnticheatIdentifier()
{
    if (IsPluginLoaded() && Functions.fnGetAnticheatIdentifier != nullptr)
    {
        return Functions.fnGetAnticheatIdentifier();
    }

    return 0;
}

void AnticheatPlugInterface::LoadPlugin(const char* szPluginName)
{
    if (szPluginName == nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Plugin name is null");
        m_bPluginLoadFailed = true;
        return;
    }

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Attempting to load plugin from %s", szPluginName);

    m_bPluginLoadFailed = false;
    g_hACPluginModule = LoadLibraryA(szPluginName);

    if (!g_hACPluginModule)
    {
        g_hACPluginModule = nullptr;
        m_bPluginLoadFailed = true;

        DWORD err = GetLastError();
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Failed to load %s (%u)", szPluginName, err);
    }
    else
    {
        // set logger 
        AC_PLUGIN_LOAD_FUNCTION(SetLoggingFunction);


        // Initialize AC
        AC_PLUGIN_LOAD_FUNCTION(Initialize);

        int result = Functions.fnInitialize();
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Initialize result = %d", result);

        // check loaded
        AC_PLUGIN_LOAD_FUNCTION(IsExternalProcessRunning);

        AC_PLUGIN_LOAD_FUNCTION(GetAnticheatIdentifier);

#if _DEBUG
        if (ApplicationHWnd != nullptr)
        {
            SetWindowText(ApplicationHWnd, Functions.fnIsExternalProcessRunning() ? "SECURED" : "INSECURE");
        }
#endif

        // integrity callback
        AC_PLUGIN_LOAD_FUNCTION(SetACIntegrityViolationOccurredCallback);


        // set action required callback
        AC_PLUGIN_LOAD_FUNCTION(SetACActionRequiredCallback);


        // set transport callback
        AC_PLUGIN_LOAD_FUNCTION(SetSendMessageViaTransportCallback);

        // AC network message arrived callback
        AC_PLUGIN_LOAD_FUNCTION(ACMessageArrivedViaTransport);

        // Login funcs
        AC_PLUGIN_LOAD_FUNCTION(Login);
        AC_PLUGIN_LOAD_FUNCTION(RefreshToken);
        AC_PLUGIN_LOAD_FUNCTION(IsLoggedIn);
        AC_PLUGIN_LOAD_FUNCTION(GetMiddlewareAuthToken);

        // Begin and end session funcs
        AC_PLUGIN_LOAD_FUNCTION(BeginSession);
        AC_PLUGIN_LOAD_FUNCTION(EndSession);

        // register player funcs
        AC_PLUGIN_LOAD_FUNCTION(RegisterPlayer);
        AC_PLUGIN_LOAD_FUNCTION(DeregisterPlayer);

        AC_PLUGIN_LOAD_FUNCTION(Tick);
        AC_PLUGIN_LOAD_FUNCTION(Shutdown);

        // Optional: callback de-registration (added in AntiCheatPlugin PR #2).
        // Resolved WITHOUT the fail-hard macro so older plugin DLLs still load.
        Functions.fnClearAllHostCallbacks =
            (FuncDefClearAllHostCallbacks)GetProcAddress(g_hACPluginModule, "ClearAllHostCallbacks");

        // Install the host callbacks for the first time. EndSession() clears them
        // before the lobby mesh is freed; BeginSession() re-installs them.
        RegisterHostCallbacks();
    }
}

void AnticheatPlugInterface::RegisterHostCallbacks()
{
    // (Re)install every host callback the plugin may invoke. The action-required
    // and transport callbacks dereference the per-lobby NetworkMesh, so EndSession()
    // clears them all (ClearAllHostCallbacks) before that mesh is freed; they must
    // therefore be re-registered whenever a session starts.
    if (!IsPluginLoaded())
    {
        return;
    }

    Functions.fnSetLoggingFunction([](const char* szMsg)
        {
            //MessageBoxA(nullptr, szMsg, szMsg, MB_OK);
            NetworkLog(ELogVerbosity::LOG_RELEASE, szMsg);
        });

    Functions.fnSetACIntegrityViolationOccurredCallback([](const char* szReason, int violationType)
        {
            if (szReason == nullptr)
            {
                szReason = "(null reason)";
            }

            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, local AC integrity violation occured (%d): %s.", violationType, szReason);
            g_bPendingExitLobby = true;
        });

    Functions.fnSetACActionRequiredCallback([](uint32_t userID, const char* szReason, EAnticheatActionType actionType, EAnticheatActionReason actionReason)
        {
            // Session torn down - the mesh this may act on can be freed. Bail.
            if (!g_bACSessionActive)
            {
                return;
            }

            if (szReason == nullptr)
            {
                szReason = "(null reason)";
            }

            NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Action required: %s", szReason);

            if (pAuthInterface == nullptr)
            {
                // no auth interface? bail out
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, lobby isn't secure, no auth interface.");
                g_bPendingExitLobby = true;
                return;
            }

            // If it's us, leave, if its someone else, d/c them
            uint32_t localUserID = pAuthInterface->GetUserID();
            if (localUserID == userID)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, lobby isn't secure, action was requested against local user.");
                g_bPendingExitLobby = true;
            }
            else
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Disconnecting remote user, lobby isn't secure, action was requested against remote user %u.", userID);

                NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
                if (pMesh != nullptr)
                {
                    pMesh->DisconnectUser(userID);
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Disconnected: %u.", userID);
                }
                else // no mesh, just back out
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, lobby isn't secure, actionable player was remote, but no mesh exists to take action.");
                    g_bPendingExitLobby = true;
                }
            }
        });

    Functions.fnSetSendMessageViaTransportCallback([](uint32_t goUserID, const void* pData, uint32_t dataLen)
        {
            // Session torn down (e.g. leaving a lobby) - the mesh may be freed. Bail.
            if (!g_bACSessionActive)
            {
                return;
            }

            if (pData == nullptr || dataLen == 0)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: SendMessageViaTransport received null/empty data");
                return;
            }

            // prefer websocket if we have it, otherwise fall back to p2p mesh
            bool bFallbackToP2P = false;
            std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
            if (pWS != nullptr)
            {
                if (pWS->IsConnected())
                {
                    if (dataLen > 0)
                    {
                        std::vector<uint8_t> vecPayload((uint8_t*)pData, (uint8_t*)pData + dataLen);
                        pWS->SendData_ACMessage(goUserID, vecPayload);
                    }
                    else
                    {
                        bFallbackToP2P = true;
                    }
                }
                else
                {
                    bFallbackToP2P = true;
                }
            }
            else
            {
                bFallbackToP2P = true;
            }

            if (bFallbackToP2P)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AC Packets - WebSocket unavailable, falling back to P2P");
                NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
                if (pMesh != nullptr)
                {
                    pMesh->SendACPacket(goUserID, pData, dataLen);
                }
                else
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Cannot send AC packet - NetworkMesh is null");
                }
            }
        });
}

bool AnticheatPlugInterface::g_bPendingExitLobby = false;

void AnticheatPlugInterface::AC_NetworkMessageArrived(uint32_t goUserID, void* pData, uint32_t dataLen)
{
    if (pData == nullptr || dataLen == 0)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: AC_NetworkMessageArrived received null/empty data");
        return;
    }

    // TODO: Cache all of these getprocaddresses
    if (IsPluginLoaded() && Functions.fnACMessageArrivedViaTransport != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] fnOnMessageArrivedViaTransport");
        Functions.fnACMessageArrivedViaTransport(goUserID, pData, dataLen);
    }
}


void AnticheatPlugInterface::Authenticate()
{
    if (IsPluginLoaded() && Functions.fnLogin != nullptr && Functions.fnIsLoggedIn != nullptr)
    {
        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
        if (pAuthInterface == nullptr)
        {
            return;
        }

        std::string authToken = pAuthInterface->GetAuthToken();
        Functions.fnLogin(authToken.c_str(),
            [](bool bSuccess)
            {
                if (!bSuccess)
                {
                    // TODO_AC: Handle this, its a fatal error
                    return;
                }

                m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

                if (Functions.fnIsLoggedIn != nullptr && Functions.fnIsLoggedIn())
                {
                    char buf[4196];
                    if (Functions.fnGetMiddlewareAuthToken != nullptr && Functions.fnGetMiddlewareAuthToken(buf, sizeof(buf)))
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Got MW token: %s", buf);

                        // Now we can begin login
                        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
                        if (pAuthInterface != nullptr)
                        {
                            pAuthInterface->SendMiddlewareToken(std::string(buf));
                        }
                        else
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Auth interface became null during login callback");
                        }
                    }
                }
                else
                {
                    // TODO_AC: Handle this, its a fatal error
                }

                
            });
    }
}

bool g_bSessionStarted = false;

void AnticheatPlugInterface::BeginSession()
{
    if (g_bSessionStarted)
    {
        // Already active - a duplicate BeginSession() is rejected by the plugin
        // (EOS_AlreadyConfigured) and is part of the begin/end churn.
        return;
    }

    // Re-install the host callbacks for this session: EndSession() de-registered
    // them so the plugin could not call into a freed mesh, so they must be set
    // again before the session (and any callbacks) start.
    RegisterHostCallbacks();

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] BeginSession() called");
    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] IsPluginLoaded=%d, fnBeginSession=%p", IsPluginLoaded(), Functions.fnBeginSession);
    
    if (IsPluginLoaded() && Functions.fnBeginSession != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Calling plugin fnBeginSession()");
        Functions.fnBeginSession();
        g_bSessionStarted = true;
        g_bACSessionActive = true;
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Plugin fnBeginSession() completed");
    }
    else
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Cannot call fnBeginSession - plugin not loaded or function pointer is null");
    }
}

void AnticheatPlugInterface::EndSession()
{
    if (!g_bSessionStarted)
    {
        // No active session - calling EndSession() now is rejected by the
        // plugin (EOS_NotConfigured) and is part of the begin/end churn.
        return;
    }

    // Stop our async callbacks from touching the mesh BEFORE it is torn down.
    g_bACSessionActive = false;

    if (IsPluginLoaded() && Functions.fnEndSession != nullptr)
    {
        Functions.fnEndSession();
    }

    // De-register the host callbacks under the plugin state lock. This blocks
    // until any in-flight worker-thread callback returns and nulls the stored
    // pointers, so the lobby mesh (freed right after EndSession() in
    // LeaveCurrentLobby) can no longer be dereferenced by a late callback.
    // No-ops against older plugin DLLs that do not export it yet.
    if (Functions.fnClearAllHostCallbacks != nullptr)
    {
        Functions.fnClearAllHostCallbacks();
    }

    g_bSessionStarted = false;
}

AnticheatPlugInterface::AnticheatPluginFunctionPtrs AnticheatPlugInterface::Functions;

HMODULE AnticheatPlugInterface::g_hACPluginModule = nullptr;
bool AnticheatPlugInterface::m_bPluginLoadFailed = false;

int64_t AnticheatPlugInterface::m_tokenCreationTime = -1;

bool AnticheatPlugInterface::RegisterPlayer(std::string mwUserID, uint32_t goUserID)
{
    if (!g_bSessionStarted) // TODO_AC: This is hacky, it's because on lobby join, the server can send AC_REGISTER_PLAYER before we join the lobby, so we didnt actually start the session yet. We should buffer these messages until session start or something instead of relying on this hacky global
    {
        AnticheatPlugInterface::BeginSession();
    }

    if (IsPluginLoaded() && Functions.fnRegisterPlayer != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "RegisterPlayer: %s to %" PRIu32, mwUserID.c_str(), goUserID);

        bool bReg = Functions.fnRegisterPlayer(mwUserID.c_str(), goUserID);
        NetworkLog(ELogVerbosity::LOG_RELEASE, "RegisterPlayerFunc result: %d", bReg);
        return bReg;
    }

    return false;
}


bool AnticheatPlugInterface::DeregisterPlayer(std::string mwUserID, uint32_t goUserID)
{
    if (IsPluginLoaded() && Functions.fnDeregisterPlayer != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "DeregisterPlayer: %s to %" PRIu32, mwUserID.c_str(), goUserID);

        bool bReg = Functions.fnDeregisterPlayer(mwUserID.c_str(), goUserID);
        NetworkLog(ELogVerbosity::LOG_RELEASE, "DeregisterPlayerFunc result: %d", bReg);
        return bReg;
    }

    return false;
}

void AnticheatPlugInterface::Tick()
{
    if (IsPluginLoaded() && Functions.fnTick != nullptr)
    {
        Functions.fnTick();

        // Do we need to refresh our token?
        if (Functions.fnIsLoggedIn != nullptr && Functions.fnIsLoggedIn())
        {
            int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
            if (m_tokenCreationTime != -1 && now - m_tokenCreationTime >= 45 * 60 * 1000) // refresh every 45m, tokens last 60m, giving us a 15m buffer to refresh and retry if something goes wrong
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Token is about to expire, refreshing...");
                RefreshToken();
            }
        }
    }
}

void AnticheatPlugInterface::RefreshToken()
{
    if (IsPluginLoaded() && Functions.fnRefreshToken != nullptr && Functions.fnIsLoggedIn != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Refreshing token");
        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
        if (pAuthInterface == nullptr)
        {
            return;
        }

        m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

        std::string authToken = pAuthInterface->GetAuthToken();
        Functions.fnRefreshToken(authToken.c_str(),
            [](bool bSuccess)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Refreshed token: %d", bSuccess);
                if (!bSuccess)
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Token refresh failed");
                    // TODO_AC: Handle this, its a fatal error
                    return;
                }
            });
    }
}

void AnticheatPlugInterface::UnloadPlugin()
{
    if (IsPluginLoaded())
    {
        // Stop async callbacks and de-register host callbacks before teardown.
        g_bACSessionActive = false;
        if (Functions.fnClearAllHostCallbacks != nullptr)
        {
            Functions.fnClearAllHostCallbacks();
        }

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Starting Shutdown");
        if (Functions.fnShutdown != nullptr)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Shutdown in progress");
            Functions.fnShutdown();
        }
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Shutdown Complete");

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Unloading plugin");
        FreeLibrary(g_hACPluginModule);
        g_hACPluginModule = nullptr;
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Unloaded plugin");
    }
}
