// VoiceOpusCodec.h
//
// Thin RAII wrappers around libopus for voice chat. Encoder is fixed at
// 48 kHz mono, 20 ms frames (960 samples), targeting ~16 kbps which is
// plenty for speech. Decoder is symmetric and includes Opus' built-in
// packet-loss concealment so dropped frames don't produce audible gaps.
//
// These classes are deliberately minimal and own no threads or buffers;
// callers supply the PCM buffers. That keeps them trivially testable
// and lets the capture/playback side decide lifetime and ownership.

#pragma once

#ifdef ENABLE_VOICE_CHAT

#include <cstdint>
#include <cstddef>

struct OpusEncoder;
struct OpusDecoder;

namespace Voice
{

// All voice frames in this system are 48 kHz mono PCM16 with 20 ms
// frames. Constants are exposed so the capture/playback modules can
// size their ring buffers without guessing.
constexpr int kSampleRate       = 48000;
constexpr int kChannels         = 1;
constexpr int kFrameDurationMs  = 20;
constexpr int kSamplesPerFrame  = (kSampleRate * kFrameDurationMs) / 1000; // 960
constexpr int kBytesPerPcmFrame = kSamplesPerFrame * kChannels * 2;        // 1920
constexpr int kMaxEncodedBytes  = 256;  // safe upper bound for ~16-24 kbps Opus

// ------------------------------------------------------------------
// OpusEncoderWrapper
// ------------------------------------------------------------------
class OpusEncoderWrapper
{
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();

    OpusEncoderWrapper(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper& operator=(const OpusEncoderWrapper&) = delete;

    // Initialise the encoder. Returns true on success. Safe to call
    // again after failure. bitrate is in bits per second; 16000-24000
    // is the sweet spot for speech.
    bool Initialise(int bitrateBps = 20000);

    // Release the encoder. Idempotent.
    void Shutdown();

    // Encode exactly one frame of 960 interleaved PCM16 samples.
    // Returns the number of bytes written into encodedOut, or 0 on
    // failure. encodedOut must be at least kMaxEncodedBytes.
    int EncodeFrame(const int16_t* pcmIn, uint8_t* encodedOut, int encodedOutCapacity);

    bool IsReady() const { return m_encoder != nullptr; }

private:
    OpusEncoder* m_encoder;
};

// ------------------------------------------------------------------
// OpusDecoderWrapper
// ------------------------------------------------------------------
class OpusDecoderWrapper
{
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();

    OpusDecoderWrapper(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper& operator=(const OpusDecoderWrapper&) = delete;

    bool Initialise();
    void Shutdown();

    // Decode one encoded packet into 960 PCM16 samples. Returns the
    // number of samples produced (should always be kSamplesPerFrame)
    // or 0 on failure.
    int DecodeFrame(const uint8_t* encodedIn, int encodedLen,
                    int16_t* pcmOut, int pcmOutCapacitySamples);

    // Call when a frame is missing to let Opus synthesise a
    // replacement via packet-loss concealment. pcmOut must hold
    // kSamplesPerFrame samples.
    int DecodeLostFrame(int16_t* pcmOut, int pcmOutCapacitySamples);

    bool IsReady() const { return m_decoder != nullptr; }

private:
    OpusDecoder* m_decoder;
};

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
