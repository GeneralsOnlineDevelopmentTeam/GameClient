// VoicePlayback.cpp

#include "PreRTS.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoicePlayback.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceOpusCodec.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>

namespace Voice
{

// --------------------------------------------------------------------
// Constants
// --------------------------------------------------------------------
static constexpr int    kTargetJitterFrames = 3;   // 3 * 20 ms = 60 ms
static constexpr int    kMaxJitterFrames    = 8;   // hard cap
static constexpr double kSpeakingTimeoutMs  = 400.0;

// --------------------------------------------------------------------
// PeerPlaybackState: per-sender decoder, jitter buffer, mute flag
// --------------------------------------------------------------------
struct PeerPlaybackState
{
    OpusDecoderWrapper                      decoder;

    // Jitter buffer: map sequence -> decoded frame. Using a map so we
    // handle out-of-order arrival naturally.
    struct Frame
    {
        uint16_t                        sequence = 0;
        std::vector<int16_t>            samples;  // kSamplesPerFrame int16
        bool                            valid = false;
    };
    std::deque<Frame>                       queue;
    uint16_t                                nextExpectedSeq = 0;
    bool                                    primed = false;

    std::atomic<bool>                       muted{false};
    std::atomic<float>                      volume{1.0f};

    // Last time a frame was submitted - used for "speaking" state.
    std::chrono::steady_clock::time_point   lastActivity;
};

// --------------------------------------------------------------------
// VoicePlayback::Impl: WASAPI objects
// --------------------------------------------------------------------
struct VoicePlayback::Impl
{
    IMMDeviceEnumerator* deviceEnum   = nullptr;
    IMMDevice*           device       = nullptr;
    IAudioClient*        audioClient  = nullptr;
    IAudioRenderClient*  renderClient = nullptr;
    WAVEFORMATEX*        mixFormat    = nullptr;
    HANDLE               bufferReady  = nullptr;
    bool                 comInitialised = false;

    int  endpointSampleRate    = 0;
    int  endpointChannels      = 0;
    int  endpointBitsPerSample = 0;
    bool endpointIsFloat       = false;

    UINT32 bufferFrameCount = 0;
};

template <typename T>
static void SafeRelease(T** pp)
{
    if (*pp != nullptr)
    {
        (*pp)->Release();
        *pp = nullptr;
    }
}

// --------------------------------------------------------------------
VoicePlayback::VoicePlayback()
    : m_impl(new Impl)
    , m_workerRunning(false)
    , m_workerShouldExit(false)
{
}

VoicePlayback::~VoicePlayback()
{
    Stop();
    ClearAllPeers();
    delete m_impl;
    m_impl = nullptr;
}

// --------------------------------------------------------------------
bool VoicePlayback::Start()
{
    if (m_workerRunning.load())
    {
        return true;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE)
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    m_impl->comInitialised = SUCCEEDED(hr);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&m_impl->deviceEnum));
    if (FAILED(hr)) { Stop(); return false; }

    hr = m_impl->deviceEnum->GetDefaultAudioEndpoint(eRender,
            eCommunications, &m_impl->device);
    if (FAILED(hr) || m_impl->device == nullptr)
    {
        hr = m_impl->deviceEnum->GetDefaultAudioEndpoint(eRender,
                eConsole, &m_impl->device);
        if (FAILED(hr) || m_impl->device == nullptr)
        {
            Stop();
            return false;
        }
    }

    hr = m_impl->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                  nullptr,
                                  reinterpret_cast<void**>(&m_impl->audioClient));
    if (FAILED(hr)) { Stop(); return false; }

    hr = m_impl->audioClient->GetMixFormat(&m_impl->mixFormat);
    if (FAILED(hr) || m_impl->mixFormat == nullptr) { Stop(); return false; }

    m_impl->endpointSampleRate    = m_impl->mixFormat->nSamplesPerSec;
    m_impl->endpointChannels      = m_impl->mixFormat->nChannels;
    m_impl->endpointBitsPerSample = m_impl->mixFormat->wBitsPerSample;
    m_impl->endpointIsFloat       = false;
    if (m_impl->mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
    {
        m_impl->endpointIsFloat = true;
    }
    else if (m_impl->mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_impl->mixFormat);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
        {
            m_impl->endpointIsFloat = true;
        }
    }

    const REFERENCE_TIME kBufferDuration = 80 * 10000; // 80 ms
    hr = m_impl->audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration, 0,
            m_impl->mixFormat, nullptr);
    if (FAILED(hr)) { Stop(); return false; }

    m_impl->bufferReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_impl->bufferReady == nullptr) { Stop(); return false; }
    hr = m_impl->audioClient->SetEventHandle(m_impl->bufferReady);
    if (FAILED(hr)) { Stop(); return false; }

    hr = m_impl->audioClient->GetBufferSize(&m_impl->bufferFrameCount);
    if (FAILED(hr)) { Stop(); return false; }

    hr = m_impl->audioClient->GetService(__uuidof(IAudioRenderClient),
            reinterpret_cast<void**>(&m_impl->renderClient));
    if (FAILED(hr)) { Stop(); return false; }

    hr = m_impl->audioClient->Start();
    if (FAILED(hr)) { Stop(); return false; }

    m_workerShouldExit.store(false);
    m_workerRunning.store(true);
    m_workerThread = std::thread(&VoicePlayback::WorkerLoop, this);
    return true;
}

// --------------------------------------------------------------------
void VoicePlayback::Stop()
{
    if (m_workerRunning.load())
    {
        m_workerShouldExit.store(true);
        if (m_impl->bufferReady != nullptr)
        {
            SetEvent(m_impl->bufferReady);
        }
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
        m_workerRunning.store(false);
    }

    if (m_impl->audioClient != nullptr)
    {
        m_impl->audioClient->Stop();
    }
    SafeRelease(&m_impl->renderClient);
    SafeRelease(&m_impl->audioClient);
    SafeRelease(&m_impl->device);
    SafeRelease(&m_impl->deviceEnum);

    if (m_impl->mixFormat != nullptr)
    {
        CoTaskMemFree(m_impl->mixFormat);
        m_impl->mixFormat = nullptr;
    }
    if (m_impl->bufferReady != nullptr)
    {
        CloseHandle(m_impl->bufferReady);
        m_impl->bufferReady = nullptr;
    }
    if (m_impl->comInitialised)
    {
        CoUninitialize();
        m_impl->comInitialised = false;
    }
}

// --------------------------------------------------------------------
void VoicePlayback::InitialisePeerLocked(int64_t senderUserID)
{
    auto it = m_peers.find(senderUserID);
    if (it != m_peers.end()) return;

    auto* state = new PeerPlaybackState();
    state->decoder.Initialise();
    state->lastActivity = std::chrono::steady_clock::now();
    m_peers[senderUserID] = state;
}

// --------------------------------------------------------------------
void VoicePlayback::SubmitFrame(int64_t senderUserID, uint16_t sequence,
                                const uint8_t* encoded, int encodedLen)
{
    if (encoded == nullptr || encodedLen <= 0) return;

    std::lock_guard<std::mutex> lock(m_peersMutex);
    InitialisePeerLocked(senderUserID);
    auto* state = m_peers[senderUserID];
    if (state == nullptr || !state->decoder.IsReady()) return;

    // Decode straight away on the submitter's thread. This is fine -
    // decode is cheap (~0.1 ms per 20 ms frame) and keeps the audio
    // worker loop free of heavy lifting under the mix lock.
    PeerPlaybackState::Frame frame;
    frame.sequence = sequence;
    frame.samples.resize(kSamplesPerFrame);
    const int decoded = state->decoder.DecodeFrame(encoded, encodedLen,
                                                   frame.samples.data(),
                                                   kSamplesPerFrame);
    frame.valid = (decoded == kSamplesPerFrame);

    // Insert into queue ordered by sequence.
    auto insertPos = std::upper_bound(
        state->queue.begin(), state->queue.end(), frame,
        [](const PeerPlaybackState::Frame& a, const PeerPlaybackState::Frame& b) {
            return static_cast<int16_t>(a.sequence - b.sequence) < 0;
        });
    state->queue.insert(insertPos, std::move(frame));

    // Drop stale frames if queue overflows.
    while (static_cast<int>(state->queue.size()) > kMaxJitterFrames)
    {
        state->queue.pop_front();
    }

    // Mark primed once we have the target jitter depth.
    if (!state->primed &&
        static_cast<int>(state->queue.size()) >= kTargetJitterFrames)
    {
        state->primed = true;
        state->nextExpectedSeq = state->queue.front().sequence;
    }

    state->lastActivity = std::chrono::steady_clock::now();
}

// --------------------------------------------------------------------
void VoicePlayback::RemovePeer(int64_t senderUserID)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_peers.find(senderUserID);
    if (it != m_peers.end())
    {
        delete it->second;
        m_peers.erase(it);
    }
}

void VoicePlayback::SetPeerMuted(int64_t senderUserID, bool muted)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    InitialisePeerLocked(senderUserID);
    m_peers[senderUserID]->muted.store(muted);
}

bool VoicePlayback::IsPeerMuted(int64_t senderUserID)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_peers.find(senderUserID);
    return (it != m_peers.end()) ? it->second->muted.load() : false;
}

void VoicePlayback::SetPeerVolume(int64_t senderUserID, float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 2.0f) volume = 2.0f;
    std::lock_guard<std::mutex> lock(m_peersMutex);
    InitialisePeerLocked(senderUserID);
    m_peers[senderUserID]->volume.store(volume);
}

float VoicePlayback::GetPeerVolume(int64_t senderUserID)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_peers.find(senderUserID);
    return (it != m_peers.end()) ? it->second->volume.load() : 1.0f;
}

void VoicePlayback::SetGlobalVolume(float volume)
{
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 2.0f) volume = 2.0f;
    m_globalVolume.store(volume);
}

bool VoicePlayback::IsPeerSpeaking(int64_t senderUserID)
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    auto it = m_peers.find(senderUserID);
    if (it == m_peers.end()) return false;

    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - it->second->lastActivity).count();
    return static_cast<double>(ms) < kSpeakingTimeoutMs;
}

void VoicePlayback::ClearAllPeers()
{
    std::lock_guard<std::mutex> lock(m_peersMutex);
    for (auto& kv : m_peers) delete kv.second;
    m_peers.clear();
}

// --------------------------------------------------------------------
// Worker loop: pulls one chunk of mixed audio per WASAPI event.
// --------------------------------------------------------------------
void VoicePlayback::WorkerLoop()
{
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    const int dstRate     = m_impl->endpointSampleRate;
    const int dstChannels = m_impl->endpointChannels;
    const bool dstIsFloat = m_impl->endpointIsFloat;
    const int dstBits     = m_impl->endpointBitsPerSample;

    // Upsample ratio from 48 kHz source to endpoint.
    const double rateRatio =
        static_cast<double>(kSampleRate) / static_cast<double>(dstRate);

    // Per-frame mono mix buffer (48 kHz int16).
    std::vector<int16_t> monoMix;
    monoMix.reserve(kSamplesPerFrame * 4);

    // Carry for fractional resample position.
    double readPos = 0.0;

    while (!m_workerShouldExit.load())
    {
        DWORD waitResult = WaitForSingleObject(m_impl->bufferReady, 200);
        if (m_workerShouldExit.load()) break;
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(m_impl->audioClient->GetCurrentPadding(&padding))) continue;
        UINT32 framesAvailable = m_impl->bufferFrameCount - padding;
        if (framesAvailable == 0) continue;

        // How many source (48k mono) samples do we need for this chunk?
        const int needSrcSamples =
            static_cast<int>(framesAvailable * rateRatio) + 2;

        // Ensure the mono mix has enough samples by pulling frames
        // from each peer's jitter buffer.
        while (static_cast<int>(monoMix.size()) < needSrcSamples)
        {
            // Mix one 20 ms (kSamplesPerFrame) chunk across all peers.
            int32_t accum[kSamplesPerFrame] = {0};
            const float globalVolume = m_globalVolume.load();

            {
                std::lock_guard<std::mutex> lock(m_peersMutex);
                for (auto& kv : m_peers)
                {
                    auto* peer = kv.second;
                    if (peer == nullptr || !peer->primed) continue;
                    if (peer->muted.load()) continue;
                    if (peer->queue.empty()) continue;

                    // Pop the next expected frame. If the head doesn't
                    // match the expected seq, treat as lost and let the
                    // decoder produce PLC output.
                    PeerPlaybackState::Frame frame;
                    const auto& head = peer->queue.front();
                    if (head.sequence == peer->nextExpectedSeq)
                    {
                        frame = std::move(peer->queue.front());
                        peer->queue.pop_front();
                    }
                    else
                    {
                        // Missing frame - synthesise replacement.
                        frame.samples.resize(kSamplesPerFrame);
                        peer->decoder.DecodeLostFrame(frame.samples.data(),
                                                      kSamplesPerFrame);
                        frame.valid = true;
                    }
                    peer->nextExpectedSeq++;

                    if (frame.valid &&
                        static_cast<int>(frame.samples.size()) == kSamplesPerFrame)
                    {
                        // Per-peer volume fold-in. Skip the multiply
                        // entirely when the combined gain is unity so
                        // we don't pay for the floating point in the
                        // common case.
                        const float peerVolume = peer->volume.load();
                        const float combined   = peerVolume * globalVolume;
                        if (combined == 1.0f)
                        {
                            for (int i = 0; i < kSamplesPerFrame; ++i)
                            {
                                accum[i] += frame.samples[i];
                            }
                        }
                        else if (combined != 0.0f)
                        {
                            for (int i = 0; i < kSamplesPerFrame; ++i)
                            {
                                accum[i] += static_cast<int32_t>(
                                    static_cast<float>(frame.samples[i]) * combined);
                            }
                        }
                    }

                    // Un-prime if buffer drains completely so we
                    // re-buffer on the next talk-spurt.
                    if (peer->queue.empty()) peer->primed = false;
                }
            }

            for (int i = 0; i < kSamplesPerFrame; ++i)
            {
                int v = accum[i];
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                monoMix.push_back(static_cast<int16_t>(v));
            }
        }

        // Now resample + format-convert + fill the WASAPI buffer.
        BYTE* renderBuffer = nullptr;
        HRESULT hr = m_impl->renderClient->GetBuffer(framesAvailable, &renderBuffer);
        if (FAILED(hr) || renderBuffer == nullptr) continue;

        for (UINT32 outFrame = 0; outFrame < framesAvailable; ++outFrame)
        {
            const int srcIdx = static_cast<int>(readPos);
            int16_t monoSample = 0;
            if (srcIdx >= 0 && srcIdx < static_cast<int>(monoMix.size()))
            {
                monoSample = monoMix[srcIdx];
            }
            readPos += rateRatio;

            // Write into all destination channels.
            for (int ch = 0; ch < dstChannels; ++ch)
            {
                if (dstIsFloat && dstBits == 32)
                {
                    float* dst = reinterpret_cast<float*>(renderBuffer);
                    dst[outFrame * dstChannels + ch] =
                        static_cast<float>(monoSample) / 32768.0f;
                }
                else if (!dstIsFloat && dstBits == 16)
                {
                    int16_t* dst = reinterpret_cast<int16_t*>(renderBuffer);
                    dst[outFrame * dstChannels + ch] = monoSample;
                }
                else if (!dstIsFloat && dstBits == 32)
                {
                    int32_t* dst = reinterpret_cast<int32_t*>(renderBuffer);
                    dst[outFrame * dstChannels + ch] =
                        static_cast<int32_t>(monoSample) << 16;
                }
                // Other formats not supported - silent.
            }
        }

        // Shift consumed samples out of monoMix.
        const int consumed =
            static_cast<int>(readPos);
        if (consumed > 0)
        {
            if (consumed >= static_cast<int>(monoMix.size()))
            {
                monoMix.clear();
                readPos = 0.0;
            }
            else
            {
                monoMix.erase(monoMix.begin(), monoMix.begin() + consumed);
                readPos -= static_cast<double>(consumed);
            }
        }

        m_impl->renderClient->ReleaseBuffer(framesAvailable, 0);
    }

    if (mmcssHandle != nullptr)
    {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
}

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
