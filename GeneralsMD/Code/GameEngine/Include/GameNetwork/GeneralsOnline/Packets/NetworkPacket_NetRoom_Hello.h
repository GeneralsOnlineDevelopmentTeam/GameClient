#pragma once

#include "GameNetwork/GeneralsOnline/NetworkPacket.h"

class NetRoom_HelloPacket : public NetworkPacket
{
public:
	NetRoom_HelloPacket(int64_t user_id);

	NetRoom_HelloPacket(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;

	int64_t GetUserID() const { return m_user_id; }

private:
	int64_t m_user_id = -1;
};