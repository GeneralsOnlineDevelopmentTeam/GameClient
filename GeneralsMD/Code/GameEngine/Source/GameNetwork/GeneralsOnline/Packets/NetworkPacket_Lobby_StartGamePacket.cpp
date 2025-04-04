#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Lobby_StartGame.h"


Lobby_StartGamePacket::Lobby_StartGamePacket() : NetworkPacket(EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
{

}

Lobby_StartGamePacket::Lobby_StartGamePacket(CBitStream& bitstream) : NetworkPacket(bitstream)
{

}

CBitStream* Lobby_StartGamePacket::Serialize()
{
	CBitStream* pBitstream = new CBitStream(EPacketID::PACKET_ID_LOBBY_START_GAME);
	return pBitstream;
}