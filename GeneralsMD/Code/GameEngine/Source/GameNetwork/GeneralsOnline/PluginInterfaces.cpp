#include "GameNetwork/GeneralsOnline/PluginInterfaces.h"
#include "../NGMP_include.h"
#include "../NetworkMesh.h"
#include "../OnlineServices_Init.h"
#include "../OnlineServices_Auth.h"
#include "../OnlineServices_LobbyInterface.h"

std::atomic<bool> AnticheatPlugInterface::g_bPendingExitLobby = false;

#if defined(GENERALS_ONLINE_USE_PLUGINS_INTERFACE)

#define AC_PLUGIN_LOAD_FUNCTION(funcName) \
    AnticheatPlugInterface::Functions.fn##funcName = (FuncDef##funcName)GetProcAddress(g_hACPluginModule, #funcName); \
    if (!AnticheatPlugInterface::Functions.fn##funcName) \
    { \
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Failed to find " #funcName " function"); \
        FreeLibrary(g_hACPluginModule); \
        g_hACPluginModule = nullptr; \
        /* every pointer resolved so far now points into an unloaded module, drop them all */ \
        AnticheatPlugInterface::Functions = AnticheatPluginFunctionPtrs(); \
        m_bPluginLoadFailed = true; \
        return; \
    }

// Optional exports: a missing one must not invalidate the whole plugin
#define AC_PLUGIN_LOAD_FUNCTION_OPTIONAL(funcName) \
    AnticheatPlugInterface::Functions.fn##funcName = (FuncDef##funcName)GetProcAddress(g_hACPluginModule, #funcName); \
    if (!AnticheatPlugInterface::Functions.fn##funcName) \
    { \
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Optional function " #funcName " not exported by plugin"); \
    }

// Documented Initialize() contract exposed by the anticheat plugins.
// Anything non-zero means the plugin can never function, so the module is unloaded.
static const char* AC_DescribeInitializeResult(int result)
{
    switch (result)
    {
        case 0: return "success";
        case 1: return "bad environment (unparseable exe path, non-ASCII install path, or missing XAudio redist)";
        case 2: return "failed to create the integrated platform options container";
        case 3: return "middleware SDK initialization failed";
        case 4: return "middleware platform creation failed";
        case 5: return "plugin was built without its middleware deployment credentials";
        default: return "unknown failure";
    }
}

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

int AnticheatPlugInterface::GetConnectionLatencyForUser(std::string mwUserID, uint32_t goUserID)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnGetConnectionLatencyForUser != nullptr)
    {
        return Functions.fnGetConnectionLatencyForUser(mwUserID.c_str(), goUserID);
    }
#endif

    return 0;
}

bool AnticheatPlugInterface::IsConnectionRelayed(std::string mwUserID, uint32_t goUserID)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnIsConnectionRelayed != nullptr)
    {
        return Functions.fnIsConnectionRelayed(mwUserID.c_str(), goUserID);
    }
#endif

    return false;
}

void AnticheatPlugInterface::LoadPlugin(const char* szPluginName)
{
    if (g_hACPluginModule != nullptr || IsPluginLoaded())
    {
        return;
    }

    if (szPluginName == nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Plugin name is null");
        m_bPluginLoadFailed = true;
        return;
    }

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Attempting to load plugin from %s", szPluginName);

#if defined(_DEBUG)
    szPluginName = "F:\\Gen\\eac_module\\build\\Debug\\easyanticheat.dll";
#endif

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
#if defined(AC_ENABLED)
        // set logger 
        AC_PLUGIN_LOAD_FUNCTION(SetLoggingFunction);

        Functions.fnSetLoggingFunction([](const char* szMsg)
            {
                // never pass plugin supplied text as a format string
                NetworkLog(ELogVerbosity::LOG_RELEASE, "%s", szMsg != nullptr ? szMsg : "(null message)");
            });

        AC_PLUGIN_LOAD_FUNCTION(SetLobbyChatOutputFunction);

        Functions.fnSetLobbyChatOutputFunction([](const char* szMsg)
            {
                UnicodeString unicodeStr;
                unicodeStr.translate(AsciiString(szMsg));

                NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
                if (pLobbyInterface != nullptr)
                {
                    if (pLobbyInterface->m_OnChatCallback != nullptr)
                    {
                        pLobbyInterface->m_OnChatCallback(unicodeStr, DetermineSystemNoticeColor(false, false));
                    }
                }
            });

        // Initialize AC
        AC_PLUGIN_LOAD_FUNCTION(Initialize);

        int result = Functions.fnInitialize([](const char* szMiddlewareID, uint64_t goUserID, EConnectionState newState) // on connection state changed callback
            {
                // These states describe the plugin's own EOS P2P connections. When the
                // plugin does not own the transport the mesh is driven by GameNetworkingSockets,
                // and writing EOS states into it stamps "not connected" over healthy
                // connections - which the full mesh connectivity check then reports as
                // missing links. Also guards against an older plugin that predates
                // SetSecureGameTransportEnabled() and so stays in EOS mode.
                if (!AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
                {
                    return;
                }

                // this can arrive on a plugin owned thread, the mesh locks internally
                NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
                if (pMesh != nullptr)
                {
                    pMesh->UpdateConnectionStateForUser(static_cast<int64_t>(goUserID), newState);
                }
            });
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Initialize result = %d", result);

        if (result != 0)
        {
            // A failed Initialize() leaves the plugin with no middleware platform, so every
            // subsequent call would fail. Tear it back down rather than running half-loaded.
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] FATAL: Plugin Initialize returned %d - %s", result, AC_DescribeInitializeResult(result));

            // Initialize() is responsible for cleaning up after itself on failure, but call
            // Shutdown() anyway if it is exported - it is documented as idempotent.
            FuncDefShutdown fnShutdown = (FuncDefShutdown)GetProcAddress(g_hACPluginModule, "Shutdown");
            if (fnShutdown != nullptr)
            {
                fnShutdown();
            }

            // Clear the logging sink before unloading: the plugin keeps host callbacks
            // registered across Shutdown(), and they must not outlive this module handle.
            if (Functions.fnSetLoggingFunction != nullptr)
            {
                Functions.fnSetLoggingFunction(nullptr);
            }

            if (Functions.fnSetLobbyChatOutputFunction != nullptr)
            {
                Functions.fnSetLobbyChatOutputFunction(nullptr);
            }

            FreeLibrary(g_hACPluginModule);
            g_hACPluginModule = nullptr;
            Functions = AnticheatPluginFunctionPtrs();
            m_bPluginLoadFailed = true;
            return;
        }

        // check loaded
        AC_PLUGIN_LOAD_FUNCTION(IsExternalProcessRunning);

        AC_PLUGIN_LOAD_FUNCTION(GetAnticheatIdentifier);

#if _DEBUG
        if (ApplicationHWnd != nullptr)
        {
            SetWindowTextA(ApplicationHWnd, Functions.fnIsExternalProcessRunning() ? "SECURED" : "INSECURE");
        }
#endif

        // integrity callback
        AC_PLUGIN_LOAD_FUNCTION(SetACIntegrityViolationOccurredCallback);

        Functions.fnSetACIntegrityViolationOccurredCallback([](const char* szReason, int violationType)
            {
                if (szReason == nullptr)
                {
                    szReason = "(null reason)";
                }

                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, local AC integrity violation occured (%d): %s.", violationType, szReason);
                g_bPendingExitLobby = true;
            });

        // set action required callback
        AC_PLUGIN_LOAD_FUNCTION(SetACActionRequiredCallback);

        Functions.fnSetACActionRequiredCallback([](uint32_t userID, const char* szReason, EAnticheatActionType actionType, EAnticheatActionReason actionReason)
            {
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

        // set transport callback
        AC_PLUGIN_LOAD_FUNCTION(SetSendMessageViaTransportCallback);
        Functions.fnSetSendMessageViaTransportCallback([](uint32_t goUserID, const void* pData, uint32_t dataLen)
            {
                if (pData == nullptr || dataLen == 0)
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: SendMessageViaTransport received null/empty data");
                    return;
                }

                // if the plugin owns a secure transport, AC traffic is carried internally by the plugin
                // (see PlayerConnection::SendACPacket) so there is nothing for the game to route here
                if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
                {
                    return;
                }

                // prefer websocket if we have it, otherwise fall back to p2p mesh
                bool bFallbackToP2P = false;

                {
                    std::shared_ptr<WebSocket>  pWS = NGMP_OnlineServicesManager::GetWebSocket();
                    if (pWS != nullptr)
                    {
                        if (pWS->IsConnected())
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

        // AC network message arrived callback
        AC_PLUGIN_LOAD_FUNCTION(ACMessageArrivedViaTransport);

        // transport funcs
        AC_PLUGIN_LOAD_FUNCTION(DoesACPluginProvideSecureGameTransport);
        AC_PLUGIN_LOAD_FUNCTION_OPTIONAL(SetSecureGameTransportEnabled);
        AC_PLUGIN_LOAD_FUNCTION(StartSignalling);
        AC_PLUGIN_LOAD_FUNCTION(SendPacket);
        AC_PLUGIN_LOAD_FUNCTION(GetNextRecvPacketSize);
        AC_PLUGIN_LOAD_FUNCTION(RecvPacket);
        AC_PLUGIN_LOAD_FUNCTION_OPTIONAL(FreePacket);
        AC_PLUGIN_LOAD_FUNCTION(GetConnectionLatencyForUser);
        AC_PLUGIN_LOAD_FUNCTION(IsConnectionRelayed);

        AC_PLUGIN_LOAD_FUNCTION(DisconnectPlayer);
        AC_PLUGIN_LOAD_FUNCTION(DisconnectAll);

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
#else
    // Initialize AC
    AC_PLUGIN_LOAD_FUNCTION(Initialize);

    int result = Functions.fnInitialize();
    NetworkLog(ELogVerbosity::LOG_RELEASE, "Initialize result = %d", result);

       AC_PLUGIN_LOAD_FUNCTION(IsExternalProcessRunning);
        AC_PLUGIN_LOAD_FUNCTION(GetAnticheatIdentifier);
#endif
    }
}

void AnticheatPlugInterface::AC_NetworkMessageArrived(uint32_t goUserID, void* pData, uint32_t dataLen)
{
#if defined(AC_ENABLED)
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
#endif
}


void AnticheatPlugInterface::Authenticate()
{
#if defined(AC_ENABLED)
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
                // NOTE: the plugin guarantees this fires exactly once, including on
                // failure, so these branches are genuinely reachable.
                if (!bSuccess)
                {
                    // TODO_AC: Handle this, its a fatal error
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Middleware login failed - session will not be secure");
                    return;
                }

                m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

                if (Functions.fnIsLoggedIn != nullptr && Functions.fnIsLoggedIn())
                {
                    char buf[4096] = { 0 };
                    if (Functions.fnGetMiddlewareAuthToken != nullptr && Functions.fnGetMiddlewareAuthToken(buf, sizeof(buf)))
                    {
                        // don't trust the plugin to terminate the buffer
                        buf[sizeof(buf) - 1] = '\0';

#if _DEBUG
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Got MW token: %s", buf);
#else
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Got MW token (%zu chars)", strlen(buf));
#endif

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
                    else
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Failed to retrieve middleware auth token after a successful login");
                    }
                }
                else
                {
                    // TODO_AC: Handle this, its a fatal error
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Login reported success but the plugin is not logged in");
                }
            });
    }
#endif
}

bool g_bSessionStarted = false;

void AnticheatPlugInterface::BeginSession()
{
#if defined(AC_ENABLED)
    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] BeginSession() called");
    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] IsPluginLoaded=%d, fnBeginSession=%p", IsPluginLoaded(), Functions.fnBeginSession);
    
    if (IsPluginLoaded() && Functions.fnBeginSession != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Calling plugin fnBeginSession()");
        Functions.fnBeginSession();
        g_bSessionStarted = true;
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Plugin fnBeginSession() completed");
    }
    else
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Cannot call fnBeginSession - plugin not loaded or function pointer is null");
    }
#endif
}

void AnticheatPlugInterface::EndSession()
{
    // a stale exit request would eject us from the next lobby
    g_bPendingExitLobby = false;

#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnEndSession != nullptr)
    {
        Functions.fnEndSession();
        g_bSessionStarted = false;
    }
#endif
}

bool AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport()
{
#if defined(AC_ENABLED)
    ServiceConfig& serviceConf = NGMP_OnlineServicesManager::GetInstance()->GetServiceConfig();
    const bool bEnabledByConfig = serviceConf.connection.anticheat_transport;

    // The plugin makes this same decision internally in places the game cannot
    // reach - notably inside the EOS anti-cheat OnMessageToPeer callback, which
    // picks between EOS P2P and handing the message back to us - so it needs its
    // own copy of the flag. Push it whenever it changes: the service config is
    // re-fetched per lobby, so this can flip between lobbies.
    static bool s_bPushedValue = false;
    static bool s_bEverPushed = false;
    if ((!s_bEverPushed || s_bPushedValue != bEnabledByConfig)
        && IsPluginLoaded() && Functions.fnSetSecureGameTransportEnabled != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Secure game transport: telling plugin use_new_networking=%s",
            bEnabledByConfig ? "true" : "false");
        Functions.fnSetSecureGameTransportEnabled(bEnabledByConfig);
        s_bPushedValue = bEnabledByConfig;
        s_bEverPushed = true;
    }

    if (!bEnabledByConfig)
    {
        return false;
    }

    if (IsPluginLoaded() && Functions.fnDoesACPluginProvideSecureGameTransport != nullptr)
    {
        return Functions.fnDoesACPluginProvideSecureGameTransport();
    }
#endif

    return false;
}

void AnticheatPlugInterface::SendPacket(const char* szMiddlewareUserID, uint64_t targetGoUserID, void* pData, int numBytes, ENetworkChannels channel, EPacketReliability reliability)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnSendPacket != nullptr)
    {
        Functions.fnSendPacket(szMiddlewareUserID, targetGoUserID, pData, numBytes, channel, reliability);
    }
#endif
}

void AnticheatPlugInterface::StartSignalling(const char* szMiddlewareUserID, uint64_t goUserID)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnStartSignalling != nullptr)
    {
        Functions.fnStartSignalling(szMiddlewareUserID, goUserID);
    }
#endif
}

int AnticheatPlugInterface::GetNextRecvPacketSize(uint8_t channelToReceiveOn)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnGetNextRecvPacketSize != nullptr)
    {
        return Functions.fnGetNextRecvPacketSize(channelToReceiveOn);
    }
#endif

    return 0;
}

bool AnticheatPlugInterface::RecvPacket(uint8_t** pOutData, uint8_t channelToReceiveOn)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnRecvPacket != nullptr)
    {
        return Functions.fnRecvPacket(pOutData, channelToReceiveOn);
    }
#endif

    return false;
}

void AnticheatPlugInterface::FreePacket(void* pPacketData)
{
#if defined(AC_ENABLED)
    if (pPacketData == nullptr)
    {
        return;
    }

    // the buffer was allocated inside the plugin, it must be released there too
    if (IsPluginLoaded() && Functions.fnFreePacket != nullptr)
    {
        Functions.fnFreePacket(pPacketData);
    }
#endif
}

void AnticheatPlugInterface::DisconnectPlayer(const char* szMiddlewareUserID, uint64_t goUserID)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnDisconnectPlayer != nullptr)
    {
        Functions.fnDisconnectPlayer(szMiddlewareUserID, goUserID);
    }
#endif
}

void AnticheatPlugInterface::DisconnectAll()
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnDisconnectAll != nullptr)
    {
        Functions.fnDisconnectAll();
    }
#endif
}

AnticheatPlugInterface::AnticheatPluginFunctionPtrs AnticheatPlugInterface::Functions;

HMODULE AnticheatPlugInterface::g_hACPluginModule = nullptr;
bool AnticheatPlugInterface::m_bPluginLoadFailed = false;

int64_t AnticheatPlugInterface::m_tokenCreationTime = -1;

bool AnticheatPlugInterface::RegisterPlayer(std::string mwUserID, uint32_t goUserID)
{
#if defined(AC_ENABLED)
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
#else
    return true;
#endif
}


bool AnticheatPlugInterface::DeregisterPlayer(std::string mwUserID, uint32_t goUserID)
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded() && Functions.fnDeregisterPlayer != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "DeregisterPlayer: %s to %" PRIu32, mwUserID.c_str(), goUserID);

        bool bReg = Functions.fnDeregisterPlayer(mwUserID.c_str(), goUserID);
        NetworkLog(ELogVerbosity::LOG_RELEASE, "DeregisterPlayerFunc result: %d", bReg);
        return bReg;
    }

    return false;
#else
    return true;
#endif
}

void AnticheatPlugInterface::Tick()
{
#if defined(AC_ENABLED)
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
#endif
}

void AnticheatPlugInterface::RefreshToken()
{
#if defined(AC_ENABLED)
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
#endif
}

void AnticheatPlugInterface::UnloadPlugin()
{
#if defined(AC_ENABLED)
    if (IsPluginLoaded())
    {
        // Close any live anticheat session first so the plugin can unregister its
        // middleware notifications while the platform is still valid.
        if (g_bSessionStarted)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Ending active session before shutdown");
            EndSession();
        }

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Starting Shutdown");
        if (Functions.fnShutdown != nullptr)
        {
            // REQUIRED: the plugin deliberately does not tear down its middleware from
            // DllMain (that would run under the loader lock and deadlock), so this
            // explicit call is the only thing that releases it.
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Shutdown in progress");
            Functions.fnShutdown();
        }
        else
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] ERROR: Plugin has no Shutdown export - middleware will not be released");
        }
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Shutdown Complete");

        // Shutdown() intentionally preserves the host callbacks we registered, so clear
        // them explicitly - they point into this module and must not survive the unload.
        if (Functions.fnSetACIntegrityViolationOccurredCallback != nullptr)
        {
            Functions.fnSetACIntegrityViolationOccurredCallback(nullptr);
        }
        if (Functions.fnSetACActionRequiredCallback != nullptr)
        {
            Functions.fnSetACActionRequiredCallback(nullptr);
        }
        if (Functions.fnSetSendMessageViaTransportCallback != nullptr)
        {
            Functions.fnSetSendMessageViaTransportCallback(nullptr);
        }
        if (Functions.fnSetLobbyChatOutputFunction != nullptr)
        {
            Functions.fnSetLobbyChatOutputFunction(nullptr);
        }
        if (Functions.fnSetLoggingFunction != nullptr)
        {
            // last, so the calls above can still be logged
            Functions.fnSetLoggingFunction(nullptr);
        }

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Unloading plugin");
        FreeLibrary(g_hACPluginModule);
        g_hACPluginModule = nullptr;

        // all function pointers reference the unloaded module now
        Functions = AnticheatPluginFunctionPtrs();
        m_bPluginLoadFailed = false;
        g_bSessionStarted = false;
        m_tokenCreationTime = -1;
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Unloaded plugin");
    }
#endif
}

#endif
