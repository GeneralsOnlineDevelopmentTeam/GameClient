// VoiceOpusCodec.cpp

#include "PreRTS.h"

#ifdef ENABLE_VOICE_CHAT

#include "GameNetwork/GeneralsOnline/Voice/VoiceOpusCodec.h"

#include <opus/opus.h>

namespace Voice
{

// ------------------------------------------------------------------
// OpusEncoderWrapper
// ------------------------------------------------------------------
OpusEncoderWrapper::OpusEncoderWrapper()
    : m_encoder(nullptr)
{
}

OpusEncoderWrapper::~OpusEncoderWrapper()
{
    Shutdown();
}

bool OpusEncoderWrapper::Initialise(int bitrateBps)
{
    Shutdown();

    int error = OPUS_OK;
    m_encoder = opus_encoder_create(kSampleRate, kChannels,
                                    OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK || m_encoder == nullptr)
    {
        m_encoder = nullptr;
        return false;
    }

    // Speech-oriented configuration.
    opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(bitrateBps));
    opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(m_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(m_encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));
    return true;
}

void OpusEncoderWrapper::Shutdown()
{
    if (m_encoder != nullptr)
    {
        opus_encoder_destroy(m_encoder);
        m_encoder = nullptr;
    }
}

int OpusEncoderWrapper::EncodeFrame(const int16_t* pcmIn, uint8_t* encodedOut,
                                    int encodedOutCapacity)
{
    if (m_encoder == nullptr || pcmIn == nullptr ||
        encodedOut == nullptr || encodedOutCapacity <= 0)
    {
        return 0;
    }

    const int result = opus_encode(m_encoder, pcmIn, kSamplesPerFrame,
                                   encodedOut, encodedOutCapacity);
    if (result < 0)
    {
        return 0;
    }
    return result;
}

// ------------------------------------------------------------------
// OpusDecoderWrapper
// ------------------------------------------------------------------
OpusDecoderWrapper::OpusDecoderWrapper()
    : m_decoder(nullptr)
{
}

OpusDecoderWrapper::~OpusDecoderWrapper()
{
    Shutdown();
}

bool OpusDecoderWrapper::Initialise()
{
    Shutdown();

    int error = OPUS_OK;
    m_decoder = opus_decoder_create(kSampleRate, kChannels, &error);
    if (error != OPUS_OK || m_decoder == nullptr)
    {
        m_decoder = nullptr;
        return false;
    }
    return true;
}

void OpusDecoderWrapper::Shutdown()
{
    if (m_decoder != nullptr)
    {
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
}

int OpusDecoderWrapper::DecodeFrame(const uint8_t* encodedIn, int encodedLen,
                                    int16_t* pcmOut, int pcmOutCapacitySamples)
{
    if (m_decoder == nullptr || encodedIn == nullptr || encodedLen <= 0 ||
        pcmOut == nullptr || pcmOutCapacitySamples < kSamplesPerFrame)
    {
        return 0;
    }

    const int result = opus_decode(m_decoder, encodedIn, encodedLen,
                                   pcmOut, kSamplesPerFrame, /*decode_fec=*/0);
    if (result < 0)
    {
        return 0;
    }
    return result;
}

int OpusDecoderWrapper::DecodeLostFrame(int16_t* pcmOut, int pcmOutCapacitySamples)
{
    if (m_decoder == nullptr || pcmOut == nullptr ||
        pcmOutCapacitySamples < kSamplesPerFrame)
    {
        return 0;
    }

    const int result = opus_decode(m_decoder, nullptr, 0,
                                   pcmOut, kSamplesPerFrame, /*decode_fec=*/0);
    if (result < 0)
    {
        return 0;
    }
    return result;
}

} // namespace Voice

#endif // ENABLE_VOICE_CHAT
