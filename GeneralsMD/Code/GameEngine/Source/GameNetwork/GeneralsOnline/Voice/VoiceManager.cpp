// VoiceManager.cpp

#include "PreRTS.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoiceManager.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceCapture.h"
#include "GameNetwork/GeneralsOnline/Voice/VoicePlayback.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceOpusCodec.h"
#include "GameNetwork/GeneralsOnline/ngmp_include.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Init.h"  // NGMP_OnlineServicesManager::Settings
#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"

#include <cstring>
#include <vector>

// ---------------------------------------------------------------------
// DEV ONLY: Local loopback mode.
//
// When VOICE_DEV_LOCAL_LOOPBACK is defined, every frame captured by the
// microphone is additionally fed straight into the playback jitter buffer
// under a fake peer ID. This lets you verify the full capture -> encode
// -> decode -> playback chain without any real peer in the mesh. You
// should hear yourself about 100 ms after speaking.
//
// Intended for local development only. Leave undefined in shipping
// builds: otherwise every player would hear themselves echo back.
// ---------------------------------------------------------------------
// #define VOICE_DEV_LOCAL_LOOPBACK 1

#ifdef VOICE_DEV_LOCAL_LOOPBACK
// Fake peer id under which the loopback audio is routed. Kept far from
// any real NGMP user id so it is easy to spot in logs and cannot collide
// with a real peer.
static constexpr int64_t kLoopbackFakePeerID = 0x10A9BACC10A9BACCll;
#endif

Voice::VoiceManager* TheVoiceManager = nullptr;

namespace Voice
{

// --------------------------------------------------------------------
VoiceManager::VoiceManager() = default;

VoiceManager::~VoiceManager()
{
    Shutdown();
}

// --------------------------------------------------------------------
void VoiceManager::Init()
{
    if (m_initialised.load()) return;

    m_capture  = std::make_unique<VoiceCapture>();
    m_playback = std::make_unique<VoicePlayback>();
    m_encoder  = std::make_unique<OpusEncoderWrapper>();
    m_encoder->Initialise(20000); // 20 kbps speech bitrate

    // Pull the persistent per-client ignore list out of GenOnlineSettings
    // and seed m_mutedPeers with it. This is a purely local operation:
    // nothing is sent over the network, no other clients are informed.
    // When EnterLobby runs later it will re-apply every entry in this set
    // to the active VoicePlayback, so the mutes take effect before the
    // first packet from that peer arrives.
    {
        const std::vector<int64_t>& persisted =
            NGMP_OnlineServicesManager::Settings.Voice_GetMutedPeers();
        for (int64_t id : persisted)
        {
            if (id > 0) m_mutedPeers.insert(id);
        }
    }

    m_initialised.store(true);
}

// --------------------------------------------------------------------
void VoiceManager::Shutdown()
{
    if (!m_initialised.load()) return;

    Leave();

    if (m_encoder)  { m_encoder->Shutdown(); }
    m_capture.reset();
    m_playback.reset();
    m_encoder.reset();

    m_initialised.store(false);
}

// --------------------------------------------------------------------
void VoiceManager::EnterLobby(int64_t myUserID)
{
    if (!m_initialised.load()) Init();

    m_myUserID.store(myUserID);
    m_outSeq.store(0);
    m_mode.store(VoiceMode::LOBBY_ALL);

    if (m_playback)
    {
        m_playback->ClearAllPeers();
        m_playback->Start();
        // Re-apply cached settings so they survive Leave/EnterLobby.
        m_playback->SetGlobalVolume(m_globalVoiceVolume);
        for (const auto& kv : m_peerVolumes)
        {
            m_playback->SetPeerVolume(kv.first, kv.second);
        }
        for (int64_t mutedID : m_mutedPeers)
        {
            m_playback->SetPeerMuted(mutedID, true);
        }
    }
    if (m_capture)
    {
        m_capture->SetDeviceID(m_captureDeviceID);
        m_capture->SetMicGain(m_micGain);
        m_capture->Start([this](const int16_t* pcm, int n) {
            this->OnCaptureFrame(pcm, n);
        });
    }
}

// --------------------------------------------------------------------
void VoiceManager::EnterInGameTeamMode(int64_t myUserID,
    const std::unordered_map<int64_t, int>& teamAssignments)
{
    if (!m_initialised.load()) Init();

    m_myUserID.store(myUserID);
    m_mode.store(VoiceMode::IN_GAME_TEAM);

    {
        std::lock_guard<std::mutex> lock(m_teamsMutex);
        m_teams = teamAssignments;
        auto it = m_teams.find(myUserID);
        m_myTeam = (it != m_teams.end()) ? it->second : -1;
    }

    // Audio devices already running (seamless transition).
    if (!m_playback || !m_playback->IsPeerSpeaking(0)) // just use IsPeerSpeaking as "is object alive"
    {
        // If Leave was called between lobby and game, restart.
        if (m_playback) m_playback->Start();
        if (m_capture)
        {
            m_capture->Start([this](const int16_t* pcm, int n) {
                this->OnCaptureFrame(pcm, n);
            });
        }
    }
}

// --------------------------------------------------------------------
void VoiceManager::Leave()
{
    m_mode.store(VoiceMode::DISABLED);
    m_pttPressed.store(false);

    if (m_capture)
    {
        m_capture->SetTransmitting(false);
        m_capture->Stop();
    }
    if (m_playback)
    {
        m_playback->Stop();
        m_playback->ClearAllPeers();
    }
    {
        std::lock_guard<std::mutex> lock(m_teamsMutex);
        m_teams.clear();
        m_myTeam = -1;
    }
}

// --------------------------------------------------------------------
void VoiceManager::Update()
{
    // Nothing time-critical here - UI polls IsPeerSpeaking() etc
    // directly. Kept as a hook in case we want to do watchdogging
    // later (e.g. auto-restart capture if the mic device was lost).
}

// --------------------------------------------------------------------
void VoiceManager::SetPushToTalkPressed(bool pressed)
{
    if (m_mode.load() == VoiceMode::DISABLED) return;

    const bool wasPressed = m_pttPressed.exchange(pressed);
    if (pressed != wasPressed && m_capture)
    {
        m_capture->SetTransmitting(pressed);
    }
}

// --------------------------------------------------------------------
// Capture callback (runs on capture worker thread)
// --------------------------------------------------------------------
void VoiceManager::OnCaptureFrame(const int16_t* pcm, int sampleCount)
{
    if (sampleCount != kSamplesPerFrame) return;
    if (m_mode.load() == VoiceMode::DISABLED) return;
    if (!m_pttPressed.load()) return;

#ifndef VOICE_DEV_LOCAL_LOOPBACK
    // In real (non-loopback) builds we need a packet sink to send out.
    // In loopback builds we don't, because we never touch the network.
    if (m_packetSink == nullptr) return;
#endif

    uint8_t encoded[kMaxEncodedBytes];
    int encodedLen = 0;
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        if (!m_encoder || !m_encoder->IsReady()) return;
        encodedLen = m_encoder->EncodeFrame(pcm, encoded, sizeof(encoded));
    }
    if (encodedLen <= 0) return;

    // Build wire packet.
    uint8_t wire[kMaxWirePacketSize];
    wire[0] = kVoicePacketTag;
    const int64_t myID = m_myUserID.load();
    std::memcpy(&wire[1], &myID, sizeof(int64_t));
    const uint16_t seq = m_outSeq.fetch_add(1);
    std::memcpy(&wire[9], &seq, sizeof(uint16_t));
    const uint16_t lenLE = static_cast<uint16_t>(encodedLen);
    std::memcpy(&wire[11], &lenLE, sizeof(uint16_t));
    std::memcpy(&wire[kVoiceHeaderBytes], encoded, encodedLen);
    const int totalLen = kVoiceHeaderBytes + encodedLen;

    if (m_packetSink != nullptr)
    {
        m_packetSink(wire, totalLen);
    }

#ifdef VOICE_DEV_LOCAL_LOOPBACK
    // Feed the encoded frame straight into playback under a fake peer
    // id so the self-drop check in OnVoicePacket doesn't kick in and
    // the loopback peer shows up as a distinct speaker. We bypass
    // OnVoicePacket entirely (which would also enforce the team filter
    // in IN_GAME_TEAM mode and reject our fake id).
    if (m_playback)
    {
        m_playback->SubmitFrame(kLoopbackFakePeerID, seq,
                                encoded, encodedLen);
    }
#endif
}

// --------------------------------------------------------------------
// Receive side (runs on network receive thread)
// --------------------------------------------------------------------
void VoiceManager::OnVoicePacket(int64_t senderUserID,
                                 const uint8_t* data, int length)
{
    if (data == nullptr || length < kVoiceHeaderBytes) return;
    if (data[0] != kVoicePacketTag) return;
    if (m_mode.load() == VoiceMode::DISABLED) return;
    if (senderUserID == m_myUserID.load()) return; // never play self

    uint16_t seq = 0, payloadLen = 0;
    std::memcpy(&seq,        &data[9],  sizeof(uint16_t));
    std::memcpy(&payloadLen, &data[11], sizeof(uint16_t));
    if (payloadLen == 0 ||
        length < static_cast<int>(kVoiceHeaderBytes + payloadLen)) return;

    // Team filter when in-game.
    if (m_mode.load() == VoiceMode::IN_GAME_TEAM)
    {
        std::lock_guard<std::mutex> lock(m_teamsMutex);
        auto it = m_teams.find(senderUserID);
        if (it == m_teams.end() || it->second != m_myTeam) return;
    }

    if (m_playback)
    {
        m_playback->SubmitFrame(senderUserID, seq,
            &data[kVoiceHeaderBytes], payloadLen);
    }
}

// --------------------------------------------------------------------
// UI queries
// --------------------------------------------------------------------
bool VoiceManager::IsPeerSpeaking(int64_t peerUserID)
{
    return m_playback ? m_playback->IsPeerSpeaking(peerUserID) : false;
}

bool VoiceManager::IsSelfSpeaking()
{
    return m_pttPressed.load() && m_capture && m_capture->GetCurrentLevel() > 0.01f;
}

void VoiceManager::SetPeerMuted(int64_t peerUserID, bool muted)
{
    // Refuse obviously invalid IDs - otherwise we'd write garbage into the
    // persistent list and load it back forever.
    if (peerUserID <= 0) return;

    const bool wasMuted = (m_mutedPeers.count(peerUserID) > 0);
    if (muted == wasMuted) return; // nothing changed, skip disk write

    if (muted) m_mutedPeers.insert(peerUserID);
    else       m_mutedPeers.erase(peerUserID);

    if (m_playback) m_playback->SetPeerMuted(peerUserID, muted);

    // Persist the full set so the mute survives a game restart. This is
    // strictly client-local: Save_Voice_MutedPeers only writes to the
    // local settings JSON, it never sends anything over the network.
    std::vector<int64_t> snapshot;
    snapshot.reserve(m_mutedPeers.size());
    for (int64_t id : m_mutedPeers) snapshot.push_back(id);
    NGMP_OnlineServicesManager::Settings.Save_Voice_MutedPeers(snapshot);
}

bool VoiceManager::IsPeerMuted(int64_t peerUserID)
{
    return m_mutedPeers.count(peerUserID) > 0;
}

float VoiceManager::GetMicLevel()
{
    return m_capture ? m_capture->GetCurrentLevel() : 0.0f;
}

// --------------------------------------------------------------------
// Audio settings (device picker, mic gain, volumes)
// --------------------------------------------------------------------
std::vector<CaptureDeviceInfo> VoiceManager::EnumerateCaptureDevices()
{
    return VoiceCapture::EnumerateDevices();
}

void VoiceManager::SetCaptureDevice(const std::wstring& deviceID)
{
    m_captureDeviceID = deviceID;
    if