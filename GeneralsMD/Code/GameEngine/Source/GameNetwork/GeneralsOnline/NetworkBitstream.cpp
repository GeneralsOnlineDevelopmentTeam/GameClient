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
static bool didOneTimeGen = false;
unsigned char g_nonce[crypto_aead_aegis256_NPUBBYTES];
unsigned char g_key[crypto_aead_aegis256_KEYBYTES];

void CBitStream::Decrypt()
{
#if defined(ENABLE_ENCRYPTION)
#define ADDITIONAL_DATA (const unsigned char *) "123456"
#define ADDITIONAL_DATA_LEN 6

	std::vector<unsigned char> vecDecryptedBytes(m_memBuffer.GetAllocatedSize());
	unsigned long long decrypted_len;
	if (crypto_aead_aegis256_decrypt(&vecDecryptedBytes.data()[0], &decrypted_len,
		NULL,
		&m_memBuffer.GetData()[0], m_memBuffer.GetAllocatedSize(),
		ADDITIONAL_DATA,
		ADDITIONAL_DATA_LEN,
		g_nonce, g_key) != 0)
	{
		/* message forged! */
	}

	// resize buffer
	vecDecryptedBytes.resize(decrypted_len);

	m_memBuffer.ReAllocate(decrypted_len);
	memcpy(&m_memBuffer.GetData()[0], &vecDecryptedBytes[0], decrypted_len);
#endif
}



void CBitStream::Encrypt()
{
#if defined(ENABLE_ENCRYPTION)
	#define ENCRYPT_TEST_FIXED_DATA
	
	// encrypt
	#define ADDITIONAL_DATA (const unsigned char *) "123456"
	#define ADDITIONAL_DATA_LEN 6

	

	if (!didOneTimeGen)
	{
#if defined(ENCRYPT_TEST_FIXED_DATA)
		memset(g_key, 1, crypto_aead_aegis256_KEYBYTES);
		memset(g_nonce, 1, sizeof(g_nonce));
#else
		crypto_aead_aegis256_keygen(g_key);
		randombytes_buf(g_nonce, sizeof g_nonce);
#endif

		didOneTimeGen = true;
	}
	std::vector<unsigned char> ciphertext(GetNumBytesUsed() + crypto_aead_aegis256_ABYTES);

	unsigned long long ciphertext_len;

	//NetworkLog("Encrypting message %s", MESSAGE);

	

	crypto_aead_aegis256_encrypt(&ciphertext.data()[0], &ciphertext_len,
		GetRawBuffer(), GetNumBytesUsed(),
		ADDITIONAL_DATA, ADDITIONAL_DATA_LEN,
		NULL, g_nonce, g_key);

	// resize buffer and copy back
	ciphertext.resize(ciphertext_len);

	//m_memBuffer.ReAllocate(ciphertext_len);
	memcpy(&m_memBuffer.GetData()[0], &ciphertext[0], ciphertext.size());
	m_Offset = ciphertext.size();
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