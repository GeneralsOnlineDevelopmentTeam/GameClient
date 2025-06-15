#pragma once

class GenOnlineSettings
{
public:
	GenOnlineSettings();

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

	float m_Camera_MaxHeight_LobbyHost = GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM;

	bool m_Input_LockCursorToGameWindow = true;

	bool m_bInitialized = false;
};