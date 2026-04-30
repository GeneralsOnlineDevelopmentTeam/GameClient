// VoiceCapture.cpp

#include "PreRTS.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoiceCapture.h"
#include "GameNetwork/GeneralsOnline/Voice/VoiceOpusCodec.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <mmreg.h>
#include <ksmedia.h>

#include <vector>
#include <chrono>

namespace Voice
{

// --------------------------------------------------------------------
// Internal state. Kept in a pimpl so the WASAPI headers stay out of
// the public header file.
// --------------------------------------------------------------------
struct VoiceCapture::Impl
{
    IMMDeviceEnumerator* deviceEnum    = nullptr;
    IMMDevice*           device        = nullptr;
    IAudioClient*        audioClient   = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX*        mixFormat     = nullptr;
    HANDLE               bufferReady   = nullptr;
    bool                 comInitialised = false;

    // Endpoint may not deliver exactly 48 kHz mono - we always convert
    // on the fly in the worker loop. These cache the key numbers.
    int  endpointSampleRate = 0;
    int  endpointChannels   = 0;
    int  endpointBitsPerSample = 0;
    bool endpointIsFloat    = false;

    // Accumulator for building up 20 ms output frames.
    std::vector<int16_t> pendingSamples;
};

// --------------------------------------------------------------------
// Tiny helpers for releasing COM interfaces without dragging in ATL.
// --------------------------------------------------------------------
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
// Construction / destruction
// --------------------------------------------------------------------
VoiceCapture::VoiceCapture()
    : m_impl(new Impl)
    , m_workerRunning(false)
    , m_workerShouldExit(false)
    , m_transmitting(false)
    , m_currentLevel(0.0f)
{
}

VoiceCapture::~VoiceCapture()
{
    Stop();
    delete m_impl;
    m_impl = nullptr;
}

// --------------------------------------------------------------------
// Start: open WASAPI capture and spin up the worker thread.
// --------------------------------------------------------------------
bool VoiceCapture::Start(CaptureFrameCallback onFrame)
{
    if (m_workerRunning.load())
    {
        return true;
    }

    m_onFrame = std::move(onFrame);

    // COM init - OK to do this multiple times, Windows reference-
    // counts per thread. The main thread is usually STA because of
    // DirectInput/DirectShow initialisation.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == RPC_E_CHANGED_MODE)
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }
    m_impl->comInitialised = SUCCEEDED(hr);

    // Grab the default capture endpoint (default communications role
    // to pick up headset mics).
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&m_impl->deviceEnum));
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    // User-picked device takes precedence; otherwise the Windows default
    // communications endpoint (headset mic); otherwise console default.
    if (!m_requestedDeviceID.empty())
    {
        hr = m_impl->deviceEnum->GetDevice(m_requestedDeviceID.c_str(),
                                           &m_impl->device);
        if (FAILED(hr) || m_impl->device == nullptr)
        {
            // Explicit pick is gone (unplugged?) - fall through to default.
            m_impl->device = nullptr;
        }
    }
    if (m_impl->device == nullptr)
    {
        hr = m_impl->deviceEnum->GetDefaultAudioEndpoint(eCapture,
                eCommunications, &m_impl->device);
    }
    if (FAILED(hr) || m_impl->device == nullptr)
    {
        // Fall back to console endpoint if there's no comms default.
        hr = m_impl->deviceEnum->GetDefaultAudioEndpoint(eCapture,
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
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    // Use the endpoint's native mix format. Trying to force 48 kHz
    // mono PCM16 with AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM works on most
    // endpoints but some drivers reject it, so we do the conversion
    // ourselves in the worker loop.
    hr = m_impl->audioClient->GetMixFormat(&m_impl->mixFormat);
    if (FAILED(hr) || m_impl->mixFormat == nullptr)
    {
        Stop();
        return false;
    }

    m_impl->endpointSampleRate    = m_impl->mixFormat->nSamplesPerSec;
    m_impl->endpointChannels      = m_impl->mixFormat->nChannels;
    m_impl->endpointBitsPerSample = m_impl->mixFormat->wBitsPerSample;

    // Detect float vs PCM. WAVE_FORMAT_EXTENSIBLE is the common case
    // on Windows 10/11 and requires peeking at the sub-format GUID.
    m_impl->endpointIsFloat = false;
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

    // 60 ms buffer - gives the scheduler enough slack without adding
    // noticeable latency.
    const REFERENCE_TIME kBufferDuration = 60 * 10000; // 60 ms in hns
    hr = m_impl->audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            kBufferDuration, 0,
            m_impl->mixFormat, nullptr);
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    m_impl->bufferReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_impl->bufferReady == nullptr)
    {
        Stop();
        return false;
    }
    hr = m_impl->audioClient->SetEventHandle(m_impl->bufferReady);
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    hr = m_impl->audioClient->GetService(__uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&m_impl->captureClient));
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    hr = m_impl->audioClient->Start();
    if (FAILED(hr))
    {
        Stop();
        return false;
    }

    m_impl->pendingSamples.clear();
    m_impl->pendingSamples.reserve(kSamplesPerFrame * 2);

    m_workerShouldExit.store(false);
    m_workerRunning.store(true);
    m_workerThread = std::thread(&VoiceCapture::WorkerLoop, this);
    return true;
}

// --------------------------------------------------------------------
// Stop: tear down capture and join the worker.
// --------------------------------------------------------------------
void VoiceCapture::Stop()
{
    if (m_workerRunning.load())
    {
        m_workerShouldExit.store(true);
        if (m_impl->bufferReady != nullptr)
        {
            // Nudge the worker so it doesn't wait out the whole
            // WaitForSingleObject timeout.
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
    SafeRelease(&m_impl->captureClient);
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
    m_transmitting.store(false);
    m_currentLevel.store(0.0f);
}

void VoiceCapture::SetTransmitting(bool transmitting)
{
    m_transmitting.store(transmitting);
    if (!transmitting)
    {
        // Reset accumulator so a new PTT press starts on a fresh
        // 20 ms boundary.
        m_impl->pendingSamples.clear();
        m_currentLevel.store(0.0f);
    }
}

// --------------------------------------------------------------------
// Worker loop: pulls from WASAPI, converts to 48k mono int16, and
// emits 20 ms frames to the caller's callback.
// --------------------------------------------------------------------
void VoiceCapture::WorkerLoop()
{
    // MMCSS raises the thread priority for pro audio work.
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    const int srcRate     = m_impl->endpointSampleRate;
    const int srcChannels = m_impl->endpointChannels;
    const bool srcIsFloat = m_impl->endpointIsFloat;
    const int srcBits     = m_impl->endpointBitsPerSample;

    // Simple linear resampler state. For voice capture quality this
    // is more than good enough; Opus at 48k is where quality matters.
    double readPos = 0.0;
    const double rateRatio =
        static_cast<double>(srcRate) / static_cast<double>(kSampleRate);

    std::vector<int16_t>& pending = m_impl->pendingSamples;

    while (!m_workerShouldExit.load())
    {
        DWORD waitResult = WaitForSingleObject(m_impl->bufferReady, 200);
        if (m_workerShouldExit.load())
        {
            break;
        }
        if (waitResult != WAIT_OBJECT_0)
        {
            continue;
        }

        UINT32 packetLength = 0;
        HRESULT hr = m_impl->captureClient->GetNextPacketSize(&packetLength);
        while (SUCCEEDED(hr) && packetLength > 0)
        {
            BYTE*  rawData    = nullptr;
            UINT32 numFrames  = 0;
            DWORD  flags      = 0;
            hr = m_impl->captureClient->GetBuffer(&rawData, &numFrames, &flags,
                                                  nullptr, nullptr);
            if (FAILED(hr))
            {
                break;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            // Convert and resample into a per-packet scratch buffer.
            const int outFrameCount =
                static_cast<int>(numFrames / rateRatio + 1);
            std::vector<int16_t> scratch;
            scratch.reserve(outFrameCount);

            // Walk the source buffer picking samples nearest readPos.
            // Output ends when readPos exceeds numFrames - 1.
            while (readPos < static_cast<double>(numFrames))
            {
                const int srcIdx = static_cast<int>(readPos);
                int16_t mono = 0;
                if (!silent && rawData != nullptr)
                {
                    // Mix to mono by averaging channels.
                    long accum = 0;
                    for (int ch = 0; ch < srcChannels; ++ch)
                    {
                        const int sampleIdx = srcIdx * srcChannels + ch;
                        if (srcIsFloat && srcBits == 32)
                        {
                            const float f = reinterpret_cast<const float*>(rawData)[sampleIdx];
                            int s = static_cast<int>(f * 32767.0f);
                            if (s >  32767) s =  32767;
                            if (s < -32768) s = -32768;
                            accum += s;
                        }
                        else if (!srcIsFloat && srcBits == 16)
                        {
                            accum += reinterpret_cast<const int16_t*>(rawData)[sampleIdx];
                        }
                        else if (!srcIsFloat && srcBits == 32)
                        {
                            const int32_t s32 = reinterpret_cast<const int32_t*>(rawData)[sampleIdx];
                            accum += (s32 >> 16);
                        }
                        // Other formats: treat as silence.
                    }
                    mono = static_cast<int16_t>(accum / (srcChannels > 0 ? srcChannels : 1));
                }
                scratch.push_back(mono);
                readPos += rateRatio;
            }
            readPos -= static_cast<double>(numFrames);

            hr = m_impl->captureClient->ReleaseBuffer(numFrames);
            if (FAILED(hr))
            {
                break;
            }

            // Append to pending, then emit as many complete frames as
            // we have - but only while PTT is held.
            if (m_transmitting.load() && m_onFrame)
            {
                pending.insert(pending.end(), scratch.begin(), scratch.end());

                while (static_cast<int>(pending.size()) >= kSamplesPerFrame)
                {
                    // Apply mic gain (if not unity) with clipping.
                    const float gain = m_micGain.load();
                    if (gain != 1.0f)
                    {
                        for (int i = 0; i < kSamplesPerFrame; ++i)
                        {
                            int v = static_cast<int>(
                                static_cast<float>(pending[i]) * gain);
                            if (v >  32767) v =  32767;
                            if (v < -32768) v = -32768;
                            pending[i] = static_cast<int16_t>(v);
                        }
                    }

                    // Compute peak for the level meter (post-gain, so the
                    // UI reflects what is actually being sent).
                    int peak = 0;
                    for (int i = 0; i < kSamplesPerFrame; ++i)
                    {
                        const int v = std::abs(static_cast<int>(pending[i]));
                        if (v > peak) peak = v;
                    }
                    m_currentLevel.store(static_cast<float>(peak) / 32768.0f);

                    m_onFrame(pending.data(), kSamplesPerFrame);
                    pending.erase(pending.begin(),
                                  pending.begin() + kSamplesPerFrame);
                }
            }
            else
            {
                // Not transmitting - drop.
                pending.clear();
                m_currentLevel.store(0.0f);
            }

            hr = m_impl->captureClient->GetNextPacketSize(&packetLength);
        }
    }

    if (mmcssHandle != nullptr)
    {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
}

// --------------------------------------------------------------------
// Mic gain
// --------------------------------------------------------------------
void VoiceCapture::SetMicGain(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    m_micGain.store(gain);
}

// --------------------------------------------------------------------
// Device enumeration
//
// We build a temporary device enumerator (independent of the running
// capture stream, if any) and walk every active capture endpoint.
// Default-comms and default-console are queried separately so the UI
// can mark them in a device picker.
// --------------------------------------------------------------------
std::vector<CaptureDeviceInfo> VoiceCapture::EnumerateDevices()
{
    std::vector<CaptureDeviceInfo> results;

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit =
        (hrCo == S_OK || hrCo == S_FALSE); // S_FALSE: already init'd on this thread

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr)
    {
        if (needUninit) CoUninitialize();
        return results;
    }

    // Resolve default comms and console endpoint ids for flagging.
    std::wstring defaultCommsID;
    std::wstring defaultConsoleID;
    {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture,
                eCommunications, &dev)) && dev != nullptr)
        {
            LPWSTR idStr = nullptr;
            if (SUCCEEDED(dev->GetId(&idStr)) && idStr != nullptr)
            {
                defaultCommsID.assign(idStr);
                CoTaskMemFree(idStr);
            }
            dev->Release();
        }
    }
    {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture,
                eConsole, &dev)) && dev != nullptr)
        {
            LPWSTR idStr = nullptr;
            if (SUCCEEDED(dev->GetId(&idStr)) && idStr != nullptr)
            {
                defaultConsoleID.assign(idStr);
                CoTaskMemFree(idStr);
            }
            dev->Release();
        }
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                        &collection);
    if (SUCCEEDED(hr) && collection != nullptr)
    {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(collection->Item(i, &dev)) || dev == nullptr) continue;

            CaptureDeviceInfo info;

            LPWSTR idStr = nullptr;
            if (SUCCEEDED(dev->GetId(&idStr)) && idStr != nullptr)
            {
                info.id.assign(idStr);
                CoTaskMemFree(idStr);
            }

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) &&
                props != nullptr)
            {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName,
                                              &varName)) &&
                    varName.vt == VT_LPWSTR && varName.pwszVal != nullptr)
                {
                    info.friendlyName.assign(varName.pwszVal);
                }
                PropVariantClear(&varName);
                props->Release();
            }

            info.isDefaultCommunications =
                (!info.id.empty() && info.id == defaultCommsID);
            info.isDefaultConsole =
                (!info.id.empty() && info.id == defaultConsoleID);

            if (!info.id.empty())
            {
                results.push_back(std::move(info));
            }
            dev->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    if (needUninit) CoUninitialize();
    return results;
}

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
