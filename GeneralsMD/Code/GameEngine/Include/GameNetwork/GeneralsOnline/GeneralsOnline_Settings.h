#pragma once
#include "libcurl/curl.h"
#include <string>
#include <vector>
#include <cstdint>

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

	bool Social_Notifications_FriendComesOnline_Menus() { return m_Social_Notification_FriendComesOnline_Menus; }
	bool Social_Notifications_FriendComesOnline_Gameplay() { return m_Social_Notification_FriendComesOnline_Gameplay; }
	bool Social_Notifications_FriendGoesOffline_Menus() { return m_Social_Notification_FriendGoesOffline_Menus; }
	bool Social_Notifications_FriendGoesOffline_Gameplay() { return m_Social_Notification_FriendGoesOffline_Gameplay; }
	bool Social_Notifications_PlayerAcceptsRequest_Menus() { return m_Social_Notification_PlayerAcceptsRequest_Menus; }
	bool Social_Notifications_PlayerAcceptsRequest_Gameplay() { return m_Social_Notification_PlayerAcceptsRequest_Gameplay; }
    bool Social_Notifications_PlayerSendsRequest_Menus() { return m_Social_Notification_PlayerSendsRequest_Menus; }
    bool Social_Notifications_PlayerSendsRequest_Gameplay() { return m_Social_Notification_PlayerSendsRequest_Gameplay; }


	bool Debug_VerboseLogging() const { return m_bVerbose; }

	// -------- Lobby voice chat settings --------
	bool  Voice_GetEnabled() const { return m_Voice_Enabled; }
	const std::wstring& Voice_GetCaptureDeviceID() const { return m_Voice_CaptureDeviceID; }
	float Voice_GetMicGain() const { return m_Voice_MicGain; }
	float Voice_GetGlobalVolume() const { return m_Voice_GlobalVolume; }

	// -------- Persistent per-client voice ignore list --------
	// Client-local only. Never transmitted, never uploaded. When the local
	// user mutes a peer via /voice mute, that peer's NGMP userID gets
	// appended here and the list survives game restarts, so a troll that
	// constantly leaves and re-joins the same lobby cannot bypass the mute
	// just by reconnecting.
	//
	// Muting affects ONLY the muter's own playback: VoicePlayback drops the
	// decoded audio locally. Nobody else's client knows or cares.
	const std::vector<int64_t>& Voice_GetMutedPeers() const { return m_Voice_MutedPeers; }

	void Save_Voice_Enabled(bool enabled)
	{
		m_Voice_Enabled = enabled;
		Save();
	}
	void Save_Voice_CaptureDeviceID(const std::wstring& deviceID)
	{
		m_Voice_CaptureDeviceID = deviceID;
		Save();
	}
	void Save_Voice_MicGain(float gain)
	{
		if (gain < 0.0f) gain = 0.0f;
		if (gain > 4.0f) gain = 4.0f;
		m_Voice_MicGain = gain;
		Save();
	}
	void Save_Voice_GlobalVolume(float volume)
	{
		if (volume < 0.0f) volume = 0.0f;
		if (volume > 2.0f) volume = 2.0f;
		m_Voice_GlobalVolume = volume;
		Save();
	}

	// Replace the full persistent mute list. Caller is expected to dedupe
	// and drop invalid IDs (<=0) beforehand; we still re-validate here so
	// a corrupt caller cannot poison the file.
	void Save_Voice_MutedPeers(const std::vector<int64_t>& mutedPeers)
	{
		m_Voice_MutedPeers.clear();
		m_Voice_MutedPeers.reserve(mutedPeers.size());
		for (int64_t id : mutedPeers)
		{
			if (id <= 0) continue;
			// dedupe - small N, linear scan is cheaper than a set
			bool dup = false;
			for (int64_t existing : m_Voice_MutedPeers)
			{
				if (existing == id) { dup = true; break; }
			}
			if (!dup) m_Voice_MutedPeers.push_back(id);
		}
		Save();
	}

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

	EHTTPVersion m_Network_HTTPVersion = EHTTPVersion::HTTP_VERSION_AUTO;
	bool m_Network_UseAlternativeEndpoint = false;

	// -------- Lobby voice chat settings --------
	// Empty string = use the system default communications device.
	bool  m_Voice_Enabled = true;
	std::wstring m_Voice_CaptureDeviceID;
	float m_Voice_MicGain = 1.0f;
	float m_Voice_GlobalVolume = 1.0f;

	// Persistent per-client ignore list. See comment on Voice_GetMutedPeers.
	// Stored in the settings JSON as an array of DECIMAL STRINGS (not raw
	// numbers) because NGMP user IDs can exceed the ~2^53 safe integer
	// precision of nlohmann::json's default number handling.
	std::vector<int64_t> m_Voice_MutedPeers;
};
