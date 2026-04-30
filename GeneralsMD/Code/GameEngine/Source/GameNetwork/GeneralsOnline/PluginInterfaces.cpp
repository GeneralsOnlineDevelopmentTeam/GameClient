#include "GameNetwork/GeneralsOnline/PluginInterfaces.h"
#include "../NGMP_include.h"
#include "../NetworkMesh.h"
#include "../OnlineServices_Init.h"
#include "../OnlineServices_Auth.h"

#include <format>

namespace
{
    std::string FormatWin32ErrorMessage(DWORD errorCode)
    {
        LPSTR messageBuffer = nullptr;

        const DWORD size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPSTR>(&messageBuffer),
            0,
            nullptr);

        std::string message;

        if (size > 0 && messageBuffer != nullptr)
        {
            message = messageBuffer;
            LocalFree(messageBuffer);
        }
        else
        {
            message = "Unknown Windows error";
        }

        while (!message.empty() &&
            (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
        {
            message.pop_back();
        }

        return std::format("{} ({})", message, errorCode);
    }
}

#define AC_PLUGIN_LOAD_FUNCTION(funcName) \
    AnticheatPlugInterface::Functions.fn##funcName = \
        (FuncDef##funcName)GetProcAddress(g_hACPluginModule, #funcName); \
    if (!AnticheatPlugInterface::Functions.fn##funcName) \
    { \
        return Fail(std::format("Plugin loaded, but required export '{}' was not found.", #funcName)); \
    }

bool AnticheatPlugInterface::IsExternalProcessRunning()
{
    if (IsPluginLoaded())
    {
        return Functions.fnIsExternalProcessRunning();
    }

    return false;
}

int AnticheatPlugInterface::GetAnticheatIdentifier()
{
    if (IsPluginLoaded())
    {
        return Functions.fnGetAnticheatIdentifier();
    }

    return 0;
}

bool AnticheatPlugInterface::LoadPlugin(const char* szPluginPath, std::string* outFailureReason)
{
    auto Fail = [&](const std::string& reason) -> bool
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Plugin load failed: %s", reason.c_str());

        if (outFailureReason != nullptr)
        {
            *outFailureReason = reason;
        }

        if (g_hACPluginModule != nullptr)
        {
            FreeLibrary(g_hACPluginModule);
            g_hACPluginModule = nullptr;
        }

        Functions = AnticheatPluginFunctionPtrs{};
        m_bPluginLoadFailed = true;

        return false;
    };

    if (g_hACPluginModule != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Existing plugin module is loaded; unloading before reload.");
        UnloadPlugin();
    }

    m_bPluginLoadFailed = false;
    m_lastLoadError = ERROR_SUCCESS;
    m_lastLoadPath.clear();
    Functions = AnticheatPluginFunctionPtrs{};

    if (szPluginPath == nullptr || szPluginPath[0] == '\0')
    {
        return Fail("Resolved AntiCheat plugin path was empty.");
    }

    m_lastLoadPath = szPluginPath;

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Attempting to load plugin from absolute path: %s", szPluginPath);

    g_hACPluginModule = LoadLibraryExA(
        szPluginPath,
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    if (!g_hACPluginModule)
    {
        m_lastLoadError = GetLastError();

        return Fail(
            std::format(
                "Windows failed to load '{}'. GetLastError={}",
                szPluginPath,
                FormatWin32ErrorMessage(m_lastLoadError)));
    }

    AC_PLUGIN_LOAD_FUNCTION(SetLoggingFunction);

    Functions.fnSetLoggingFunction([](const char* szMsg)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "%s", szMsg);
        });

    AC_PLUGIN_LOAD_FUNCTION(Initialize);

    const int result = Functions.fnInitialize();
    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Initialize result = %d", result);

    AC_PLUGIN_LOAD_FUNCTION(IsExternalProcessRunning);
    AC_PLUGIN_LOAD_FUNCTION(GetAnticheatIdentifier);

#if _DEBUG
    SetWindowText(ApplicationHWnd, Functions.fnIsExternalProcessRunning() ? "SECURED" : "INSECURE");
#endif

    AC_PLUGIN_LOAD_FUNCTION(SetACIntegrityViolationOccurredCallback);

    Functions.fnSetACIntegrityViolationOccurredCallback([](const char* szReason, int violationType)
        {
            NetworkLog(
                ELogVerbosity::LOG_RELEASE,
                "[AC] Leaving lobby, local AC integrity violation occurred (%d): %s.",
                violationType,
                szReason);

            g_bPendingExitLobby = true;
        });

    AC_PLUGIN_LOAD_FUNCTION(SetACActionRequiredCallback);

    Functions.fnSetACActionRequiredCallback([](
        uint32_t userID,
        const char* szReason,
        EAnticheatActionType actionType,
        EAnticheatActionReason actionReason)
        {
            NGMP_OnlineServices_AuthInterface* pAuthInterface =
                NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();

            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Action required.");

            if (pAuthInterface == nullptr)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, no auth interface.");
                g_bPendingExitLobby = true;
                return;
            }

            if (pAuthInterface->GetUserID() == userID)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, action requested against local user.");
                g_bPendingExitLobby = true;
                return;
            }

            NetworkLog(
                ELogVerbosity::LOG_RELEASE,
                "[AC] Disconnecting remote user %u, action requested by AntiCheat.",
                userID);

            NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
            if (pMesh != nullptr)
            {
                pMesh->DisconnectUser(userID);
            }
            else
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Leaving lobby, no mesh exists for remote action.");
                g_bPendingExitLobby = true;
            }
        });

    AC_PLUGIN_LOAD_FUNCTION(SetSendMessageViaTransportCallback);

    Functions.fnSetSendMessageViaTransportCallback([](uint32_t goUserID, const void* pData, uint32_t dataLen)
        {
            NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
            if (pMesh != nullptr)
            {
                pMesh->SendACPacket(goUserID, pData, dataLen);
            }
        });

    AC_PLUGIN_LOAD_FUNCTION(ACMessageArrivedViaTransport);
    AC_PLUGIN_LOAD_FUNCTION(Login);
    AC_PLUGIN_LOAD_FUNCTION(RefreshToken);
    AC_PLUGIN_LOAD_FUNCTION(IsLoggedIn);
    AC_PLUGIN_LOAD_FUNCTION(GetMiddlewareAuthToken);
    AC_PLUGIN_LOAD_FUNCTION(BeginSession);
    AC_PLUGIN_LOAD_FUNCTION(EndSession);
    AC_PLUGIN_LOAD_FUNCTION(RegisterPlayer);
    AC_PLUGIN_LOAD_FUNCTION(DeregisterPlayer);
    AC_PLUGIN_LOAD_FUNCTION(Tick);
    AC_PLUGIN_LOAD_FUNCTION(Shutdown);

    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Plugin loaded successfully: %s", szPluginPath);

    return true;
}

bool AnticheatPlugInterface::g_bPendingExitLobby = false;

void AnticheatPlugInterface::AC_NetworkMessageArrived(uint32_t goUserID, void* pData, uint32_t dataLen)
{
    // TODO: Cache all of these getprocaddresses
    if (IsPluginLoaded() && Functions.fnACMessageArrivedViaTransport != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] fnOnMessageArrivedViaTransport");
        Functions.fnACMessageArrivedViaTransport(goUserID, pData, dataLen);
    }
}


void AnticheatPlugInterface::Authenticate()
{
    if (IsPluginLoaded() && Functions.fnLogin != nullptr && Functions.fnIsLoggedIn)
    {
        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
        if (pAuthInterface == nullptr)
        {
            return;
        }

        Functions.fnLogin(pAuthInterface->GetAuthToken().c_str(),
            [](bool bSuccess)
            {
                if (!bSuccess)
                {
                    // TODO_AC: Handle this, its a fatal error
                    return;
                }

                m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

                if (Functions.fnIsLoggedIn())
                {
                    char buf[4196];
                    if (Functions.fnGetMiddlewareAuthToken(buf, sizeof(buf)))
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Got MW token: %s", buf);

                        // Now we can begin login
                        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
                        if (pAuthInterface == nullptr)
                        {
                            return;
                        }

                        pAuthInterface->SendMiddlewareToken(std::string(buf));
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
}

void AnticheatPlugInterface::EndSession()
{
    if (IsPluginLoaded() && Functions.fnEndSession != nullptr)
    {
        Functions.fnEndSession();
        g_bSessionStarted = false;
    }
}

AnticheatPlugInterface::AnticheatPluginFunctionPtrs AnticheatPlugInterface::Functions;

HMODULE AnticheatPlugInterface::g_hACPluginModule = nullptr;
bool AnticheatPlugInterface::m_bPluginLoadFailed = false;
DWORD AnticheatPlugInterface::m_lastLoadError = ERROR_SUCCESS;
std::string AnticheatPlugInterface::m_lastLoadPath;

int64_t AnticheatPlugInterface::m_tokenCreationTime = -1;

bool AnticheatPlugInterface::RegisterPlayer(std::string mwUserID, uint32_t goUserID)
{
    if (!g_bSessionStarted) // TODO_AC: This is hacky, it's because on lobby join, the server can send AC_REGISTER_PLAYER before we join the lobby, so we didnt actually start the session yet. We should buffer these messages until session start or something instead of relying on this hacky global
    {
        AnticheatPlugInterface::BeginSession();
    }

    if (IsPluginLoaded() && Functions.fnRegisterPlayer != nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "RegisterPlayer: %s to %" PRIu64, mwUserID.c_str(), goUserID);

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
        NetworkLog(ELogVerbosity::LOG_RELEASE, "DeregisterPlayer: %s to %" PRIu64, mwUserID.c_str(), goUserID);

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
        if (Functions.fnIsLoggedIn())
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
    if (IsPluginLoaded() && Functions.fnRefreshToken != nullptr && Functions.fnIsLoggedIn)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Refreshing token");
        NGMP_OnlineServices_AuthInterface* pAuthInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_AuthInterface>();
        if (pAuthInterface == nullptr)
        {
            return;
        }

        m_tokenCreationTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

        Functions.fnRefreshToken(pAuthInterface->GetAuthToken().c_str(),
            [](bool bSuccess)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Refreshed token: %d", bSuccess);
                if (!bSuccess)
                {
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
