#pragma once

enum ENetworkConnectionState
{
	NOT_CONNECTED,
	CONNECTED_DIRECT,
	CONNECTED_RELAYED
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