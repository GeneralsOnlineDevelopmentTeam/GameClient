#pragma once

class GenOnlineSettings
{
public:
	GenOnlineSettings();

	float Camera_GetMinHeight() const { return m_Camera_MinHeight; }
	float Camera_GetMaxHeight() const { return m_Camera_MaxHeight; }

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

	const float m_Camera_MaxHeight_default = 600.f;
	float m_Camera_MaxHeight = m_Camera_MaxHeight_default;

	bool m_bInitialized = false;
};