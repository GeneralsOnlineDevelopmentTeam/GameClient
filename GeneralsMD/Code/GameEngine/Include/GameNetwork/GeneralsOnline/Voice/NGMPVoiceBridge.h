// NGMPVoiceBridge.h
//
// Glue between the VoiceManager (platform-agnostic audio + codec) and
// the NGMP peer mesh. Lives here so Voice/ stays free of NGMP/Steam
// includes and NGMP/ stays free of WASAPI/Opus includes.
//
// The bridge owns:
//   - the global TheVoiceManager lifetime (create in Init, destroy in Shutdown)
//   - the PacketSink that serialises a voice frame onto the wire with the
//     VOICE_MAGIC_NUMBER prefix and broadcasts it to every peer in the mesh
//   - the reverse path: given a SteamNetworkingMessage payload whose
//     TransportMessageHeader magic == VOICE_MAGIC_NUMBER, strip the header
//     and hand the remaining bytes to TheVoiceManager->OnVoicePacket
//   - lobby -> in-game state transitions driven by the lobby lifecycle
//   - the per-frame PTT poll (Left Alt via GetAsyncKeyState)
//
// Called from:
//   - GameEngine::init          -> NGMPVoice_Init
//   - GameEngine::~GameEngine   -> NGMPVoice_Shutdown
//   - GameEngine::update        -> NGMPVoice_Update
//   - OnlineServices_LobbyInterface (Join/Create success) -> NGMPVoice_OnEnteredLobby
//   - OnlineServices_LobbyInterface::LeaveCurrentLobby    -> NGMPVoice_OnLeftLobby
//   - NextGenTransport::doRecv (on magic == VOICE_MAGIC_NUMBER) ->
//         NGMPVoice_DispatchIncoming

#pragma once

#include <cstdint>

class NGMPVoiceBridge
{
public:
    // Called once, very early in GameEngine::init. Safe to call without
    // ENABLE_VOICE_CHAT (becomes a no-op).
    static void Init();

    // Called once, in GameEngine::~GameEngine. No-op if Init wasn't called.
    static void Shutdown();

    // Called every frame from GameEngine::update. Polls the PTT key and
    // performs the lobby -> in-game mode transition.
    static void Update();

    // Called by the NGMP lobby interface after it successfully joins or
    // creates a lobby (i.e. after m_pLobbyMesh has been constructed). The
    // bridge then asks the VoiceManager to open the mic/speakers and
    // installs the broadcast sink.
    static void OnEnteredLobby(int64_t myUserID);

    // Called when leaving the current lobby, before the mesh is deleted.
    // Closes audio devices and clears the sink pointer.
    static void OnLeftLobby();

    // Dispatch a raw incoming network message that has already been
    // identified as a voice packet by its header magic. `payload` must
    // point to the bytes that follow the 6-byte TransportMessageHeader,
    // and `payloadLen` must be the number of such bytes.
    static void DispatchIncoming(int64_t senderUserID,
                                 const unsigned char* payload,
                                 int payloadLen);

    // True if the bridge currently has an active voice session (lobby
    // or in-game). Used to gate UI widgets.
    static bool IsActive();
};
