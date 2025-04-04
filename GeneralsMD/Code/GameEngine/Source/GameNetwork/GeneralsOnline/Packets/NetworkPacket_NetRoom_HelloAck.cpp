#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_NetRoom_HelloAck.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

NetRoom_HelloAckPacket::NetRoom_HelloAckPacket() : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{

}

NetRoom_HelloAckPacket::NetRoom_HelloAckPacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{

}

CBitStream* NetRoom_HelloAckPacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_NET_ROOM_HELLO_ACK);
	return pBitstream;
}