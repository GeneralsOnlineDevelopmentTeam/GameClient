#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "../json.hpp"

#define SETTINGS_KEY_CAMERA "camera"
#define SETTINGS_KEY_CAMERA_MIN_HEIGHT "min_height"
#define SETTINGS_KEY_CAMERA_MAX_HEIGHT "max_height"

#define SETTINGS_KEY_INPUT "input"
#define SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW "lock_cursor_to_game_window"

#define SETTINGS_FILENAME "GeneralsOnline_settings.json"

GenOnlineSettings::GenOnlineSettings()
{
	Initialize();
}

void GenOnlineSettings::Load(void)
{
	bool bApplyDefaults = false;

	std::vector<uint8_t> vecBytes;
	FILE* file = fopen(SETTINGS_FILENAME, "rb");
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

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MAX_HEIGHT))
				{
					m_Camera_MaxHeight = std::min<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MAX_HEIGHT]), m_Camera_MaxHeight_default);
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
		}
		
	}
	else // setup defaults
	{
		bApplyDefaults = true;
	}

	if (bApplyDefaults)
	{
		m_Camera_MinHeight = m_Camera_MinHeight_default;
		m_Camera_MaxHeight = m_Camera_MaxHeight_default;
		m_Input_LockCursorToGameWindow = true;
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
					{SETTINGS_KEY_CAMERA_MAX_HEIGHT, m_Camera_MaxHeight}
				}
		  },

			{
				SETTINGS_KEY_INPUT,
					{
						{SETTINGS_KEY_INPUT_LOCK_CURSOR_TO_GAME_WINDOW, m_Input_LockCursorToGameWindow},
					}
			}
	};
	
	std::string strData = root.dump(1);

	FILE* file = fopen(SETTINGS_FILENAME, "wb");
	if (file)
	{
		fwrite(strData.data(), 1, strData.size(), file);
		fclose(file);
	}
}