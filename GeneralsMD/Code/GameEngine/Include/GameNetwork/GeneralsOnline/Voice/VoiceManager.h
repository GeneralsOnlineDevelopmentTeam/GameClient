// VoiceManager.h
//
// Orchestrator for the lobby voice chat feature. Owns the single
// capture, the single playback, the encoder, and the routing logic
// between them.
//
// Typical lifecycle from the outside:
//
//   // When the player enters a Network Multiplayer lobby:
//   TheVoiceManager->EnterLobby(myUserID);
//
//   // Per frame or per UI tick:
//   TheVoiceManager->Update();
//
//   // Match starts: switch to team-only mode and inform who is on
//   // which team.
//   TheVoiceManager->EnterInGameTeamMode(teamAssignments);
//
//   // Match or lobby ends:
//   TheVoiceManager->Leave();
//
// The manager installs its own PTT key hook via SetPushToTalkPressed
// calls driven from the window input handlers (the key is LEFT-ALT
// held - see VoiceManager::SetPushToTalkPressed).
//
// Incoming voice packets (20 ms Opus frames) arrive via OnVoicePacket
// called from the NGMP receive path.

#pragma once

#ifdef ENABLE_VOICE_CHAT

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>

#include "GameNetwork/GeneralsOnline/Voice/VoiceCapture.h" // CaptureDeviceInfo

namespace Voice
{

class VoiceCapture;
class VoicePlayback;
class OpusEncoderWrapper;

enum class VoiceMode
{
    DISABLED,         // Not in a lobby or match - voice subsystem idle
    LOBBY_ALL,        // Pre-game lobby, everyone hears everyone
    IN_GAME_TEAM      // In a running match, only same team
};

class VoiceManager
{
public:
    VoiceManager();
    ~VoiceManager();

    VoiceManager(const VoiceManager&) = delete;
    VoiceManager& operator=(const VoiceManager&) = delete;

    // Idempotent init - allocates capture/playback/encoder. Does NOT
    // open audio devices yet, that happens on EnterLobby.
    void Init();
    void Shutdown();

    // Entering a Network Multiplayer pre-game lobby. Opens mic and
    // speaker, starts worker threads, sets mode to LOBBY_ALL. The
    // user's own ID is needed so outgoing packets can be stamped.
    void EnterLobby(int64_t myUserID);

    // Match started - switch to team-only mode. teamAssignments maps
    // user ID -> team number. Peers whose team differs from the local
    // team are routed past the jitter buffer (muted implicitly).
    void EnterInGameTeamMode(int64_t myUserID,
        const std::unordered_map<int64_t, int>& teamAssignments);

    // Leaving lobby/match - close audio, clear peers, back to DISABLED.
    void Leave();

    // Call once per frame from the main UI update. Uses the poll to
    // drive the speaking-indicator timers and other lightweight
    // bookkeeping.
    void Update();

    // -------- Push-to-talk ---------------------------------------
    // Call from the input handler when the PTT key goes down / up.
    // The manager handles the state; capture is always live, only
    // transmission is gated.
    void SetPushToTalkPressed(bool pressed);
    bool IsPushToTalkPressed() const { return m_pttPressed.load(); }

    // -------- Outgoing packet hook -------------------------------
    // Set a function that will be called with each encoded voice
    // packet ready to go over the wire. The network layer sets this
    // to a function that calls NetworkMesh::SendGamePacket() on each
    // allowed recipient.
    using PacketSink = void(*)(const uint8_t* data, int length);
    void SetPacketSink(PacketSink sink) { m_packetSink = sink; }

    // -------- Incoming packet path -------------------------------
    // Called from the NGMP receive thread when a voice packet
    // arrives. Parses header, decrypts team filter, pushes into
    // VoicePlayback's jitter buffer.
    void OnVoicePacket(int64_t senderUserID,
                       const uint8_t* data, int length);

    // -------- UI queries -----------------------------------------
    bool IsPeerSpeaking(int64_t peerUserID);
    bool IsSelfSpeaking();
    void SetPeerMuted(int64_t peerUserID, bool muted);
    bool IsPeerMuted(int64_t peerUserID);
    float GetMicLevel();

    // -------- Audio settings (forwarded to capture / playback) ---
    // Enumerate capture endpoints for a Settings-screen device picker.
    static std::vector<CaptureDeviceInfo> EnumerateCaptureDevices();

    // Select capture device by WASAPI id (empty string = default comms).
    // Takes effect on the next EnterLobby / capture restart.
    void SetCaptureDevice(const std::wstring& deviceID);
    const std::wstring& GetCaptureDevice() const { return m_captureDeviceID; }

    // Linear mic input gain (0.0 .. 4.0, default 1.0).
    void  SetMicGain(float gain);
    float GetMicGain() const { return m_micGain; }

    // Master voice volume (0.0 .. 2.0, default 1.0). Applies to the
    // whole mix of everyone else's voices.
    void  SetGlobalVoiceVolume(float volume);
    float GetGlobalVoiceVolume() const { return m_globalVoiceVolume; }

    // Per-peer voice volume (0.0 .. 2.0, default 1.0). Lets a player
    // turn a loud team-mate down without muting them.
    void  SetPeerVolume(int64_t peerUserID, float volume);
    float GetPeerVolume(int64_t peerUserID);

    // -------- Packet layout (also used by network layer) --------
    // [0]    = packet tag byte (kVoicePacketTag)
    // [1..8] = sender user id (little-endian int64)
    // [9..10]= sequence number (little-endian uint16)
    // [11..12]=encoded payload length (little-endian uint16)
    // [13..] = opus payload
    static constexpr uint8_t  kVoicePacketTag    = 0xFA;
    static constexpr int      kVoiceHeaderBytes  = 13;
    static constexpr int      kMaxWirePacketSize = kVoiceHeaderBytes + 256;

    VoiceMode GetMode() const { return m_mode; }

private:
    // Called by the capture thread when a 20 ms PCM frame is ready.
    // Encodes, builds the wire packet, and fans out via m_packetSink.
    void OnCaptureFrame(const int16_t* pcm, int sampleCount);

    std::unique_ptr<VoiceCapture>        m_capture;
    std::unique_ptr<VoicePlayback>       m_playback;
    std::unique_ptr<OpusEncoderWrapper>  m_encoder;
    std::mutex                           m_encoderMutex;

    std::atomic<VoiceMode>               m_mode{VoiceMode::DISABLED};
    std::atomic<int64_t>                 m_myUserID{0};
    std::atomic<uint16_t>                m_outSeq{0};
    std::atomic<bool>                    m_pttPressed{false};
    std::atomic<bool>                    m_initialised{false};

    PacketSink                           m_packetSink = nullptr;

    // Team assignments for in-game filtering.
    std::mutex                                   m_teamsMutex;
    std::unordered_map<int64_t, int>             m_teams;
    int                                          m_myTeam = -1;

    // Mute list - persists across lobby/match boundaries for the
    // current session so muting once stays muted through the game.
    std::unordered_set<int64_t>                  m_mutedPeers;

    // Per-peer volume overrides that should persist across
    // lobby/match boundaries (so turning a peer down once sticks).
    std::unordered_map<int64_t, float>           m_peerVolumes;

    // Settings cached here so they survive Leave()/EnterLobby cycles
    // where the capture and playback objects get re-Start()ed.
    std::wstring                                 m_captureDeviceID;
    float                                        m_micGain           = 1.0f;
    float                                        m_globalVoiceVolume = 1.0f;
};

} // namespace Voice

// Global instance pointer. Created in NGMP init, destroyed on shutdown.
extern Voice::VoiceManager* TheVoiceManager;

#endif // ENABLE_VOICE_CHAT
