// VoicePlayback.h
//
// WASAPI render-side voice playback. Owns one jitter buffer per sender
// (keyed by int64 user ID) and mixes their decoded audio into a single
// output stream on a dedicated worker thread.
//
// The design trades a little latency (target 60 ms of jitter buffer)
// for robustness against the kind of brief packet-order inversions
// and drops you get on real home internet connections.
//
// Per-peer state:
//   - An Opus decoder
//   - A ring of decoded 20 ms frames indexed by sequence number
//   - A "currently playing" cursor
//   - A mute flag
//   - A last-activity timestamp for the speaking indicator

#pragma once

#ifdef ENABLE_VOICE_CHAT

#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <deque>

namespace Voice
{

struct PeerPlaybackState; // fwd

class VoicePlayback
{
public:
    VoicePlayback();
    ~VoicePlayback();

    VoicePlayback(const VoicePlayback&) = delete;
    VoicePlayback& operator=(const VoicePlayback&) = delete;

    // Opens the default render endpoint and starts the worker thread.
    bool Start();
    void Stop();

    // Feed an incoming encoded Opus frame from a remote peer. This is
    // safe to call from any thread (network receive thread, main
    // thread, etc.). If the peer has never been seen before, its
    // decoder and jitter buffer are created on the fly.
    void SubmitFrame(int64_t senderUserID, uint16_t sequenceNumber,
                     const uint8_t* encoded, int encodedLen);

    // Drop a peer entirely: decoder released, buffer cleared. Call
    // when a player leaves the lobby/match.
    void RemovePeer(int64_t senderUserID);

    // Mute / unmute a specific peer. Muted peers still have their
    // decoder fed (to keep Opus state coherent if they're unmuted
    // later) but contribute silence to the mix.
    void SetPeerMuted(int64_t senderUserID, bool muted);
    bool IsPeerMuted(int64_t senderUserID);

    // Per-peer linear volume. 1.0 = unity, 0.0 = silent, 2.0 = +6 dB.
    // Clamped to [0.0, 2.0]. Applied on top of the global voice volume
    // in the mixer. Default is 1.0.
    void  SetPeerVolume(int64_t senderUserID, float volume);
    float GetPeerVolume(int64_t senderUserID);

    // Global voice volume, applied to the whole mix. 1.0 = unity.
    // Clamped to [0.0, 2.0]. This is the master slider for "how loud
    // are other players" in the Options UI. Default is 1.0.
    void  SetGlobalVolume(float volume);
    float GetGlobalVolume() const { return m_globalVolume.load(); }

    // Query whether a peer has sent voice recently (<400ms ago).
    // Used by the "speaking" LEDs in the UI.
    bool IsPeerSpeaking(int64_t senderUserID);

    // Drop every peer and clear all state. Used when leaving a
    // lobby / match.
    void ClearAllPeers();

private:
    void WorkerLoop();
    void InitialisePeerLocked(int64_t senderUserID);

    struct Impl;
    Impl* m_impl;

    std::atomic<bool>    m_workerRunning;
    std::atomic<bool>    m_workerShouldExit;
    std::atomic<float>   m_globalVolume{1.0f};
    std::thread          m_workerThread;

    std::mutex                                         m_peersMutex;
    std::unordered_map<int64_t, PeerPlaybackState*>    m_peers;
};

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
