#pragma once

#include <string>

enum class EAnticheatActionType : int32_t
{
    NONE = 0,
    KICK = 1
};

enum class EAnticheatActionReason : int32_t
{
    Unknown = 0,
    InternalError = 1,
    InvalidMessage = 2,
    AuthFailure = 3,
    ACNotRunning = 4,
    HeartbeatTimedOut = 5,
    ClientViolation = 6,
    BackendViolation = 7,
    TempCooldown = 8,
    TempBanned = 9,
    PermaBanned = 10
};


class AnticheatPlugInterface
{
public:
    static bool g_bPendingExitLobby;

    static void AC_NetworkMessageArrived(uint32_t goUserID, void* pData, uint32_t dataLen);

    static bool DidPluginFailToLoad() { return m_bPluginLoadFailed; }

    static bool IsPluginLoaded()
    {
        return g_hACPluginModule != nullptr && !m_bPluginLoadFailed;
    }

    static bool IsExternalProcessRunning();

    static int GetAnticheatIdentifier();

    static bool LoadPlugin(const char* szPluginPath, std::string* outFailureReason = nullptr);
    static void Authenticate();
    static void UnloadPlugin();
    static void Tick();

    static void RefreshToken();

    static bool RegisterPlayer(std::string mwUserID, uint32_t goUserID);
    static bool DeregisterPlayer(std::string mwUserID, uint32_t goUserID);

    static void BeginSession();
    static void EndSession();

    // Callbacks from plugin
    typedef void (*LoginCallback)(bool bSuccess);
    typedef void (*LoggingFunc)(const char*);
    typedef void (*FuncDefACPlayerActionRequiredCallbackFunc)(uint32_t, const char*, EAnticheatActionType, EAnticheatActionReason);
    typedef void (*FuncDefSetACActionRequiredCallback)(FuncDefACPlayerActionRequiredCallbackFunc);
    typedef void (*SendMessageViaTransportCallbackFunc)(uint32_t, const void*, uint32_t);

    typedef void (*FuncDefCIntegrityViolationOccurredCallbackFunc)(const char*, int);
    typedef void (*FuncDefSetACIntegrityViolationOccurredCallback)(FuncDefCIntegrityViolationOccurredCallbackFunc);

    // Func defs
    typedef void (*FuncDefSetLoggingFunction)(LoggingFunc);
    typedef int (*FuncDefInitialize)(void);
    typedef bool (*FuncDefIsExternalProcessRunning)(void);

    typedef int (*FuncDefGetAnticheatIdentifier)(void);
    
    typedef void (*FuncDefSetSendMessageViaTransportCallback)(SendMessageViaTransportCallbackFunc);
    typedef void (*FuncDefACMessageArrivedViaTransport)(uint32_t, void*, uint32_t);
    typedef void (*FuncDefLogin)(const char* szGameToken, LoginCallback cb);
    typedef void (*FuncDefRefreshToken)(const char* szGameToken, LoginCallback cb);
    typedef bool (*FuncDefGetMiddlewareAuthToken)(char* buffer, size_t bufferSize);
    typedef bool (*FuncDefIsLoggedIn)(void);
    typedef void (*FuncDefBeginSession)(void);
    typedef void (*FuncDefEndSession)(void);
    typedef bool (*FuncDefRegisterPlayer)(const char* szMiddlewareUserID, uint32_t goUserID);
    typedef bool (*FuncDefDeregisterPlayer)(const char* szMiddlewareUserID, uint32_t goUserID);
    typedef void (*FuncDefTick)(void);

    typedef void (*FuncDefShutdown)(void);

    struct AnticheatPluginFunctionPtrs
    {
        FuncDefSetLoggingFunction fnSetLoggingFunction = nullptr;
        FuncDefInitialize fnInitialize = nullptr;
        FuncDefIsExternalProcessRunning fnIsExternalProcessRunning = nullptr;
        FuncDefGetAnticheatIdentifier fnGetAnticheatIdentifier = nullptr;
        FuncDefSetACActionRequiredCallback fnSetACActionRequiredCallback = nullptr;
        FuncDefSetACIntegrityViolationOccurredCallback fnSetACIntegrityViolationOccurredCallback = nullptr;
        FuncDefSetSendMessageViaTransportCallback fnSetSendMessageViaTransportCallback = nullptr;
        FuncDefACMessageArrivedViaTransport fnACMessageArrivedViaTransport = nullptr;
        FuncDefLogin fnLogin = nullptr;
        FuncDefRefreshToken fnRefreshToken = nullptr;
        FuncDefGetMiddlewareAuthToken fnGetMiddlewareAuthToken = nullptr;
        FuncDefIsLoggedIn fnIsLoggedIn = nullptr;
        FuncDefBeginSession fnBeginSession = nullptr;
        FuncDefEndSession fnEndSession = nullptr;
        FuncDefRegisterPlayer fnRegisterPlayer = nullptr;
        FuncDefDeregisterPlayer fnDeregisterPlayer = nullptr;
        FuncDefTick fnTick = nullptr;
        FuncDefShutdown fnShutdown = nullptr;
    };
    static AnticheatPluginFunctionPtrs Functions;

    // Module
    static HMODULE g_hACPluginModule;
    static bool m_bPluginLoadFailed;
    static DWORD m_lastLoadError;
    static std::string m_lastLoadPath;

    static int64_t m_tokenCreationTime;
};

extern HWND ApplicationHWnd;
