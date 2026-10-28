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
