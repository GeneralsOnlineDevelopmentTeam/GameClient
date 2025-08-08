#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

CBitStream::CBitStream(EPacketID packetID)
{
	m_memBuffer = MemoryBuffer(BITSTREAM_DEFAULT_SIZE);

	m_Offset = 0;
	m_packetID = packetID;

	Write(packetID);
}

CBitStream::CBitStream(int64_t len, void* pBuffer, size_t sz)
{
	m_memBuffer = MemoryBuffer(len);

	memcpy(m_memBuffer.GetData() + m_Offset, pBuffer, sz);
	m_Offset += sz;
}

CBitStream::CBitStream(CBitStream* bsIn)
{
	m_memBuffer = MemoryBuffer(bsIn->GetNumBytesUsed());

	memcpy(m_memBuffer.GetData() + m_Offset, bsIn->GetRawBuffer(), bsIn->GetNumBytesUsed());

	m_packetID = bsIn->GetPacketID();
	m_Offset = bsIn->GetNumBytesUsed();
}

CBitStream::CBitStream(int64_t len)
{
	m_memBuffer = MemoryBuffer(len);
}

CBitStream::CBitStream(std::vector<BYTE> vecBytes)
{
	m_memBuffer = MemoryBuffer(vecBytes.size());
	memcpy(m_memBuffer.GetData(), (void*)vecBytes.data(), vecBytes.size());
	m_Offset = vecBytes.size();
}

CBitStream::~CBitStream()
{

}