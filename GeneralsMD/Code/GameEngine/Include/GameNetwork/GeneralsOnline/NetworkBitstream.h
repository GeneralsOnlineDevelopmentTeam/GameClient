#pragma once

#include "NGMP_include.h"

#define BITSTREAM_DEFAULT_SIZE 1024

enum EPacketID
{
	PACKET_ID_NONE = -1,
};

class MemoryBuffer
{
public:
	MemoryBuffer()
	{
		m_bOwnership = true;
		m_pBuffer = nullptr;
		m_bufferSize = 0;
	}

	// reading over network, enet allocated and owns the memory
	MemoryBuffer(uint8_t* pExistingBuffer, size_t size)
	{
		m_bOwnership = false;
		m_pBuffer = pExistingBuffer;
		m_bufferSize = size;
		/*
		m_pBuffer = (uint8_t*)malloc(size);
		memset(m_pBuffer, 0, size);
		m_bufferSize = size;
		*/
	}

	MemoryBuffer(size_t size)
	{
		m_bOwnership = true;
		m_pBuffer = (uint8_t*)malloc(size);
		memset(m_pBuffer, 0, size);
		m_bufferSize = size;
	}

	void ReAllocate(size_t size)
	{
		if (m_bOwnership)
		{
			m_pBuffer = (uint8_t*)realloc((void*)m_pBuffer, size);
			m_bufferSize = size;
		}
	}

	MemoryBuffer& operator=(MemoryBuffer&& rhs)
	{
		if (&rhs == this)
			return *this;

		std::swap(m_pBuffer, rhs.m_pBuffer);
		std::swap(m_bufferSize, rhs.m_bufferSize);
		std::swap(m_bOwnership, rhs.m_bOwnership);

		return *this;
	}

	~MemoryBuffer()
	{
		if (m_pBuffer != nullptr)
		{
			if (m_bOwnership)
			{
				free(m_pBuffer);
				m_pBuffer = nullptr;
			}
		}
	}

	uint8_t* GetData() const { return m_pBuffer; }
	size_t GetAllocatedSize() const { return m_bufferSize; }

protected:
	uint8_t* m_pBuffer = nullptr;
	size_t m_bufferSize = 0;

	bool m_bOwnership = false;
};

class CBitStream
{
public:
	CBitStream()
	{
		m_memBuffer = MemoryBuffer();
	};

	void Encrypt(std::vector<BYTE>& vecKey);
	void Decrypt(std::vector<BYTE>& vecKey);

	CBitStream(EPacketID packetID);
	CBitStream(int64_t len);
	CBitStream(int64_t len, void* pBuffer, size_t sz);
	CBitStream(std::vector<BYTE> vecBytes);
	CBitStream(CBitStream* bsIn);
	~CBitStream();

	// reading over network, enet allocated and owns the memory
	CBitStream(uint8_t* pExistingBuffer, size_t size, EPacketID packetID)
	{
		// TODO_NGMP: Mark that we dont own this buffer and should not free it, or free it via Enet functions
		m_memBuffer = MemoryBuffer(pExistingBuffer, size);

		m_packetID = packetID;

		// set offset to 0, we're about to read, not write
		m_Offset = 0;
	}

	template<typename T>
	void Write(T val)
	{
		// bounds check
		if (m_Offset + sizeof(val) > m_memBuffer.GetAllocatedSize())
		{
			NetworkLog(ELogVerbosity::LOG_DEBUG, "[NGMP] Size after writing will be %d, but buffer is only %d", m_Offset + sizeof(val), m_memBuffer.GetAllocatedSize());
			__debugbreak();
			return;
		}

		uint8_t valSize = sizeof(val);
		memcpy(m_memBuffer.GetData() + m_Offset, &val, valSize);
		m_Offset += valSize;
	}

	template<typename T>
	T Read()
	{
		T outVar = T();

		// safety
		if (m_Offset >= m_memBuffer.GetAllocatedSize())
		{
			NetworkLog(ELogVerbosity::LOG_DEBUG, "[NGMP] Trying to read starting from %d, but buffer is only %d", m_Offset, m_memBuffer.GetAllocatedSize());
			__debugbreak();
			return outVar;
		}

		uint8_t valSize = sizeof(T);
		memcpy(&outVar, m_memBuffer.GetData() + m_Offset, valSize);
		m_Offset += valSize;

		return outVar;
	}

	void WriteString(const char* szStr)
	{
		// TODO: Only supports strings up to 255 len, probably okay
		uint8_t length = (uint8_t)strlen(szStr) + 1;
		uint8_t valSize = sizeof(char) * length;

		// bounds check
		if (m_Offset + valSize > m_memBuffer.GetAllocatedSize())
		{
			NetworkLog(ELogVerbosity::LOG_DEBUG, "[NGMP] Size after writing will be %d, but buffer is only %d", m_Offset + valSize, m_memBuffer.GetAllocatedSize());
			__debugbreak();
			return;
		}

		// write length first
		Write(length);

		// write string
		memcpy(m_memBuffer.GetData() + m_Offset, szStr, valSize);
		m_Offset += valSize;
	}

	const char* ReadString()
	{
		// read length
		uint8_t length = Read<uint8_t>();

		// now read the string
		uint8_t valSize = sizeof(char) * length;
		const char* szStr = (const char*)(m_memBuffer.GetData() + m_Offset);
		m_Offset += valSize;
		return szStr;
	}

	// vector container (NOTE: Only supports up to 255 elements
	template<typename T>
	void WritePrimitiveVectorContainer(std::vector<T> vec)
	{
		// write length
		Write<uint8_t>(vec.size());

		// write each element
		for (T elem : vec)
		{
			Write<T>(elem);
		}
	}

	template<typename T>
	std::vector<T> ReadPrimitiveVectorContainer()
	{
		std::vector<T> vecOut;

		// read length
		uint8_t length = Read<uint8_t>();

		// read each element
		for (int i = 0; i < length; ++i)
		{
			T primitive = Read<T>();
			vecOut.push_back(primitive);
		}

		return vecOut;
	}

	EPacketID GetPacketID() const
	{
		assert(m_packetID != EPacketID::PACKET_ID_NONE);
		return m_packetID;
	}

	uint8_t* GetRawBuffer()
	{
		return m_memBuffer.GetData();
	}

	MemoryBuffer& GetMemoryBuffer()
	{
		return m_memBuffer;
	}

	int64_t GetNumBytesAllocated() const { return m_memBuffer.GetAllocatedSize(); }
	int64_t GetNumBytesUsed() const { return m_Offset; }

	void ResetOffsetForLocalRead() { m_Offset = 0; }

private:
	MemoryBuffer m_memBuffer;
	int64_t m_Offset = 0;

	EPacketID m_packetID = EPacketID::PACKET_ID_NONE;
};