#include "PreRTS.h" // This must go first in EVERY cpp file int the GameEngine

#include "Common/CRC.h"
#include "GameNetwork/NetworkInterface.h"
#include "GameNetwork/GeneralsOnline/NextGenTransport.h"

#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"
#include "ValveNetworkingSockets/steam/steamnetworkingtypes.h"
#include "GameNetwork/GeneralsOnline/PluginInterfaces.h"

#ifdef _INTERNAL
// for occasional debugging...
//#pragma optimize("", off)
//#pragma MESSAGE("************************************** WARNING, optimization disabled for debugging purposes")
#endif

NextGenTransport::NextGenTransport()
{
    // Initialize statistics tracking
    m_statisticsSlot = 0;
    m_lastSecond = timeGetTime();
    m_useLatency = FALSE;
    m_usePacketLoss = FALSE;
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
    
    // Clear retry state for all outgoing packets
    for (int i = 0; i < MAX_MESSAGES; ++i)
    {
        m_outPacketState[i].retryCount = 0;
        m_inBufferOccupied[i] = false;  // Mark all incoming slots as empty
    }
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

    // Statistics gathering - advance slot every second (same as UDPTransport)
    UnsignedInt now = timeGetTime();
    if (m_lastSecond + 1000 < now)
    {
        m_lastSecond = now;
        m_statisticsSlot = (m_statisticsSlot + 1) % MAX_TRANSPORT_STATISTICS_SECONDS;
        m_outgoingPackets[m_statisticsSlot] = 0;
        m_outgoingBytes[m_statisticsSlot] = 0;
        m_incomingPackets[m_statisticsSlot] = 0;
        m_incomingBytes[m_statisticsSlot] = 0;
        m_unknownPackets[m_statisticsSlot] = 0;
        m_unknownBytes[m_statisticsSlot] = 0;
    }

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

    if (AnticheatPlugInterface::DoesACPluginProvideSecureGameTransport())
    {
        // TODO_EOS: Just have a "has" function instead
        while (AnticheatPlugInterface::GetNextRecvPacketSize(static_cast<uint8_t>(ENetworkChannels::Game)) > 0)
        {
            int64_t userID = -1; // TODO_EOS
            uint32_t numBytes = AnticheatPlugInterface::GetNextRecvPacketSize(static_cast<uint8_t>(ENetworkChannels::Game));

            std::vector<uint8_t> vecPacketData;
            vecPacketData.resize(numBytes);

            uint8_t* pPacketData = vecPacketData.data();
            bool bSuccess = AnticheatPlugInterface::RecvPacket(&pPacketData, static_cast<uint8_t>(ENetworkChannels::Game));
            if (bSuccess && pPacketData != nullptr)
            {
                // TODO_EOS: Impl
                bool bIsACPacket = false;

                if (bIsACPacket)
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Received AC message of size %u from user %lld", numBytes, static_cast<long long>(userID));
                    AnticheatPlugInterface::AC_NetworkMessageArrived(userID, pPacketData, numBytes - 3);
                }
                else
                {
                    NetworkLog(ELogVerbosity::LOG_DEBUG,
                        "[GAME PACKET] Received message of size %u from user %lld",
                        numBytes, static_cast<long long>(userID));

                    // Must at least contain the header
                    if (numBytes < sizeof(TransportMessageHeader))
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE,
                            "Game Packet Recv: Dropping packet smaller than header (%u < %zu)",
                            numBytes, sizeof(TransportMessageHeader));
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
                        continue;
                    }

                    // Clear incomingMessage, then copy header + payload region only
                    std::memset(&incomingMessage, 0, sizeof(incomingMessage));

                    // Copy header safely
                    std::memcpy(&incomingMessage.header,
                        pPacketData,
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
                        continue;
                    }

                    // Copy payload into data[]
                    if (payloadLen > 0)
                    {
                        std::memcpy(incomingMessage.data,
                            static_cast<unsigned char*>(pPacketData) + sizeof(TransportMessageHeader),
                            payloadLen);
                    }

                    // Length is bounded by sizeof(data), so cast is safe
                    incomingMessage.length = static_cast<Int>(payloadLen);

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
                    if (m_usePacketLoss)
                    {
                        // Drop packet if random value is below loss percentage
                        // E.g., if m_packetLoss = 50, drop ~50% of packets
                        if (TheGlobalData->m_packetLoss > GameClientRandomValue(0, 100))
                        {
                            // Simulated packet loss
                            NetworkLog(ELogVerbosity::LOG_DEBUG,
                                "Game Packet Recv: Simulated packet loss (loss%%=%d)",
                                TheGlobalData->m_packetLoss);
                            continue;
                        }
                    }
#endif

                    const bool isGenerals = isGeneralsPacket(&incomingMessage);

                    if (!isGenerals)
                    {
                        // Check if it's a CRC failure or magic number failure to help diagnose corruption
                        if (incomingMessage.header.magic != GENERALS_MAGIC_NUMBER)
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE,
                                "Game Packet Recv: BAD MAGIC NUMBER - Expected 0x%04X, got 0x%04X from user %lld. "
                                "Packet is corrupted or from wrong game version.",
                                GENERALS_MAGIC_NUMBER, incomingMessage.header.magic,
                                static_cast<long long>(userID));
                        }
                        else
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE,
                                "Game Packet Recv: CRC MISMATCH - Expected 0x%08X, got 0x%08X from user %lld. "
                                "Packet is corrupted during transmission or has invalid payload length (%u).",
                                incomingMessage.header.crc, 0, // We'd need to compute the CRC to compare
                                static_cast<long long>(userID), incomingMessage.length);
                        }
                        m_unknownPackets[m_statisticsSlot]++;
                        m_unknownBytes[m_statisticsSlot] += numBytes;
                        continue;
                    }

                    m_incomingPackets[m_statisticsSlot]++;
                    m_incomingBytes[m_statisticsSlot] += numBytes;

                    // Store into first free slot in m_inBuffer
                    bool stored = false;
                    int fullCount = 0;
                    for (int i = 0; i < MAX_MESSAGES; ++i)
                    {
                        // Check if slot is occupied using flag, not length
                        // (length could be 0 for legitimate empty packets)
                        // However, if the packet has been consumed (length cleared to 0 by outside code),
                        // clear the occupied flag too
                        if (m_inBuffer[i].length == 0 && m_inBufferOccupied[i])
                        {
                            m_inBufferOccupied[i] = false;
                        }

                        if (m_inBufferOccupied[i])
                        {
                            fullCount++;
                            continue;
                        }

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
                                    "Game Packet Recv: WARNING - Truncating payload from %u to %zu bytes for inBuffer[%d] from user %lld. "
                                    "This indicates the incoming packet exceeds the buffer capacity and data will be lost. "
                                    "Consider increasing MAX_MESSAGE_LEN or MAX_PACKET_SIZE.",
                                    payloadLen, dstCap, i, static_cast<long long>(userID));
                            }

                            std::memcpy(m_inBuffer[i].data,
                                incomingMessage.data,
                                toCopy);

                            m_inBuffer[i].length = static_cast<Int>(toCopy);
                        }
                        else
                        {
                            // Zero-length packet - store with length=0 but mark as occupied
                            m_inBuffer[i].length = 0;
                        }

                        // Mark slot as occupied
                        m_inBufferOccupied[i] = true;
                        stored = true;
                        break;
                    }

                    if (!stored)
                    {
                        // Buffer is full - log this as it indicates potential packet loss
                        NetworkLog(ELogVerbosity::LOG_RELEASE,
                            "Game Packet Recv: ERROR - m_inBuffer is FULL (%d/%d slots occupied), dropping packet from user %lld. "
                            "Incoming packets will be lost until buffer slots are freed. "
                            "Consider increasing MAX_MESSAGES (%d) to handle higher packet rates.",
                            fullCount, MAX_MESSAGES, static_cast<long long>(userID), MAX_MESSAGES);
                    }
                    else
                    {
                        ++numRead;
                        bRet = TRUE;
                    }
                }
            }
        }
    }
    else
    {
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

                const uint32_t numBytesWithHeader = msg->m_cbSize;

                // is it an AC packet?
                std::vector<byte> vecDataWithHeader;
                vecDataWithHeader.resize(numBytesWithHeader);
                memcpy(vecDataWithHeader.data(), msg->GetData(), numBytesWithHeader);

                // all packets must have at least 1 byte to indicate channel
                if (numBytesWithHeader >= sizeof(ENetworkChannel))
                {
                    ENetworkChannel netChannel = (ENetworkChannel)vecDataWithHeader[0];

                    std::vector<byte> vecPacketDataWithoutHeader;
                    vecPacketDataWithoutHeader.resize(numBytesWithHeader - sizeof(ENetworkChannel));
                    memcpy(vecPacketDataWithoutHeader.data(), (char*)msg->GetData() + sizeof(ENetworkChannel), numBytesWithHeader - sizeof(ENetworkChannel));

                    if (netChannel == ENetworkChannel::NETWORK_CHANNEL_AC)
                    {
                        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC PACKET] Received AC message of size %u from user %lld", vecPacketDataWithoutHeader.size(), static_cast<long long>(kvPair.second.m_userID));


                        // remove header
                        // TODO_AC: Optimize this


                        AnticheatPlugInterface::AC_NetworkMessageArrived(kvPair.second.m_userID, vecPacketDataWithoutHeader.data(), vecPacketDataWithoutHeader.size());
                        msg->Release();
                        continue;
                    }
                    else if (netChannel == ENetworkChannel::NETWORK_CHANNEL_GAME)
                    {
                        NetworkLog(ELogVerbosity::LOG_DEBUG,
                            "[GAME PACKET] Received message of size %u from user %lld",
                            vecPacketDataWithoutHeader.size(), static_cast<long long>(kvPair.second.m_userID));

                        // Must at least contain the header
                        if (vecPacketDataWithoutHeader.size() < sizeof(TransportMessageHeader))
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE,
                                "Game Packet Recv: Dropping packet smaller than header (%u < %zu)",
                                vecPacketDataWithoutHeader.size(), sizeof(TransportMessageHeader));
                            msg->Release();
                            continue;
                        }

                        // Max bytes we ever expect from the wire:
                        // header + payload (no trailing length/addr/port)
                        const uint32_t maxWireSize =
                            static_cast<uint32_t>(sizeof(TransportMessageHeader) + MAX_MESSAGE_LEN);

                        if (vecPacketDataWithoutHeader.size() > maxWireSize)
                        {
                            NetworkLog(ELogVerbosity::LOG_RELEASE,
                                "Game Packet Recv: Dropping packet too large (%u > %u)",
                                vecPacketDataWithoutHeader.size(), maxWireSize);
                            msg->Release();
                            continue;
                        }

                        // Clear incomingMessage, then copy header + payload region only
                        std::memset(&incomingMessage, 0, sizeof(incomingMessage));

                        // Copy header safely
                        std::memcpy(&incomingMessage.header,
                            vecPacketDataWithoutHeader.data(),
                            sizeof(TransportMessageHeader));

                        // Compute payload length
                        const uint32_t payloadLen =
                            vecPacketDataWithoutHeader.size() - static_cast<uint32_t>(sizeof(TransportMessageHeader));

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
                                static_cast<unsigned char*>(vecPacketDataWithoutHeader.data()) + sizeof(TransportMessageHeader),
                                payloadLen);
                        }

                        // Length is bounded by sizeof(data), so cast is safe
                        incomingMessage.length = static_cast<Int>(payloadLen);

                        msg->Release();

#if defined(RTS_DEBUG) || defined(RTS_INTERNAL)
                        if (m_usePacketLoss)
                        {
                            // Drop packet if random value is below loss percentage
                            // E.g., if m_packetLoss = 50, drop ~50% of packets
                            if (TheGlobalData->m_packetLoss > GameClientRandomValue(0, 100))
                            {
                                // Simulated packet loss
                                NetworkLog(ELogVerbosity::LOG_DEBUG,
                                    "Game Packet Recv: Simulated packet loss (loss%%=%d)",
                                    TheGlobalData->m_packetLoss);
                                continue;
                            }
                        }
#endif

                        const bool isGenerals = isGeneralsPacket(&incomingMessage);

                        if (!isGenerals)
                        {
                            // Check if it's a CRC failure or magic number failure to help diagnose corruption
                            if (incomingMessage.header.magic != GENERALS_MAGIC_NUMBER)
                            {
                                NetworkLog(ELogVerbosity::LOG_RELEASE,
                                    "Game Packet Recv: BAD MAGIC NUMBER - Expected 0x%04X, got 0x%04X from user %lld. "
                                    "Packet is corrupted or from wrong game version.",
                                    GENERALS_MAGIC_NUMBER, incomingMessage.header.magic,
                                    static_cast<long long>(kvPair.second.m_userID));
                            }
                            else
                            {
                                NetworkLog(ELogVerbosity::LOG_RELEASE,
                                    "Game Packet Recv: CRC MISMATCH - Expected 0x%08X, got 0x%08X from user %lld. "
                                    "Packet is corrupted during transmission or has invalid payload length (%u).",
                                    incomingMessage.header.crc, 0, // We'd need to compute the CRC to compare
                                    static_cast<long long>(kvPair.second.m_userID), incomingMessage.length);
                            }
                            m_unknownPackets[m_statisticsSlot]++;
                            m_unknownBytes[m_statisticsSlot] += vecPacketDataWithoutHeader.size();
                            continue;
                        }

                        m_incomingPackets[m_statisticsSlot]++;
                        m_incomingBytes[m_statisticsSlot] += vecPacketDataWithoutHeader.size();

                        // Store into first free slot in m_inBuffer
                        bool stored = false;
                        int fullCount = 0;
                        for (int i = 0; i < MAX_MESSAGES; ++i)
                        {
                            // Check if slot is occupied using flag, not length
                            // (length could be 0 for legitimate empty packets)
                            // However, if the packet has been consumed (length cleared to 0 by outside code),
                            // clear the occupied flag too
                            if (m_inBuffer[i].length == 0 && m_inBufferOccupied[i])
                            {
                                m_inBufferOccupied[i] = false;
                            }

                            if (m_inBufferOccupied[i])
                            {
                                fullCount++;
                                continue;
                            }

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
                                        "Game Packet Recv: WARNING - Truncating payload from %u to %zu bytes for inBuffer[%d] from user %lld. "
                                        "This indicates the incoming packet exceeds the buffer capacity and data will be lost. "
                                        "Consider increasing MAX_MESSAGE_LEN or MAX_PACKET_SIZE.",
                                        payloadLen, dstCap, i, static_cast<long long>(kvPair.second.m_userID));
                                }

                                std::memcpy(m_inBuffer[i].data,
                                    incomingMessage.data,
                                    toCopy);

                                m_inBuffer[i].length = static_cast<Int>(toCopy);
                            }
                            else
                            {
                                // Zero-length packet - store with length=0 but mark as occupied
                                m_inBuffer[i].length = 0;
                            }

                            // Mark slot as occupied
                            m_inBufferOccupied[i] = true;
                            stored = true;
                            break;
                        }

                        if (!stored)
                        {
                            // Buffer is full - log this as it indicates potential packet loss
                            NetworkLog(ELogVerbosity::LOG_RELEASE,
                                "Game Packet Recv: ERROR - m_inBuffer is FULL (%d/%d slots occupied), dropping packet from user %lld. "
                                "Incoming packets will be lost until buffer slots are freed. "
                                "Consider increasing MAX_MESSAGES (%d) to handle higher packet rates.",
                                fullCount, MAX_MESSAGES, static_cast<long long>(kvPair.second.m_userID), MAX_MESSAGES);
                        }
                        else
                        {
                            ++numRead;
                            bRet = TRUE;
                        }
                    }
                }
                else if (vecDataWithHeader.size() != -1)
                {
                    // Malformed packet - too small for header
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[NET PACKET] Dropping malformed packet - size %u is less than header size 1 from user %lld", vecDataWithHeader.size(), static_cast<long long>(kvPair.second.m_userID));
                    msg->Release();
                    continue;
                }
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
        {
            m_outPacketState[i].retryCount = 0;  // Reset retry counter when packet slot is cleared
            continue;
        }

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
                m_outPacketState[i].retryCount = 0;
                retval = FALSE;
                continue;
            }

            NetworkMesh* pMesh = NGMP_OnlineServicesManager::GetNetworkMesh();
            if (pMesh == nullptr)
            {
                NetworkLog(ELogVerbosity::LOG_RELEASE,
                    "Game Packet Send: No network mesh");
                // Don't clear the packet - retry next frame
                retval = FALSE;
                continue;
            }

            // CRITICAL FIX: Create a temporary buffer with ONLY header + data
            // Do NOT send the entire TransportMessage struct which contains metadata
            // (length, addr, port fields that corrupt the packet on the wire)
            std::vector<byte> packetData;
            packetData.resize(totalLen);
            
            // Copy header
            std::memcpy(packetData.data(), 
                       &m_outBuffer[i].header, 
                       sizeof(TransportMessageHeader));
            
            // Copy payload data
            std::memcpy(packetData.data() + sizeof(TransportMessageHeader),
                       m_outBuffer[i].data,
                       m_outBuffer[i].length);

            int sendResult =
                pMesh->SendGamePacket(
                    packetData.data(),  // Send only header + data, NOT entire struct
                    totalLen,
                    pSlot->m_userID);

            if (sendResult >= 0)
            {
                // Send successful
                ++numSent;
                m_outgoingPackets[m_statisticsSlot]++;
                m_outgoingBytes[m_statisticsSlot] +=
                    m_outBuffer[i].length + sizeof(TransportMessageHeader);
                m_outBuffer[i].length = 0; // Remove from queue
                m_outPacketState[i].retryCount = 0;
                retval = TRUE;
            }
            else
            {
                // Send failed - implement retry logic for transient errors
                m_outPacketState[i].retryCount++;
                
                if (m_outPacketState[i].retryCount < OutgoingPacketState::MAX_RETRIES)
                {
                    NetworkLog(ELogVerbosity::LOG_RELEASE,
                        "Game Packet Send: SendGamePacket failed (err=%d), retry %d/%d for packet to user %lld",
                        sendResult, m_outPacketState[i].retryCount, 
                        OutgoingPacketState::MAX_RETRIES, pSlot->m_userID);
                    // Keep packet in queue for retry
                    retval = FALSE;
                }
                else
                {
                    // Max retries exceeded - drop packet
                    NetworkLog(ELogVerbosity::LOG_RELEASE,
                        "Game Packet Send: Dropping packet after %d failed retries to user %lld",
                        m_outPacketState[i].retryCount, pSlot->m_userID);
                    m_outBuffer[i].length = 0;
                    m_outPacketState[i].retryCount = 0;
                    retval = FALSE;
                }
            }
        }
        else
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE,
                "Game Packet Send: No slot for addr %u, dropping packet", m_outBuffer[i].addr);
            m_outBuffer[i].length = 0;
            m_outPacketState[i].retryCount = 0;
            retval = FALSE;
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
