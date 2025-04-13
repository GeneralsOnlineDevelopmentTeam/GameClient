#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Pong.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

NetworkPacket_Pong::NetworkPacket_Pong() : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{

}

NetworkPacket_Pong::NetworkPacket_Pong(CBitStream& bitstream) : NetworkPacket(bitstream)
{

}

CBitStream* NetworkPacket_Pong::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_PONG);
	return pBitstream;
}