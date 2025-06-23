#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "../json.hpp"
#include "../OnlineServices_LobbyInterface.h"
#include "../OnlineServices_Init.h"

#define SETTINGS_KEY_CAMERA "camera"
#define SETTINGS_KEY_CAMERA_MIN_HEIGHT "min_height"
#define SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST "max_height_when_lobby_host"

#define SETTINGS_KEY_INPUT "input"
#define SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW "lock_cursor_to_game_window"

#define SETTINGS_KEY_RENDER "render"
#define SETTINGS_KEY_RENDER_LIMIT_FRAMERATE "limit_framerate"
#define SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL "fps_limit"
#define SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY "stats_overlay"

#define SETTINGS_KEY_CHAT "chat"
#define SETTINGS_KEY_CHAT_LIFE_SECONDS "duration_seconds_until_fade_out"

#define SETTINGS_FILENAME_LEGACY "GeneralsOnline_settings.json"
#define SETTINGS_FILENAME "settings.json"

GenOnlineSettings::GenOnlineSettings()
{
	
}

float GenOnlineSettings::DetermineCameraMaxHeight()
{
	// Are we in a lobby? use it's settings
	NGMP_OnlineServicesManager* pOnlineServicesMgr = NGMP_OnlineServicesManager::GetInstance();
	if (pOnlineServicesMgr != nullptr)
	{
		NGMP_OnlineServices_LobbyInterface* pLobbyInterface = pOnlineServicesMgr->GetLobbyInterface();

		if (pLobbyInterface != nullptr)
		{
			if (pLobbyInterface->IsInLobby())
			{
				LobbyEntry& theLobby = pLobbyInterface->GetCurrentLobby();

				return (float)theLobby.max_cam_height;;
			}
		}
	}

	return (float)GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM;
}

void GenOnlineSettings::Load(void)
{
	char GameDir[MAX_PATH + 1] = {};
	::GetCurrentDirectoryA(MAX_PATH + 1u, GameDir);
	std::string strSettingsFileDir = std::format("{}/GeneralsOnlineData/", TheGlobalData->getPath_UserData().str());
	std::string strSettingsFilePath = std::format("{}/{}", strSettingsFileDir, SETTINGS_FILENAME);
	std::string strSettingsFilePathLegacy = std::format("{}/{}", GameDir, SETTINGS_FILENAME_LEGACY);

	// create directories we need
	if (!std::filesystem::exists(strSettingsFileDir))
	{
		std::filesystem::create_directory(strSettingsFileDir);
	}

	// NGMP_NOTE: Prior to 6/23, we used the game dir for settings, this code migrates any legacy settings file to the new location (game user data dir)
	if (std::filesystem::exists(strSettingsFilePathLegacy))
	{
		std::filesystem::copy(strSettingsFilePathLegacy, strSettingsFilePath, std::filesystem::copy_options::overwrite_existing);
		std::filesystem::remove(strSettingsFilePathLegacy);
	}

	bool bApplyDefaults = false;

	std::vector<uint8_t> vecBytes;
	FILE* file = fopen(strSettingsFilePath.c_str(), "rb");
	if (file)
	{
		fseek(file, 0, SEEK_END);
		long fileSize = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (fileSize > 0)
		{
			vecBytes.resize(fileSize);
			fread(vecBytes.data(), 1, fileSize, file);
		}
		fclose(file);
	}


	if (!vecBytes.empty())
	{
		std::string strJSON = std::string((char*)vecBytes.data(), vecBytes.size());
		nlohmann::json jsonSettings = nullptr;
		
		try
		{
			jsonSettings = nlohmann::json::parse(strJSON);

		}
		catch (...)
		{
			jsonSettings = nullptr;
			bApplyDefaults = true;
		}
		
		if (!bApplyDefaults && jsonSettings != nullptr)
		{
			if (jsonSettings.contains(SETTINGS_KEY_CAMERA))
			{
				auto cameraSettings = jsonSettings[SETTINGS_KEY_CAMERA];

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MIN_HEIGHT))
				{
					m_Camera_MinHeight = std::max<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MIN_HEIGHT]), m_Camera_MinHeight_default);
				}

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST))
				{
					m_Camera_MaxHeight_LobbyHost = std::clamp<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST]), GENERALS_ONLINE_MIN_LOBBY_CAMERA_ZOOM, GENERALS_ONLINE_MAX_LOBBY_CAMERA_ZOOM);
				}
			}

			if (jsonSettings.contains(SETTINGS_KEY_INPUT))
			{
				auto inputSettings = jsonSettings[SETTINGS_KEY_INPUT];

				if (inputSettings.contains(SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW))
				{
					m_Input_LockCursorToGameWindow = inputSettings[SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW];
				}
			}

			if (jsonSettings.contains(SETTINGS_KEY_RENDER))
			{
				auto renderSettings = jsonSettings[SETTINGS_KEY_RENDER];

				if (renderSettings.contains(SETTINGS_KEY_RENDER_LIMIT_FRAMERATE))
				{
					m_Render_LimitFramerate = renderSettings[SETTINGS_KEY_RENDER_LIMIT_FRAMERATE];
				}

				if (renderSettings.contains(SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL))
				{
					m_Render_FramerateLimit_FPSVal = renderSettings[SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL];
				}

				if (renderSettings.contains(SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY))
				{
					m_Render_DrawStatsOverlay = renderSettings[SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY];
				}
			}

			if (jsonSettings.contains(SETTINGS_KEY_CHAT))
			{
				auto chatSettings = jsonSettings[SETTINGS_KEY_CHAT];

				if (chatSettings.contains(SETTINGS_KEY_CHAT_LIFE_SECONDS))
				{
					m_Chat_LifeSeconds = chatSettings[SETTINGS_KEY_CHAT_LIFE_SECONDS];
				}
			}
		}
		
	}
	else // setup defaults
	{
		bApplyDefaults = true;
	}

	if (bApplyDefaults)
	{
		m_Camera_MinHeight = m_Camera_MinHeight_default;
		m_Camera_MaxHeight_LobbyHost = m_Camera_MaxHeight_LobbyHost;
		m_Input_LockCursorToGameWindow = true;
		m_Render_LimitFramerate = true;
		m_Render_FramerateLimit_FPSVal = 60;
		m_Render_DrawStatsOverlay = true;
		m_Chat_LifeSeconds = 30;
	}
	
	// Always save so we re-serialize anything new or missing
	Save();
}

void GenOnlineSettings::Save()
{
	if (!m_bInitialized)
	{
		Initialize();
	}

	nlohmann::json root = {
		  {
				SETTINGS_KEY_CAMERA,
				{
					{SETTINGS_KEY_CAMERA_MIN_HEIGHT, m_Camera_MinHeight},
					{SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST, m_Camera_MaxHeight_LobbyHost}
				}
		  },

			{
				SETTINGS_KEY_INPUT,
					{
						{SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW, m_Input_LockCursorToGameWindow}
					}
			},

		{
			SETTINGS_KEY_RENDER,
				{
					{SETTINGS_KEY_RENDER_LIMIT_FRAMERATE, m_Render_LimitFramerate},
					{SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL, m_Render_FramerateLimit_FPSVal},
					{SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY, m_Render_DrawStatsOverlay},
				}
		},

		{
			SETTINGS_KEY_CHAT,
				{
					{SETTINGS_KEY_CHAT_LIFE_SECONDS, m_Chat_LifeSeconds}
				}
		}
	};
	
	std::string strData = root.dump(1);

	std::string strSettingsFilePath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), SETTINGS_FILENAME);
	FILE* file = fopen(strSettingsFilePath.c_str(), "wb");
	if (file)
	{
		fwrite(strData.data(), 1, strData.size(), file);
		fclose(file);
	}
}