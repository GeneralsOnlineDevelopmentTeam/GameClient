#pragma once

#include <cstdint>

// Little endian wire values, the game runs on little endian targets only

inline void WriteU16(uint8_t* out, uint16_t value)
{
	out[0] = static_cast<uint8_t>(value & 0xFF);
	out[1] = static_cast<uint8_t>(value >> 8);
}

inline uint16_t ReadU16(const uint8_t* in)
{
	return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

inline void WriteU64(uint8_t* out, uint64_t value)
{
	for (int i = 0; i < 8; ++i)
	{
		out[i] = static_cast<uint8_t>(value >> (8 * i));
	}
}

inline uint64_t ReadU64(const uint8_t* in)
{
	uint64_t value = 0;
	for (int i = 0; i < 8; ++i)
	{
		value |= static_cast<uint64_t>(in[i]) << (8 * i);
	}
	return value;
}
