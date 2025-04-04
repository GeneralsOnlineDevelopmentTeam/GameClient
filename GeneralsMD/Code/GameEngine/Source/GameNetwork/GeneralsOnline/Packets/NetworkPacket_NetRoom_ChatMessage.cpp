#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"

NetRoom_ChatMessagePacket::NetRoom_ChatMessagePacket(AsciiString& strMessage) : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{
	m_strMessage = strMessage.str();
}

NetRoom_ChatMessagePacket::NetRoom_ChatMessagePacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{
	m_strMessage = bitstream.ReadString();
}

CBitStream* NetRoom_ChatMessagePacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG);
	pBitstream->WriteString(m_strMessage.c_str());
	return pBitstream;
}