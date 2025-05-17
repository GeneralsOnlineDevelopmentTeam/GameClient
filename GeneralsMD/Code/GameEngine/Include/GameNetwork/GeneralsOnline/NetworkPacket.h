#pragma once

#include <stdint.h>

class CBitStream;

enum EPacketReliability : uint8_t
{
	PACKET_RELIABILITY_UNRELIABLE_UNORDERED, /* Packets will only be sent once and may be received out of order */
	PACKET_RELIABILITY_UNRELIABLE_UNORDERED_DISCARD_OUT_OF_ORDER, /* Packets will only be sent once and will be discarded if out of order */
	PACKET_RELIABILITY_RELIABLE_UNORDERED, /* Packets may be sent multiple times and may be received out of order */
	PACKET_RELIABILITY_RELIABLE_ORDERED, /* Packets may be sent multiple times and will be received in order */
};

enum EPacketCategory
{
	EVENT
};

class NetworkPacket
{
public:
	// send
	NetworkPacket(EPacketReliability reliability)
	{
		m_Reliability = reliability;
	}

	// receive
	NetworkPacket(CBitStream& bitstream)
	{
		// We don't really care on the receivers side
		m_Reliability = EPacketReliability::PACKET_RELIABILITY_UNRELIABLE_UNORDERED;
	}

	virtual CBitStream* Serialize() = 0;

	EPacketReliability GetReliability() const { return m_Reliability; }
protected:
	EPacketReliability m_Reliability;
};