#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_Hello.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

NetRoom_HelloPacket::NetRoom_HelloPacket(int64_t user_id) : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{
	m_user_id = user_id;
}

NetRoom_HelloPacket::NetRoom_HelloPacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{
	m_user_id = bitstream.Read<int64_t>();
}

CBitStream* NetRoom_HelloPacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_NET_ROOM_HELLO);
	pBitstream->Write<int64_t>(m_user_id);
	return pBitstream;
}

Net_ChallengePacket::Net_ChallengePacket() : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{

}

Net_ChallengePacket::Net_ChallengePacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{

}

CBitStream* Net_ChallengePacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_CHALLENGE);
	return pBitstream;
}

Net_ChallengeRespPacket::Net_ChallengeRespPacket(int64_t myUserID) : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{
	m_user_id = myUserID;
}

Net_ChallengeRespPacket::Net_ChallengeRespPacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{
	m_user_id = bitstream.Read<int64_t>();
}

CBitStream* Net_ChallengeRespPacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_CHALLENGE_RESP);
	pBitstream->Write<int64_t>(m_user_id);
	return pBitstream;
}
