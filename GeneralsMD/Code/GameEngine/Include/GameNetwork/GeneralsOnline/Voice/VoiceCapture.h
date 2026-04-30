// VoiceCapture.h
//
// WASAPI-based microphone capture for lobby voice chat. Runs its own
// worker thread that pulls packets from the OS capture endpoint,
// resamples/converts to 48 kHz mono PCM16 if needed, and hands
// completed 20 ms frames to a caller-supplied callback. The callback
// is invoked on the capture worker thread - callers must be
// thread-safe or marshal work back to the main thread themselves.
//
// Lifecycle:
//   1. Construct (cheap, does nothing).
//   2. Start() - opens the default capture endpoint, starts the
//      worker thread, begins delivering frames. Returns false if no
//      microphone is available or WASAPI initialisation fails.
//   3. SetTransmitting(true/false) - gates whether the worker actually
//      calls the frame callback. When false the mic is still open but
//      frames are discarded. This models push-to-talk without
//      constantly stopping/restarting the capture stream.
//   4. Stop() - joins the worker and releases the endpoint.
//   5. Destruct.
//
// The capture format is requested as 48 kHz mono PCM16 shared-mode. If
// the endpoint rejects that, we fall back to the endpoint's native
// mix format and convert on the fly using Windows' automatic format
// conversion inside IAudioClient (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
// plus AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY).

#pragma once

#ifdef ENABLE_VOICE_CHAT

#include <cstdint>
#include <functional>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace Voice
{

// Callback signature: receives one 20 ms frame of 48 kHz mono PCM16
// (960 int16 samples). The pointer is valid only for the duration of
// the call; copy if you need to keep it.
using CaptureFrameCallback = std::function<void(const int16_t* pcm, int sampleCount)>;

// Description of a capture endpoint as returned by
// VoiceCapture::EnumerateDevices. The id is the opaque WASAPI endpoint
// id string (suitable for IMMDeviceEnumerator::GetDevice); the
// friendlyName is for presenting to the user.
struct CaptureDeviceInfo
{
    std::wstring id;
    std::wstring friendlyName;
    bool         isDefaultCommunications = false;
    bool         isDefaultConsole        = false;
};

class VoiceCapture
{
public:
    VoiceCapture();
    ~VoiceCapture();

    VoiceCapture(const VoiceCapture&) = delete;
    VoiceCapture& operator=(const VoiceCapture&) = delete;

    // Opens the default capture endpoint and starts the worker thread.
    // Returns false on any failure (no mic, no permission, WASAPI
    // unavailable); the object is safe to destruct in that state.
    bool Start(CaptureFrameCallback onFrame);

    // Stops the worker thread and releases the WASAPI objects. Safe
    // to call multiple times.
    void Stop();

    // When false, captured audio is dropped on the floor instead of
    // being passed to the callback. This is how push-to-talk is
    // implemented: the mic stream keeps running (avoids click/pop on
    // enable) but frames only reach the network path while the PTT
    // key is held.
    void SetTransmitting(bool transmitting);

    bool IsRunning() const { return m_workerRunning.load(); }

    // Peak level of the most recent transmitted frame, 0.0 - 1.0.
    // Useful for a "mic level" indicator in the UI.
    float GetCurrentLevel() const { return m_currentLevel.load(); }

    // ---------- Device selection -----------------------------------
    // Enumerate all active capture endpoints. Safe to call before or
    // after Start() - uses its own temporary device enumerator so it
    // doesn't touch the running capture stream.
    static std::vector<CaptureDeviceInfo> EnumerateDevices();

    // Select a capture endpoint by its WASAPI id string. Pass an empty
    // string to go back to the Windows default communications endpoint.
    // Only takes effect on the next Start(): call Stop() then Start()
    // again to apply at runtime.
    void SetDeviceID(const std::wstring& id) { m_requestedDeviceID = id; }
    const std::wstring& GetDeviceID() const  { return m_requestedDeviceID; }

    // ---------- Mic gain -------------------------------------------
    // Linear multiplier applied to captured samples before Opus encode.
    // 1.0 = unity (no change), 2.0 = +6 dB, 0.0 = mute.
    // Clamped to [0.0, 4.0] internally. Thread-safe.
    void  SetMicGain(float gain);
    float GetMicGain() const { return m_micGain.load(); }

private:
    // Runs on the worker thread.
    void WorkerLoop();

    // Opaque forward declaration so the WASAPI headers don't leak
    // through to every TU that includes this file.
    struct Impl;
    Impl* m_impl;

    CaptureFrameCallback m_onFrame;
    std::atomic<bool>    m_workerRunning;
    std::atomic<bool>    m_workerShouldExit;
    std::atomic<bool>    m_transmitting;
    std::atomic<float>   m_currentLevel;
    std::atomic<float>   m_micGain{1.0f};
    std::wstring         m_requestedDeviceID; // empty => default comms
    std::thread          m_workerThread;
};

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
