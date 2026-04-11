#include "PreRTS.h" // This must go first in EVERY cpp file int the GameEngine

#include "Common/CRC.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/NextGenTransport.h"

#include "GameNetwork/GeneralsOnline/ngmp_include.h"
#include "GameNetwork/GeneralsOnline/ngmp_interfaces.h"

// GENERALS ONLINE - Voice Chat
#include "GameNetwork/GeneralsOnline/Voice/NGMPVoiceBridge.h"

#ifdef _INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

NextGenTransport::NextGenTransport()
{
}

NextGenTransport::~NextGenTransport()
{
    reset();
}

Bool NextGenTransport::init(AsciiString ip, UnsignedShort port)
{
    return TRUE;
}

Bool NextGenTransport::init(UnsignedInt ip, UnsignedShort port)
{
    return TRUE;
}

void NextGenTransport::reset(void)
{
    // Clear buffers and statistics to avoid stale state.
    std::memset(m_inBuffer, 0, sizeof(m_inBuffer));
    std::memset(m_outBuffer, 0, sizeof(m_outBuffer));
    std::memset(m_incomingPackets, 0, sizeof(m_incomingPackets));
    std::memset(m_incomingBytes, 0, sizeof(m_incomingBytes));
    std::memset(m_outgoingPackets, 0, sizeof(m_outgoingPackets));
    std::memset(m_outgoingBytes, 0, sizeof(m_outgoingBytes));
    std::memset(m_unknownPackets, 0, sizeof(m_unknownPackets));
    std::memset(m_unknownBytes, 0, sizeof(m_unknownBytes));
}

Bool NextGenTransport::update(void)
{
    Bool retval = TRUE;

    if (doRecv() == FALSE)
    {
        retval = FALSE;
    }
    if (doSend() == FALSE)
    {
        retval = FALSE;
    }

    return retval;
}

Bool NextGenTransport::doRecv(void)
{
    bool bRet = FALSE;
    int numRead = 0;

    TransportMessage incomingMessage{};
    std::memset(&incomingMessage, 0, sizeof(incomingMessage));

    auto* pLobbyInterface =
        NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
    if (!pLobbyInterface)
    {
        NetworkLog(ELogVerbosity::LOG_DEBUG, "Game Packet Recv: No lobby interface");
        return FALSE;
    }

    NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
    if (!pMesh)
    {
        NetworkLog(ELogVerbosity::LOG_DEBUG, "Game Packet Recv: No network mesh");
        return FALSE;
    }

    std::map<int64_t, PlayerConnection>& connections = pMesh->GetAllConnections();
    for (auto& kvPair : connections)
    {
        SteamNetworkingMessage_t* pMsg[255] = { nullptr };
        int numPackets = kvPair.second.Recv(pMsg);

        if (numPackets <= 0)
            continue;

        if (numPackets > static_cast<int>(std::size(pMsg)))
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Recv: numPackets (%d) > pMsg capacity (%zu), clamping",
                numPackets, std::size(pMsg));
            numPackets = static_cast<int>(std::size(pMsg));
        }

        for (int iPacket = 0; iPacket < numPackets; ++iPacket)
        {
            SteamNetworkingMessage_t* msg = pMsg[iPacket];
            if (!msg)
                continue;

            const uint32_t numBytes = msg->m_cbSize;

            NetworkLog(ELogVerbosity::LOG_DEBUG,
                "[GAME PACKET] Received message of size %u from user %lld",
                numBytes, static_cast<long long>(kvPair.second.m_userID));

            // Must at least contain the header
            if (numBytes < sizeof(TransportMessageHeader))
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Recv: Dropping packet smaller than header (%u < %zu)",
                    numBytes, sizeof(TransportMessageHeader));
                msg->Release();
                continue;
            }

            // Max bytes we ever expect from the wire:
            // header + payload (no trailing length/addr/port)
            const uint32_t maxWireSize =
                static_cast<uint32_t>(sizeof(TransportMessageHeader) + MAX_MESSAGE_LEN);

            if (numBytes > maxWireSize)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Recv: Dropping packet too large (%u > %u)",
                    numBytes, maxWireSize);
                msg->Release();
                continue;
            }

            // Clear incomingMessage, then copy header + payload region only
            std::memset(&incomingMessage, 0, sizeof(incomingMessage));

            // Copy header safely
            std::memcpy(&incomingMessage.header,
                msg->m_pData,
                sizeof(TransportMessageHeader));

            // Compute payload length
            const uint32_t payloadLen =
                numBytes - static_cast<uint32_t>(sizeof(TransportMessageHeader));

            // Sanity check payloadLen against local buffer size
            if (payloadLen > sizeof(incomingMessage.data))
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Recv: Dropping packet, payloadLen (%u) > incoming buffer (%zu)",
                    payloadLen, sizeof(incomingMessage.data));
                msg->Release();
                continue;
            }

            // Copy payload into data[]
            if (payloadLen > 0)
            {
                std::memcpy(incomingMessage.data,
                    static_cast<unsigned char*>(msg->m_pData) + sizeof(TransportMessageHeader),
                    payloadLen);
            }

            // Length is bounded by sizeof(data), so cast is safe
            incomingMessage.length = static_cast<Int>(payloadLen);

            msg->Release();

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
            if (m_usePacketLoss)
            {
                if (TheGlobalData->m_packetLoss >= GameClientRandomValue(0, 100))
                {
                    // Simulated packet loss
                    continue;
                }
            }
#endif

            // GENERALS ONLINE - Voice Chat
            // Voice packets reuse the same 6-byte TransportMessageHeader
            // prefix but stamp a different magic number so we can
            // dispatch them without going through the generals CRC path.
            if (incomingMessage.header.magic == VOICE_MAGIC_NUMBER)
            {
                if (payloadLen > 0)
                {
                    NGMPVoiceBridge::DispatchIncoming(
                        kvPair.second.m_userID,
                        incomingMessage.data,
                        static_cast<int>(payloadLen));
                }
                m_incomingPackets[m_statisticsSlot]++;
                m_incomingBytes[m_statisticsSlot] += numBytes;
                continue;
            }

            const bool isGenerals = isGeneralsPacket(&incomingMessage);

            if (!isGenerals)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Recv: Is NOT a generals packet");
                m_unknownPackets[m_statisticsSlot]++;
                m_unknownBytes[m_statisticsSlot] += numBytes;
                continue;
            }

            m_incomingPackets[m_statisticsSlot]++;
            m_incomingBytes[m_statisticsSlot] += numBytes;

            // Store into first free slot in m_inBuffer
            bool stored = false;
            for (int i = 0; i < MAX_MESSAGES; ++i)
            {
                if (m_inBuffer[i].length != 0)
                    continue;

                // Clear slot
                std::memset(&m_inBuffer[i], 0, sizeof(m_inBuffer[i]));

                // Copy header
                m_inBuffer[i].header = incomingMessage.header;

                // Copy payload with bounds check
                if (payloadLen > 0)
                {
                    const size_t dstCap = sizeof(m_inBuffer[i].data);
                    const size_t toCopy = (payloadLen <= dstCap) ? payloadLen : dstCap;

                    if (payloadLen > dstCap)
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE,
                            "Game Packet Recv: Truncating payload from %u to %zu bytes for inBuffer[%d]",
                            payloadLen, dstCap, i);
                    }

                    std::memcpy(m_inBuffer[i].data,
                        incomingMessage.data,
                        toCopy);

                    m_inBuffer[i].length = static_cast<Int>(toCopy);
                }
                else
                {
                    m_inBuffer[i].length = 0;
                }

                stored = true;
                break;
            }

            if (!stored)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Recv: m_inBuffer full, dropping packet");
            }
            else
            {
                ++numRead;
                bRet = TRUE;
            }
        }
    }

    NetworkLog(ELogVerbosity::LOG_DEBUG,
        "Game Packet Recv: Read %d packets this frame", numRead);

    return bRet;
}

Bool NextGenTransport::doSend(void)
{
    Bool retval = TRUE;
    int numSent = 0;

    for (int i = 0; i < MAX_MESSAGES; ++i)
    {
        if (m_outBuffer[i].length == 0)
            continue;

        NGMP_OnlineServicesManager* pOnlineServicesManager = NGMP_OnlineServicesManager::GetInstance();
        if (pOnlineServicesManager == nullptr)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Send: No OnlineServicesManager");
            return FALSE;
        }

        NGMP_OnlineServices_LobbyInterface* pLobbyInterface =
            NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();
        if (pLobbyInterface == nullptr)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Send: No LobbyInterface");
            return FALSE;
        }

        if (TheNGMPGame == nullptr)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Send: TheNGMPGame is null");
            return FALSE;
        }

        NGMPGameSlot* pSlot =
            static_cast<NGMPGameSlot*>(TheNGMPGame->getSlot(m_outBuffer[i].addr));

        if (pSlot != nullptr)
        {
            const uint32_t totalLen =
                static_cast<uint32_t>(m_outBuffer[i].length) + sizeof(TransportMessageHeader);

            // Sanity check against some reasonable upper bound
            if (totalLen > (sizeof(TransportMessageHeader) + MAX_PACKET_SIZE))
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Send: totalLen (%u) exceeds allowed max (%zu)",
                    totalLen,
                    sizeof(TransportMessageHeader) + static_cast<size_t>(MAX_PACKET_SIZE));
                m_outBuffer[i].length = 0; // drop this entry
                retval = FALSE;
                continue;
            }

            NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
            if (pMesh == nullptr)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Send: No network mesh");
                m_outBuffer[i].length = 0;
                retval = FALSE;
                continue;
            }

            int sendResult =
                pMesh->SendGamePacket(
                    static_cast<void*>(&m_outBuffer[i]),
                    totalLen,
                    pSlot->m_userID);

            retval = (sendResult >= 0);
        }
        else
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Send: No slot for addr %u", m_outBuffer[i].addr);
            retval = FALSE;
        }

        if (retval)
        {
            ++numSent;
            m_outgoingPackets[m_statisticsSlot]++;
            m_outgoingBytes[m_statisticsSlot] +=
                m_outBuffer[i].length + sizeof(TransportMessageHeader);
            m_outBuffer[i].length = 0; // Remove from queue
        }
        else
        {
            // Keep the entry? For now, drop it to avoid infinite retry loops.
            m_outBuffer[i].length = 0;
        }
    }

    NetworkLog(ELogVerbosity::LOG_DEBUG,
        "Game Packet Send: Sent %d packets this frame", numSent);

    return retval;
}

Bool NextGenTransport::queueSend(UnsignedInt addr,
    UnsignedShort port,
    const UnsignedByte* buf,
    Int len /*, NetMessageFlags flags, Int id */)
{
    if (buf == nullptr)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE,
            "Game Packet QueueSend: null buffer");
        return FALSE;
    }

    if (len < 1 || len > MAX_PACKET_SIZE)
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE,
            "Game Packet QueueSend: invalid length %d (max %d)",
            len, MAX_PACKET_SIZE);
        return FALSE;
    }

    for (int i = 0; i < MAX_MESSAGES; ++i)
    {
        if (m_outBuffer[i].length != 0)
            continue;

        const size_t dstCap = sizeof(m_outBuffer[i].data);
        if (static_cast<size_t>(len) > dstCap)
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet QueueSend: len (%d) > outBuffer[%d].data capacity (%zu)",
                len, i, dstCap);
            return FALSE;
        }

        // Insert data here
        std::memset(&m_outBuffer[i], 0, sizeof(m_outBuffer[i]));

        m_outBuffer[i].length = len;
        std::memcpy(m_outBuffer[i].data, buf, static_cast<size_t>(len));
        m_outBuffer[i].addr = addr;
        m_outBuffer[i].port = port;

        m_outBuffer[i].header.magic = GENERALS_MAGIC_NUMBER;

        CRC crc;
        // CRC over header.magic through end of payload
        const size_t crcLen =
            static_cast<size_t>(m_outBuffer[i].length) +
            sizeof(TransportMessageHeader) - sizeof(UnsignedInt);

        if (crcLen > sizeof(m_outBuffer[i]) - offsetof(TransportMessage, header.magic))
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet QueueSend: CRC length overflow, crcLen=%zu", crcLen);
            m_outBuffer[i].length = 0;
            return FALSE;
        }

        crc.computeCRC(
            reinterpret_cast<unsigned char*>(&(m_outBuffer[i].header.magic)),
            static_cast<unsigned int>(crcLen));

        m_outBuffer[i].header.crc = crc.get();

        if (!isGeneralsPacket(&m_outBuffer[i]))
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Queue Sending: Is NOT a generals packet");
        }

        return TRUE;
    }

    NetworkLog(ELogVerbosity::LOG_RELEASE,
        "Game Packet QueueSend: m_outBuffer full, dropping packet");
    return FALSE;
}
