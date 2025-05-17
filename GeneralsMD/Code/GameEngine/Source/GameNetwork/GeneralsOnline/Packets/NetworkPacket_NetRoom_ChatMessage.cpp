#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_ChatMessage.h"

NetRoom_ChatMessagePacket::NetRoom_ChatMessagePacket(AsciiString& strMessage, bool bIsAnnouncement, bool bShowAnnounceToHost) : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{
	m_strMessage = strMessage.str();
	m_bIsAnnouncement = bIsAnnouncement;
	m_bShowAnnounceToHost = bShowAnnounceToHost;
}

NetRoom_ChatMessagePacket::NetRoom_ChatMessagePacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{
	m_strMessage = bitstream.ReadString();
	m_bIsAnnouncement = bitstream.Read<bool>();
	m_bShowAnnounceToHost = bitstream.Read<bool>();
}

CBitStream* NetRoom_ChatMessagePacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_NET_ROOM_CHAT_MSG);
	pBitstream->WriteString(m_strMessage.c_str());
	pBitstream->Write<bool>(m_bIsAnnouncement);
	pBitstream->Write<bool>(m_bShowAnnounceToHost);
	return pBitstream;
}