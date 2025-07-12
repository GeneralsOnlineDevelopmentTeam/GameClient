#pragma once

class GenOnlineSettings
{
public:
	GenOnlineSettings();

	float Camera_MoveSpeedRatio() const { return m_Camera_MoveSpeedRatio; }
	float Camera_GetMinHeight() const { return m_Camera_MinHeight; }
	float Camera_GetMaxHeight_WhenLobbyHost() const { return m_Camera_MaxHeight_LobbyHost; }

	float DetermineCameraMaxHeight();

	bool Input_LockCursorToGameWindow() const { return m_Input_LockCursorToGameWindow; }

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
	int Graphics_GetFPSLimit() const { return std::clamp<int>(m_Render_FramerateLimit_FPSVal, 60, 240); }

	int GetChatLifeSeconds() const { return std::max<int>(m_Chat_LifeSeconds, 10); }

	void Initialize()
	{
		m_bInitialized = true;
		Load();
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

	bool m_Input_LockCursorToGameWindow = true;

	bool m_bInitialized = false;

	bool m_Render_DrawStatsOverlay = true;
	bool m_Render_LimitFramerate = true;
	int m_Render_FramerateLimit_FPSVal = 60;
	int m_Chat_LifeSeconds = 30;
};