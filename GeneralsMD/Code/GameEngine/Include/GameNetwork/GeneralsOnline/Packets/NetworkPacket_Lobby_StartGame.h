#pragma once

#include "../NGMP_include.h"
#include "../NetworkBitstream.h"

class Lobby_StartGamePacket : public NetworkPacket
{
public:
	Lobby_StartGamePacket();

	Lobby_StartGamePacket(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;
};