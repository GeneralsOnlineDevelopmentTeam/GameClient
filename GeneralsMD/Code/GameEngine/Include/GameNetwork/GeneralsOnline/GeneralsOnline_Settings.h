#pragma once
#include "libcurl/curl.h"

#include <string>

enum EHTTPVersion
{
    HTTP_VERSION_AUTO,
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_2_0,
    HTTP_VERSION_3_0
};

class GenOnlineSettings
{
public:
	GenOnlineSettings();

	static constexpr const char* DEFAULT_ANTICHEAT_PLUGIN = "easyanticheat";

	float Camera_MoveSpeedRatio() const { return m_Camera_MoveSpeedRatio; }
	float Camera_GetMinHeight() const { return m_Camera_MinHeight; }
	float Camera_GetMaxHeight_WhenLobbyHost() const { return m_Camera_MaxHeight_LobbyHost; }

	float DetermineCameraMaxHeight();

	void Graphics_SetFPS(int fpsLimit, bool bLimitFramerate)
	{
		m_Render_FramerateLimit_FPSVal = fpsLimit;
		m_Render_LimitFramerate = bLimitFramerate;
		Save();
	}

	void Save_Camera_MaxHeight_WhenLobbyHost(float maxHeight)
	{
		if (maxHeight >= GENERALS_ONLINE_MIN_LOBBY_CAMERA_ZOOM || maxHeight <= GENERALS_ONLINE_MAX_LOBBY_CAMERA_ZOOM)
		{
			m_Camera_MaxHeight_LobbyHost = maxHeight;
			Save();
		}
	}

	bool Graphics_DrawStatsOverlay() const { return m_Render_DrawStatsOverlay; }
	bool Graphics_LimitFramerate() const { return m_Render_LimitFramerate; }
	int Graphics_GetFPSLimit() const
	{
		if (!m_Render_LimitFramerate)
		{
			return 1000000;
		}

		return m_Render_FramerateLimit_FPSVal;
	}

	void EnsureInitialized()
	{
		if (!m_bInitialized)
		{
			Initialize();
		}
	}

	bool IsInitialized() const
	{
		return m_bInitialized;
	}

	bool WasAnticheatPluginDefaulted() const
	{
		return m_bAnticheatPluginDefaulted;
	}

    std::string GetAnticheatPlugin() const
	{
		if (m_Plugins_Anticheat.empty())
		{
			return DEFAULT_ANTICHEAT_PLUGIN;
		}

		return m_Plugins_Anticheat;
	}

	bool Social_Notifications_FriendComesOnline_Menus() { return m_Social_Notification_FriendComesOnline_Menus; }
	bool Social_Notifications_FriendComesOnline_Gameplay() { return m_Social_Notification_FriendComesOnline_Gameplay; }
	bool Social_Notifications_FriendGoesOffline_Menus() { return m_Social_Notification_FriendGoesOffline_Menus; }
	bool Social_Notifications_FriendGoesOffline_Gameplay() { return m_Social_Notification_FriendGoesOffline_Gameplay; }
	bool Social_Notifications_PlayerAcceptsRequest_Menus() { return m_Social_Notification_PlayerAcceptsRequest_Menus; }
	bool Social_Notifications_PlayerAcceptsRequest_Gameplay() { return m_Social_Notification_PlayerAcceptsRequest_Gameplay; }
    bool Social_Notifications_PlayerSendsRequest_Menus() { return m_Social_Notification_PlayerSendsRequest_Menus; }
    bool Social_Notifications_PlayerSendsRequest_Gameplay() { return m_Social_Notification_PlayerSendsRequest_Gameplay; }


	bool Debug_VerboseLogging() const { return m_bVerbose; }

	int GetChatLifeSeconds() const { return std::max<int>(m_Chat_LifeSeconds, 10); }

	void Initialize()
	{
		m_bInitialized = true;
		Load();
	}

	bool Network_UseAlternativeEndpoint() const { return m_Network_UseAlternativeEndpoint; }
	EHTTPVersion Network_GetHTTPVersion() const { return m_Network_HTTPVersion; }
	int Network_GetHTTPVersionForCurl() const
	{
		switch (m_Network_HTTPVersion)
		{
			case HTTP_VERSION_AUTO:
			{
				return CURL_HTTP_VERSION_NONE;
			}

			case HTTP_VERSION_1_0:
			{
				return CURL_HTTP_VERSION_1_0;
            }

			case HTTP_VERSION_1_1:
			{
				return CURL_HTTP_VERSION_1_1;
			}

			case HTTP_VERSION_2_0:
			{
				return CURL_HTTP_VERSION_2_0;
			}

			case HTTP_VERSION_3_0:
			{
				return CURL_HTTP_VERSION_3;
			}
		}

		return CURL_HTTP_VERSION_NONE;
	}

private:
	void Load(void);
	void Save();

private:
	// NOTE: This also works as the default creation (since we just call Save)
	const float m_Camera_MinHeight_default = 100.f;
	float m_Camera_MinHeight = m_Camera_MinHeight_default;

	const float m_Camera_MoveSpeedRatio_default = 1.f;
	float m_Camera_MoveSpeedRatio = m_Camera_MoveSpeedRatio_default;

	float m_Camera_MaxHeight_LobbyHost = GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM;

	bool m_bInitialized = false;

	bool m_bVerbose = false;
	bool m_bAnticheatPluginDefaulted = false;

	bool m_Render_DrawStatsOverlay = true;
	bool m_Render_LimitFramerate = true;
	int m_Render_FramerateLimit_FPSVal = 60;
	int m_Chat_LifeSeconds = 30;

	bool m_Social_Notification_FriendComesOnline_Menus = true;
	bool m_Social_Notification_FriendComesOnline_Gameplay = true;
	bool m_Social_Notification_FriendGoesOffline_Menus = true;
	bool m_Social_Notification_FriendGoesOffline_Gameplay = true;
	bool m_Social_Notification_PlayerAcceptsRequest_Menus = true;
	bool m_Social_Notification_PlayerAcceptsRequest_Gameplay = true;
	bool m_Social_Notification_PlayerSendsRequest_Menus = true;
	bool m_Social_Notification_PlayerSendsRequest_Gameplay = true;

	std::string m_Plugins_Anticheat = DEFAULT_ANTICHEAT_PLUGIN;

	EHTTPVersion m_Network_HTTPVersion = EHTTPVersion::HTTP_VERSION_AUTO;
	bool m_Network_UseAlternativeEndpoint = false;
};
