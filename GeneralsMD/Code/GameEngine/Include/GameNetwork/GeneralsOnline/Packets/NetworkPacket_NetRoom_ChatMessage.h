#pragma once

#include "../NGMP_include.h"

class NetRoom_ChatMessagePacket : public NetworkPacket
{
public:
	NetRoom_ChatMessagePacket(AsciiString& strMessage);

	NetRoom_ChatMessagePacket(CBitStream& bitstream);

	virtual CBitStream* Serialize() override;

	const std::string& GetMsg() const { return m_strMessage; }

private:
	std::string m_strMessage = "";
};