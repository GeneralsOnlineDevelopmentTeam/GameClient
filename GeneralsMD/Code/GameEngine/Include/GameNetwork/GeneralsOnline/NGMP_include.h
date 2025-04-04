#pragma once


void NetworkLog(const char* fmt, ...);

// common game engine includes
#include "../../Common/UnicodeString.h"
#include "../../Common/AsciiString.h"

// standard libs
#include <fstream>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>

#include "GameNetwork/GeneralsOnline/NextGenMP_defines.h"
#include "GameNetwork/GeneralsOnline/NetworkPacket.h"
#include "GameNetwork/GeneralsOnline/NetworkBitstream.h"
#include "GameNetwork/GeneralsOnline/NGMP_types.h"
#include "GameNetwork/GeneralsOnline/NGMPGame.h"

#include "Packets/NextGenTransport.h"

#include "GameNetwork/GeneralsOnline/Packets/NetworkPacket_Lobby_StartGame.h"

#if defined(GENERALS_ONLINE_BRANCH_JMARSHALL)
#include "../Console/Console.h"
#endif