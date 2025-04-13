#pragma once

#include "GameNetwork/GeneralsOnline/NetworkPacket.h"

class NetworkPacket_Pong : public NetworkPacket
{
public:
	NetworkPacket_Pong();

	NetworkPacket_Pong(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;
};