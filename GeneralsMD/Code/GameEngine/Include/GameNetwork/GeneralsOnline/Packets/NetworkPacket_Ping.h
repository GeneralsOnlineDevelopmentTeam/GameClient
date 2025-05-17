#pragma once

#include "GameNetwork/GeneralsOnline/NetworkPacket.h"

class NetworkPacket_Ping : public NetworkPacket
{
public:
	NetworkPacket_Ping();

	NetworkPacket_Ping(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;
};