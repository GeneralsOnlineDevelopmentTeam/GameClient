#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Ping.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

NetworkPacket_Ping::NetworkPacket_Ping() : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{

}

NetworkPacket_Ping::NetworkPacket_Ping(CBitStream& bitstream) : NetworkPacket(bitstream)
{

}

CBitStream* NetworkPacket_Ping::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_PING);
	return pBitstream;
}