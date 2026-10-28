#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "../json.hpp"
#include "../OnlineServices_LobbyInterface.h"
#include "../OnlineServices_Init.h"
#include <algorithm>
#include "Common/FrameRateLimit.h"
#include "Common/OptionPreferences.h"

#define SETTINGS_KEY_CAMERA "camera"
#define SETTINGS_KEY_CAMERA_MIN_HEIGHT "min_height"
#define SETTINGS_KEY_CAMERA_MOVE_SPEED_RATIO "move_speed_ratio"
#define SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST "max_height_only_when_lobby_host"

#define SETTINGS_KEY_RENDER "render"
#define SETTINGS_KEY_RENDER_LEGACY_LIMIT_FRAMERATE "limit_framerate"
#define SETTINGS_KEY_RENDER_LEGACY_FPS_LIMIT "fps_limit"
#define SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY "stats_overlay"

#define SETTINGS_KEY_CHAT "chat"
#define SETTINGS_KEY_CHAT_LIFE_SECONDS "duration_seconds_until_fade_out"

#define SETTINGS_KEY_SOCIAL "social"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_MENUS "notification_friend_comes_online_menus"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_GAMEPLAY "notification_friend_comes_online_gameplay"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_MENUS "notification_friend_goes_offline_menus"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_GAMEPLAY "notification_friend_goes_offline_gameplay"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_MENUS "notification_player_accepts_request_menus"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_GAMEPLAY "notification_player_accepts_request_gameplay"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_MENUS "notification_player_sends_request_menus"
#define SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_GAMEPLAY "notification_player_sends_request_gameplay"

#define SETTINGS_KEY_DEBUG "debug"
#define SETTINGS_KEY_DEBUG_VERBOSE_LOGGING "verbose_logging"

#define SETTINGS_KEY_NETWORK "network"
#define SETTINGS_KEY_NETWORK_HTTP_VERSION "http_version"
#define SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT "use_alternative_endpoint"

#define SETTINGS_KEY_PLUGINS "plugins"
#define SETTINGS_KEY_PLUGINS_ANTICHEAT "anticheat"

#define SETTINGS_KEY_DATAPACKS "data_packs"
#define SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH "use_community_data_patch"

#define SETTINGS_FILENAME_LEGACY "GeneralsOnline_settings.json"
#define SETTINGS_FILENAME "settings.json"

GenOnlineSettings::GenOnlineSettings()
{
	
}

float GenOnlineSettings::DetermineCameraMaxHeight()
{
	// Are we in a lobby? use it's settings
	NGMP_OnlineServices_LobbyInterface* pLobbyInterface = NGMP_OnlineServicesManager::GetInterface<NGMP_OnlineServices_LobbyInterface>();

	if (pLobbyInterface != nullptr)
	{
		if (pLobbyInterface->IsInLobby())
		{
			LobbyEntry& theLobby = pLobbyInterface->GetCurrentLobby();
			return (float)theLobby.max_cam_height;
		}
	}

	return (float)GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM;
}

std::string GenOnlineSettings::GetAnticheatPlugin() const
{
    // if empty, default to whatever official plugin is available
    if (m_Plugins_Anticheat.empty())
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] AntiCheat name was empty, doing fallback flow");
        std::string pluginPath1 = "plugins/easyanticheat/easyanticheat.dll";
        std::string pluginPath2 = "plugins/goanticheat/goanticheat.dll";

        // prefer EAC
        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Fallback flow: Testing EAC");
        if (std::filesystem::exists(pluginPath2.c_str()))
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Fallback flow: EAC Found.");
            return std::string("easyanticheat");
        }

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Fallback flow: Testing GOAC");
        if (std::filesystem::exists(pluginPath2.c_str()))
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Fallback flow: GOAC Found.");
            return std::string("goanticheat");
        }

        NetworkLog(ELogVerbosity::LOG_RELEASE, "[AC] Fallback failed.");
    }

    return m_Plugins_Anticheat;
}

// The render frame rate limit used to be kept here as well as in Options.ini. A value from an old settings.json moves to the
// RenderFpsLimit key of Options.ini, unless that file already has one.
static void MigrateFpsLimitToOptions(bool limited, int fps)
{
	OptionPreferences options;
	if (options.find("RenderFpsLimit") != options.end())
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[Settings] render.limit_framerate and render.fps_limit dropped, Options.ini already has RenderFpsLimit");
		return;
	}
	const int value = limited ? std::clamp(fps, 30, 480) : static_cast<int>(RenderFpsPreset::UncappedFpsValue);
	AsciiString text;
	text.format("%d", value);
	options["RenderFpsLimit"] = text;
	options.write();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Settings] Moved render.limit_framerate=%d render.fps_limit=%d to Options.ini RenderFpsLimit=%d", limited ? 1 : 0, fps, value);
}

void GenOnlineSettings::Load(void)
{
	char GameDir[MAX_PATH + 1] = {};
	::GetCurrentDirectoryA(MAX_PATH + 1u, GameDir);
	std::string strSettingsFileDir = std::format("{}/GeneralsOnlineData/", TheGlobalData->getPath_UserData().str());
	std::string strSettingsFilePath = std::format("{}/{}", strSettingsFileDir, SETTINGS_FILENAME);
	std::string strSettingsFilePathLegacy = std::format("{}/{}", GameDir, SETTINGS_FILENAME_LEGACY);

	std::error_code ec;

	// create directories we need
	if (!std::filesystem::exists(strSettingsFileDir, ec))
	{
		std::filesystem::create_directory(strSettingsFileDir, ec);
	}

	// NGMP_NOTE: Prior to 6/23, we used the game dir for settings, this code migrates any legacy settings file to the new location (game user data dir)
	if (std::filesystem::exists(strSettingsFilePathLegacy, ec))
	{
		// only drop the legacy file once its copy is in place
		std::filesystem::copy(strSettingsFilePathLegacy, strSettingsFilePath, std::filesystem::copy_options::overwrite_existing, ec);
		if (!ec)
		{
			std::filesystem::remove(strSettingsFilePathLegacy, ec);
		}
	}

	bool bApplyDefaults = false;

	// a settings file we couldn't read or parse must not be silently replaced by defaults
	bool bReadFailed = false;
	bool bParseFailed = false;

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
			if (fread(vecBytes.data(), 1, fileSize, file) != static_cast<size_t>(fileSize))
			{
				vecBytes.clear();
				bReadFailed = true;
			}
		}
		else if (fileSize < 0)
		{
			bReadFailed = true;
		}
		fclose(file);
	}
	else if (errno != ENOENT)
	{
		bReadFailed = true;
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
			bParseFailed = true;
		}
		
		if (!bApplyDefaults && jsonSettings != nullptr)
		try
		{
			if (jsonSettings.contains(SETTINGS_KEY_CAMERA))
			{
				auto cameraSettings = jsonSettings[SETTINGS_KEY_CAMERA];

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MIN_HEIGHT))
				{
					m_Camera_MinHeight = std::max<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MIN_HEIGHT]), m_Camera_MinHeight_default);
				}

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MOVE_SPEED_RATIO))
				{
					m_Camera_MoveSpeedRatio = std::clamp<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MOVE_SPEED_RATIO]), 0.05f, 25.f);
				}

				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST))
				{
					m_Camera_MaxHeight_LobbyHost = std::clamp<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST]), GENERALS_ONLINE_MIN_LOBBY_CAMERA_ZOOM, GENERALS_ONLINE_MAX_LOBBY_CAMERA_ZOOM);
				}
			}

			if (jsonSettings.contains(SETTINGS_KEY_RENDER))
			{
				auto renderSettings = jsonSettings[SETTINGS_KEY_RENDER];

				// the frame rate limit lives in Options.ini now: carry an old value over, the keys are dropped on the next save
				const bool hasLimited = renderSettings.contains(SETTINGS_KEY_RENDER_LEGACY_LIMIT_FRAMERATE);
				const bool hasFps = renderSettings.contains(SETTINGS_KEY_RENDER_LEGACY_FPS_LIMIT);
				if (hasLimited || hasFps)
				{
					const auto& limitedValue = renderSettings[SETTINGS_KEY_RENDER_LEGACY_LIMIT_FRAMERATE];
					const auto& fpsValue = renderSettings[SETTINGS_KEY_RENDER_LEGACY_FPS_LIMIT];
					MigrateFpsLimitToOptions(!hasLimited || !limitedValue.is_boolean() || limitedValue.get<bool>(),
						hasFps && fpsValue.is_number_integer() ? fpsValue.get<int>() : 60);
				}

				if (renderSettings.contains(SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY))
				{
					m_Render_DrawStatsOverlay = renderSettings[SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY];
				}
			}

            if (jsonSettings.contains(SETTINGS_KEY_NETWORK))
            {
                auto networkSettings = jsonSettings[SETTINGS_KEY_NETWORK];

                if (networkSettings.contains(SETTINGS_KEY_NETWORK_HTTP_VERSION))
                {
                   int httpVersion = networkSettings[SETTINGS_KEY_NETWORK_HTTP_VERSION];

				   // clamp
				   if (httpVersion < 0 || httpVersion > HTTP_VERSION_3_0)
				   {
					   m_Network_HTTPVersion = EHTTPVersion::HTTP_VERSION_AUTO;
				   }
				   else
				   {
					   m_Network_HTTPVersion = (EHTTPVersion)httpVersion;
				   }
                }

                if (networkSettings.contains(SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT))
                {
                    m_Network_UseAlternativeEndpoint = networkSettings[SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT];
                }
            }

			if (jsonSettings.contains(SETTINGS_KEY_DEBUG))
			{
				auto debugSettings = jsonSettings[SETTINGS_KEY_DEBUG];

				if (debugSettings.contains(SETTINGS_KEY_DEBUG_VERBOSE_LOGGING))
				{
					m_bVerbose = debugSettings[SETTINGS_KEY_DEBUG_VERBOSE_LOGGING];
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

            if (jsonSettings.contains(SETTINGS_KEY_SOCIAL))
            {
                auto socialSettings = jsonSettings[SETTINGS_KEY_SOCIAL];

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_MENUS))
                {
					m_Social_Notification_FriendComesOnline_Menus = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_MENUS];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_GAMEPLAY))
                {
					m_Social_Notification_FriendComesOnline_Gameplay = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_GAMEPLAY];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_MENUS))
                {
					m_Social_Notification_FriendGoesOffline_Menus = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_MENUS];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_GAMEPLAY))
                {
					m_Social_Notification_FriendGoesOffline_Gameplay = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_GAMEPLAY];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_MENUS))
                {
                    m_Social_Notification_PlayerAcceptsRequest_Menus = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_MENUS];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_GAMEPLAY))
                {
                    m_Social_Notification_PlayerAcceptsRequest_Gameplay = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_GAMEPLAY];
                }
                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_MENUS))
                {
                    m_Social_Notification_PlayerSendsRequest_Menus = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_MENUS];
                }

                if (socialSettings.contains(SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_GAMEPLAY))
                {
                    m_Social_Notification_PlayerSendsRequest_Gameplay = socialSettings[SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_GAMEPLAY];
                }
            }

			if (jsonSettings.contains(SETTINGS_KEY_PLUGINS))
			{
				auto pluginSettings = jsonSettings[SETTINGS_KEY_PLUGINS];

				if (pluginSettings.contains(SETTINGS_KEY_PLUGINS_ANTICHEAT))
				{
					m_Plugins_Anticheat = pluginSettings[SETTINGS_KEY_PLUGINS_ANTICHEAT];
				}
			}

            if (jsonSettings.contains(SETTINGS_KEY_DATAPACKS))
            {
                auto datapackSettings = jsonSettings[SETTINGS_KEY_DATAPACKS];

                if (datapackSettings.contains(SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH))
                {
                    m_DataPacks_UseCommunityPatch = datapackSettings[SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH];
                }
            }
		}
		catch (...)
		{
			// a value of the wrong type
			bApplyDefaults = true;
			bParseFailed = true;
		}
		
	}
	else // setup defaults
	{
		bApplyDefaults = true;
	}

	if (bApplyDefaults)
	{
		m_Camera_MinHeight = m_Camera_MinHeight_default;
		m_Camera_MaxHeight_LobbyHost = GENERALS_ONLINE_DEFAULT_LOBBY_CAMERA_ZOOM;
		m_bVerbose = false;
		m_Render_DrawStatsOverlay = true;
		m_Chat_LifeSeconds = 30;

        m_Social_Notification_FriendComesOnline_Menus = true;
        m_Social_Notification_FriendComesOnline_Gameplay = true;
        m_Social_Notification_FriendGoesOffline_Menus = true;
        m_Social_Notification_FriendGoesOffline_Gameplay = true;
	}
	
	if (bReadFailed)
	{
		// keep the file on disk, the defaults above only apply to this session
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Could not read settings file, keeping it untouched");
		return;
	}

	if (bParseFailed)
	{
		// keep the unparseable file for recovery before the defaults replace it
		std::filesystem::rename(strSettingsFilePath, strSettingsFilePath + ".bad", ec);
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
					{SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST, m_Camera_MaxHeight_LobbyHost},
					{ SETTINGS_KEY_CAMERA_MOVE_SPEED_RATIO, m_Camera_MoveSpeedRatio },
				}
		  },

		{
			SETTINGS_KEY_RENDER,
				{
					{SETTINGS_KEY_RENDER_DRAW_STATS_OVERLAY, m_Render_DrawStatsOverlay},
				}
		},

        {
            SETTINGS_KEY_NETWORK,
                {
                    {SETTINGS_KEY_NETWORK_HTTP_VERSION, m_Network_HTTPVersion},
                    {SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT, m_Network_UseAlternativeEndpoint}
                }
        },

		{
			SETTINGS_KEY_CHAT,
				{
					{SETTINGS_KEY_CHAT_LIFE_SECONDS, m_Chat_LifeSeconds}
				}
		},

		{
			SETTINGS_KEY_DEBUG,
				{
					{SETTINGS_KEY_DEBUG_VERBOSE_LOGGING, m_bVerbose},
				}
		},

        {
			SETTINGS_KEY_SOCIAL,
                {
                    {SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_MENUS, m_Social_Notification_FriendComesOnline_Menus},
					{SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_COMES_ONLINE_GAMEPLAY, m_Social_Notification_FriendComesOnline_Gameplay},
					{SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_MENUS, m_Social_Notification_FriendGoesOffline_Menus},
					{SETTINGS_KEY_SOCIAL_NOTIFICATIONS_FRIEND_GOES_OFFLINE_GAMEPLAY, m_Social_Notification_FriendGoesOffline_Gameplay},
					{SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_MENUS, m_Social_Notification_PlayerAcceptsRequest_Menus},
					{SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_ACCEPTS_REQUEST_GAMEPLAY, m_Social_Notification_PlayerAcceptsRequest_Gameplay},
                    {SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_MENUS, m_Social_Notification_PlayerSendsRequest_Menus},
                    {SETTINGS_KEY_SOCIAL_NOTIFICATIONS_PLAYER_SENDS_REQUEST_GAMEPLAY, m_Social_Notification_PlayerSendsRequest_Gameplay},
                }
        },

        {
			SETTINGS_KEY_PLUGINS,
                {
                    {SETTINGS_KEY_PLUGINS_ANTICHEAT, m_Plugins_Anticheat}
                }
        },

        {
            SETTINGS_KEY_DATAPACKS,
                {
                    {SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH, m_DataPacks_UseCommunityPatch}
                }
        }
    };
	
	std::string strData = root.dump(1);

	std::string strSettingsFilePath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), SETTINGS_FILENAME);

	// write to a temp file and swap it in so a failed write never leaves a truncated settings file
	std::string strTempPath = strSettingsFilePath + ".tmp";
	bool bSaved = false;
	FILE* file = fopen(strTempPath.c_str(), "wb");
	if (file)
	{
		bSaved = fwrite(strData.data(), 1, strData.size(), file) == strData.size();
		if (fclose(file) != 0)
			bSaved = false;
	}

	if (bSaved)
		bSaved = MoveFileExA(strTempPath.c_str(), strSettingsFilePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;

	if (!bSaved)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Failed to save settings file");
		std::error_code ec;
		std::filesystem::remove(strTempPath, ec);
	}
}
