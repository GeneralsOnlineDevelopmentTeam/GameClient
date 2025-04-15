#pragma once

enum ENetworkConnectionState
{
	NOT_CONNECTED,
	CONNECTED_DIRECT,
	CONNECTED_RELAYED
};

enum ENetworkMeshType : uint8_t
{
	NETWORK_ROOM = 0,
	GAME_LOBBY = 1,
	GAME_TRANSPORT = 2
};

class NetworkMemberBase
{
public:
	int64_t user_id = -1;
	std::string display_name;

	ENetworkConnectionState m_connectionState = ENetworkConnectionState::NOT_CONNECTED;
	bool m_bIsHost = false;

	bool m_bIsReady = false;
};