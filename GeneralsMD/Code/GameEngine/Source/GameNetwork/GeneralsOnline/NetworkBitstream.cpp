#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"

#define SODIUM_STATIC 1
#include "GameNetwork/GeneralsOnline/vendor/libsodium/sodium.h"
#pragma comment(lib, "libsodium/libsodium.lib")

CBitStream::CBitStream(EPacketID packetID)
{
	m_memBuffer = MemoryBuffer(BITSTREAM_DEFAULT_SIZE);

	m_Offset = 0;
	m_packetID = packetID;

	Write(packetID);
}

#define ENABLE_ENCRYPTION
void CBitStream::Decrypt(std::vector<BYTE>& vecKey)
{
#if defined(ENABLE_ENCRYPTION)

	// first N bytes (crypto_aead_xchacha20poly1305_ietf_NPUBBYTES) is the nonce
	std::vector<uint8_t> nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
	memcpy(nonce.data(), &m_memBuffer.GetData()[0], crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

	// data is at membuffer + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES
	std::vector<unsigned char> vecDecryptedBytes(m_memBuffer.GetAllocatedSize());
	unsigned long long decrypted_len = 0;
	if (crypto_aead_xchacha20poly1305_ietf_decrypt(&vecDecryptedBytes.data()[0], &decrypted_len,
		NULL,
		&m_memBuffer.GetData()[0] + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, m_memBuffer.GetAllocatedSize() - crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
		nullptr,
		0,
		nonce.data(), &vecKey.data()[0]) != 0)
	{
		/* message forged! */
		NetworkLog("[NGMP]: Message forged! Decrypt failed");
	}
	else
	{
		// resize buffer
		vecDecryptedBytes.resize(decrypted_len);

		// reset the read offset
		m_Offset = 0;

		m_memBuffer.ReAllocate(decrypted_len);
		memcpy(&m_memBuffer.GetData()[0], &vecDecryptedBytes[0], decrypted_len);
	}

#endif
}



void CBitStream::Encrypt(std::vector<BYTE>& vecKey)
{
#if defined(ENABLE_ENCRYPTION)
	std::vector<unsigned char> ciphertext(GetNumBytesUsed() + crypto_aead_xchacha20poly1305_ietf_ABYTES);

	unsigned long long ciphertext_len;

	//NetworkLog("Encrypting message %s", MESSAGE);

	// create nonce
	std::vector<uint8_t> nonce(crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
	randombytes_buf(nonce.data(), nonce.size());

	// encrypt current buffer
	crypto_aead_xchacha20poly1305_ietf_encrypt(&ciphertext.data()[0], &ciphertext_len,
		GetRawBuffer(), GetNumBytesUsed(),
		nullptr, 0,
		NULL, nonce.data(), &vecKey.data()[0]);

	// resize buffer and copy back
	ciphertext.resize(ciphertext_len);

	// make sure there is enough space for encrypted data + nonce
	m_memBuffer.ReAllocate(ciphertext_len + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

	// copy back the nonce
	memcpy(&m_memBuffer.GetData()[0], nonce.data(), crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);

	// copy back encrypted data
	memcpy(&m_memBuffer.GetData()[0] + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES, &ciphertext[0], ciphertext.size());

	// fix the offset
	m_Offset = ciphertext.size() + crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
#endif
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