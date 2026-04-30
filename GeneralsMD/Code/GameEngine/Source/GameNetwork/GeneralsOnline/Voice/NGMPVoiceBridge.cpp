// NGMPVoiceBridge.cpp
//
// See NGMPVoiceBridge.h for the role of this file.

#include "PreRTS.h"

#include "GameNetwork/GeneralsOnline/Voice/NGMPVoiceBridge.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoiceManager.h"

#include "GameNetwork/NetworkDefs.h"                        // TransportMessageHeader, VOICE_MAGIC_NUMBER
#include "GameNetwork/GeneralsOnline/NetworkMesh.h"         // NetworkMesh, PlayerConnection
#include "GameNetwork/GeneralsOnline/ngmp_include.h"
#include "GameNetwork/GeneralsOnline/ngmp_interfaces.h"
#include "GameNetwork/GeneralsOnline/NGMPGame.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Auth.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_LobbyInterface.h" // extern TheNGMPGame
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"

#include "GameLogic/GameLogic.h"                            // TheGameLogic
#include "GameClient/InGameUI.h"                            // TheInGameUI->message for in-game notifications

#include <windows.h>                                        // GetAsyncKeyState, VK_LMENU
#include <cstring>
#include <unordered_map>
#include <chrono>

// ---------------------------------------------------------------------
// DEV ONLY: Force-start voice without a real lobby.
//
// When VOICE_DEV_FORCE_START is defined, NGMPVoiceBridge::Init will
// immediately call VoiceManager::EnterLobby with a dummy user id. This
// bypasses the normal "only active inside a Generals Online lobby"
// lifecycle and lets you test voice chat from the main menu or a
// skirmish match. Combine with VOICE_DEV_LOCAL_LOOPBACK in VoiceManager
// to hear your own mic with no network path involved.
//
// Do not define this in shipping builds.
// ---------------------------------------------------------------------
// #define VOICE_DEV_FORCE_START 1

#ifdef VOICE_DEV_FORCE_START
static constexpr int64_t kForceStartDummyUserID = 0x10A9BACC10A9BACDll;
#endif

//---------------------------------------------------------------------
// Static state
//---------------------------------------------------------------------
namespace
{
    bool s_bridgeInitialised = false;

    // Last session state observed, used to detect lobby->in-game edge.
    enum class SessionState
    {
        Idle,
        Lobby,
        InGame
    };
    SessionState s_session = SessionState::Idle;

    int64_t      s_myUserID = 0;

    // Previous PTT key state so we only call SetPushToTalkPressed on edges.
    bool         s_pttPrev  = false;

    //-----------------------------------------------------------------
    // Discord-style speaker notifier state
    //
    // Tracks per-peer speaking state across frames so we can fire a
    // transition event (silent -> speaking) and show
    //   [MIC] PlayerName
    // in the lobby chat or in-game HUD. We also throttle re-notifications
    // so a peer that stops+restarts quickly doesn't spam the log; a
    // minimum quiet gap is required before we'll announce the same peer
    // again.
    //-----------------------------------------------------------------
    struct SpeakerState
    {
        bool  speaking      = false;
        int64_t lastSpokeMs = 0;   // last time speaking was observed
    };
    std::unordered_map<int64_t, SpeakerState> s_speakerStates;

    // Re-notify threshold: if a peer has been silent for this long and
    // starts talking again, we will re-announce.
    constexpr int64_t kRenotifyGapMs = 4000;

    int64_t NowMs()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count();
    }

    // Narrow std::string (display_name is ASCII/UTF-8) -> UnicodeString
    UnicodeString NarrowToUni(const std::string& narrow)
    {
        std::wstring w;
        w.reserve(narrow.size());
        for (char c : narrow)
            w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        return UnicodeString(w.c_str());
    }

    // Emit a "<name> started speaking" notification through whichever
    // UI sink is appropriate right now:
    //   - In-match   -> TheInGameUI->message (HUD toast)
    //   - In-lobby   -> lobby chat callback (yellow line in chat box)
    void EmitSpeakerNotification(const std::string& displayName, int64_t userID)
    {
        UnicodeString line;
        if (!displayName.empty())
        {
            line.format(L"[MIC] %ls", NarrowToUni(displayName).str());
        }
        else
        {
            line.format(L"[MIC] player %lld", (long long)userID);
        }

        // In-game HUD message (auto-fades, doesn't stack up).
        if (TheGameLogic != nullptr && TheGameLogic->isInGame() && TheInGameUI != nullptr)
        {
            TheInGameUI->message(line);
            return;
        }

        // Lobby chat box (reuses the already-registered chat callback).
        NGMP_OnlineServices_LobbyInterface* pLobby =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
        if (pLobby != nullptr && pLobby->m_OnChatCallback != nullptr)
        {
            const Color micColor = static_cast<Color>(0xFFFFC20F); // yellow
            pLobby->m_OnChatCallback(line, micColor);
        }
    }

    // Called every frame from NGMPVoiceBridge::Update when voice is active.
    // Walks current remote peers, compares their speaking state to what we
    // saw last frame, and fires a notification on silent->speaking edges.
    void PollSpeakerTransitions()
    {
        if (TheVoiceManager == nullptr) return;

        NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
        if (pMesh == nullptr)
        {
            // No mesh means we're not in a lobby/match -> reset state so
            // stale entries don't carry across sessions.
            s_speakerStates.clear();
            return;
        }

        NGMP_OnlineServices_LobbyInterface* pLobby =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

        const int64_t now = NowMs();

        for (auto& kv : pMesh->GetAllConnections())
        {
            const int64_t peerID = kv.first;
            if (peerID == s_myUserID) continue; // don't announce ourselves

            const bool nowSpeaking = TheVoiceManager->IsPeerSpeaking(peerID);
            SpeakerState& st = s_speakerStates[peerID];

            if (nowSpeaking)
            {
                // Fire only on transition, or re-fire if they had a long
                // enough silent gap since the last notification.
                const bool wasSilent = !st.speaking;
                const bool longGap   = (now - st.lastSpokeMs) > kRenotifyGapMs;
                if (wasSilent && longGap)
                {
                    std::string name;
                    if (pLobby != nullptr)
                    {
                        LobbyMemberEntry e = pLobby->GetRoomMemberFromID(peerID);
                        if (e.user_id != -1) name = e.display_name;
                    }
                    EmitSpeakerNotification(name, peerID);
                }
                st.speaking    = true;
                st.lastSpokeMs = now;
            }
            else
            {
                st.speaking = false;
                // keep lastSpokeMs so the gap logic above still works
            }
        }
    }

    //-----------------------------------------------------------------
    // PacketSink - called from the VoiceManager capture thread.
    //
    // Must have the C-style signature (no capture). We use the globals
    // above to find the currently active mesh and broadcast.
    //-----------------------------------------------------------------
    void VoicePacketSink(const uint8_t* voiceData, int voiceLen)
    {
        if (voiceData == nullptr || voiceLen <= 0) return;

        NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
        if (pMesh == nullptr) return;

        // Build wire buffer: [TransportMessageHeader][voice payload]
        // crc is unused for voice packets (set to 0), magic identifies them.
        const int totalLen =
            static_cast<int>(sizeof(TransportMessageHeader)) + voiceLen;

        // kMaxWirePacketSize (256 + 13) + 6 header = ~275 bytes, safe on stack.
        unsigned char wire[sizeof(TransportMessageHeader) +
                           Voice::VoiceManager::kMaxWirePacketSize];
        if (totalLen > static_cast<int>(sizeof(wire)))
        {
            return; // oversized packet, drop
        }

        TransportMessageHeader hdr;
        hdr.crc   = 0;
        hdr.magic = VOICE_MAGIC_NUMBER;
        std::memcpy(wire, &hdr, sizeof(TransportMessageHeader));
        std::memcpy(wire + sizeof(TransportMessageHeader),
                    voiceData,
                    static_cast<size_t>(voiceLen));

        // Broadcast to every connected peer in the mesh.
        std::map<int64_t, PlayerConnection>& conns = pMesh->GetAllConnections();
        for (auto& kv : conns)
        {
            pMesh->SendGamePacket(wire,
                                  static_cast<uint32_t>(totalLen),
                                  kv.first);
        }
    }

    //-----------------------------------------------------------------
    // Build team map from TheNGMPGame and push it into the VoiceManager.
    //-----------------------------------------------------------------
    void EnterInGameTeamModeFromCurrentGame()
    {
        if (TheVoiceManager == nullptr) return;
        if (TheNGMPGame == nullptr) return;

        std::unordered_map<int64_t, int> teamMap;

        for (Int i = 0; i < MAX_SLOTS; ++i)
        {
            GameSlot* pSlot = TheNGMPGame->getSlot(i);
            if (pSlot == nullptr) continue;
            if (!pSlot->isHuman()) continue;

            NGMPGameSlot* pNGMPSlot = static_cast<NGMPGameSlot*>(pSlot);
            if (pNGMPSlot->m_userID == -1) continue;

            teamMap[pNGMPSlot->m_userID] = pSlot->getTeamNumber();
        }

        TheVoiceManager->EnterInGameTeamMode(s_myUserID, teamMap);
    }
} // anonymous namespace

//=====================================================================
// Public API
//=====================================================================

void NGMPVoiceBridge::Init()
{
    if (s_bridgeInitialised) return;

    if (TheVoiceManager == nullptr)
    {
        TheVoiceManager = new Voice::VoiceManager();
    }
    TheVoiceManager->Init();
    TheVoiceManager->SetPacketSink(&VoicePacketSink);

    // Apply persisted voice settings (mic device id, gain, master volume).
    // These survive Leave/EnterLobby cycles because VoiceManager caches
    // them internally and re-applies them on EnterLobby.
    {
        GenOnlineSettings& s = NGMP_OnlineServicesManager::Settings;
        TheVoiceManager->SetCaptureDevice(s.Voice_GetCaptureDeviceID());
        TheVoiceManager->SetMicGain(s.Voice_GetMicGain());
        TheVoiceManager->SetGlobalVoiceVolume(s.Voice_GetGlobalVolume());
    }

    s_session  = SessionState::Idle;
    s_myUserID = 0;
    s_pttPrev  = false;
    s_bridgeInitialised = true;

#ifdef VOICE_DEV_FORCE_START
    // Dev-only: jump straight into LOBBY_ALL with a dummy user id so
    // PTT works from the main menu. No real lobby is required.
    s_myUserID = kForceStartDummyUserID;
    s_session  = SessionState::Lobby;
    TheVoiceManager->EnterLobby(kForceStartDummyUserID);
#endif
}

void NGMPVoiceBridge::Shutdown()
{
    if (!s_bridgeInitialised) return;

    if (TheVoiceManager != nullptr)
    {
        TheVoiceManager->SetPacketSink(nullptr);
        TheVoiceManager->Shutdown();
        delete TheVoiceManager;
        TheVoiceManager = nullptr;
    }

    s_session  = SessionState::Idle;
    s_myUserID = 0;
    s_pttPrev  = false;
    s_bridgeInitialised = false;
}

void NGMPVoiceBridge::OnEnteredLobby(int64_t myUserID)
{
    if (!s_bridgeInitialised || TheVoiceManager == nullptr) return;

    // NGMP: Honour the host-set voice_enabled flag on the current lobby.
    // If the host disabled voice for this lobby we do NOT start the
    // VoiceManager session at all: PTT does nothing, no mic is opened,
    // and no inbound audio is decoded. This is the hard-enforcement path.
    {
        NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
        if (pLobbyInterface != nullptr)
        {
            const LobbyEntry& lobby = pLobbyInterface->GetCurrentLobby();
            if (!lobby.voice_enabled)
            {
                s_myUserID = myUserID;
                s_session  = SessionState::Idle; // voice is OFF for this lobby
                return;
            }
        }
    }

    s_myUserID = myUserID;
    s_session  = SessionState::Lobby;
    TheVoiceManager->EnterLobby(myUserID);
}

void NGMPVoiceBridge::OnLeftLobby()
{
    if (!s_bridgeInitialised || TheVoiceManager == nullptr) return;

    s_session  = SessionState::Idle;
    s_myUserID = 0;
    TheVoiceManager->Leave();
}

void NGMPVoiceBridge::Update()
{
    if (!s_bridgeInitialised || TheVoiceManager == nullptr) return;

    // ---- PTT poll (Left Alt held) ------------------------------------
    // GetAsyncKeyState is window-focus independent and does not interact
    // with the game's key-consumption pipeline, so PTT won't be eaten
    // by a text field etc.
    const bool pttNow =
        (s_session != SessionState::Idle) &&
        ((::GetAsyncKeyState(VK_LMENU) & 0x8000) != 0);

    if (pttNow != s_pttPrev)
    {
        TheVoiceManager->SetPushToTalkPressed(pttNow);
        s_pttPrev = pttNow;
    }

    // ---- Lobby -> in-game edge detection -----------------------------
    // The same NGMP mesh is reused for in-game, so we just need to flip
    // VoiceManager into team-only mode when the match actually starts.
    if (s_session == SessionState::Lobby)
    {
        if (TheNGMPGame != nullptr &&
            TheGameLogic != nullptr &&
            TheGameLogic->isInGame())
        {
            EnterInGameTeamModeFromCurrentGame();
            s_session = SessionState::InGame;
        }
    }
    else if (s_session == SessionState::InGame)
    {
        // If the game ended and we're back in a lobby, drop back to
        // LOBBY_ALL so everyone in the post-game lobby can talk again.
        if (TheGameLogic == nullptr || !TheGameLogic->isInGame())
        {
            if (TheNGMPGame != nullptr)
            {
                TheVoiceManager->EnterLobby(s_myUserID);
                s_session = SessionState::Lobby;
            }
            else
            {
                TheVoiceManager->Leave();
                s_session = SessionState::Idle;
            }
        }
    }

    TheVoiceManager->Update();

    // Discord-style "who's talking" indicator: check peers each frame and
    // surface a [MIC] name notification whenever a peer starts speaking.
    // Pure GenOnline logic - uses TheInGameUI->message and the existing
    // lobby chat callback, no vanilla UI file touched.
    if (s_session != SessionState::Idle)
    {
        PollSpeakerTransitions();
    }
}

void NGMPVoiceBridge::DispatchIncoming(int64_t senderUserID,
                                       const unsigned char* payload,
                                       int payloadLen)
{
    if (!s_bridgeInitialised || TheVoiceManager == nullptr) return;
    if (payload == nullptr || payloadLen <= 0)       return;

    TheVoiceManager->OnVoicePacket(senderUserID, payload, payloadLen);
}

bool NGMPVoiceBridge::IsActive()
{
    return s_bridgeInitialised && s_session != SessionState::Idle;
}

#else // !ENABLE_VOICE_CHAT

// Stubs so call sites compile without Opus/WASAPI.
void NGMPVoiceBridge::Init()      {}
void NGMPVoiceBridge::Shutdown()  {}
void NGMPVoiceBridge::Update()    {}
void NGMPVoiceBridge::OnEnteredLobby(int64_t) {}
void NGMPVoiceBridge::OnLeftLobby() {}
void NGMPVoiceBridge::DispatchIncoming(int64_t, const unsigned char*, int) {}
bool NGMPVoiceBridge::IsActive()  { return false; }

#endif // ENABLE_VOICE_CHAT
