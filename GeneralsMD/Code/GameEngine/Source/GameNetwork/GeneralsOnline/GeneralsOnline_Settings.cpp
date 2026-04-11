#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "../json.hpp"
#include "../OnlineServices_LobbyInterface.h"
#include "../OnlineServices_Init.h"

#include <windows.h>  // MultiByteToWideChar / WideCharToMultiByte for voice device id

namespace
{
	// UTF-8 <-> wstring helpers for JSON-serialising the capture device id.
	std::string WStringToUtf8(const std::wstring& ws)
	{
		if (ws.empty()) return std::string();
		int needed = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
			nullptr, 0, nullptr, nullptr);
		if (needed <= 0) return std::string();
		std::string out(static_cast<size_t>(needed), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
			out.data(), needed, nullptr, nullptr);
		return out;
	}

	std::wstring Utf8ToWString(const std::string& s)
	{
		if (s.empty()) return std::wstring();
		int needed = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
			nullptr, 0);
		if (needed <= 0) return std::wstring();
		std::wstring out(static_cast<size_t>(needed), L'\0');
		::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
			out.data(), needed);
		return out;
	}
}

#define SETTINGS_KEY_CAMERA "camera"
#define SETTINGS_KEY_CAMERA_MIN_HEIGHT "min_height"
#define SETTINGS_KEY_CAMERA_MOVE_SPEED_RATIO "move_speed_ratio"
#define SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST "max_height_only_when_lobby_host"

#define SETTINGS_KEY_RENDER "render"
#define SETTINGS_KEY_RENDER_LIMIT_FRAMERATE "limit_framerate"
#define SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL "fps_limit"
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

#define SETTINGS_KEY_VOICE "voice"
#define SETTINGS_KEY_VOICE_ENABLED "enabled"
#define SETTINGS_KEY_VOICE_CAPTURE_DEVICE_ID "capture_device_id"
#define SETTINGS_KEY_VOICE_MIC_GAIN "mic_gain"
#define SETTINGS_KEY_VOICE_GLOBAL_VOLUME "global_volume"
#define SETTINGS_KEY_VOICE_MUTED_PEERS "muted_peers"


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

            if (jsonSettings.contains(SETTINGS_KEY_VOICE))
            {
                auto voiceSettings = jsonSettings[SETTINGS_KEY_VOICE];

                if (voiceSettings.contains(SETTINGS_KEY_VOICE_ENABLED))
                {
                    m_Voice_Enabled = voiceSettings[SETTINGS_KEY_VOICE_ENABLED];
                }

                if (voiceSettings.contains(SETTINGS_KEY_VOICE_CAPTURE_DEVICE_ID))
                {
                    std::string idUtf8 = voiceSettings[SETTINGS_KEY_VOICE_CAPTURE_DEVICE_ID];
                    m_Voice_CaptureDeviceID = Utf8ToWString(idUtf8);
                }

                if (voiceSettings.contains(SETTINGS_KEY_VOICE_MIC_GAIN))
                {
                    m_Voice_MicGain = std::clamp<float>(
                        static_cast<float>(voiceSettings[SETTINGS_KEY_VOICE_MIC_GAIN]),
                        0.0f, 4.0f);
                }

                if (voiceSettings.contains(SETTINGS_KEY_VOICE_GLOBAL_VOLUME))
                {
                    m_Voice_GlobalVolume = std::clamp<float>(
                        static_cast<float>(voiceSettings[SETTINGS_KEY_VOICE_GLOBAL_VOLUME]),
                        0.0f, 2.0f);
                }

                // Persistent per-client mute list. Stored as decimal
                // strings to preserve int64 precision (see header comment).
                if (voiceSettings.contains(SETTINGS_KEY_VOICE_MUTED_PEERS))
                {
                    m_Voice_MutedPeers.clear();
                    const auto& arr = voiceSettings[SETTINGS_KEY_VOICE_MUTED_PEERS];
                    if (arr.is_array())
                    {
                        for (const auto& item : arr)
                        {
                            int64_t id = 0;
                            try
                            {
                                if (item.is_string())
                                {
                                    id = std::stoll(item.get<std::string>());
                                }
                                else if (item.is_number_integer())
                                {
                                    id = item.get<int64_t>();
                                }
                                else
                                {
                                    continue;
                                }
                            }
                            catch (...)
                            {
                                continue; // corrupt entry, skip silently
                            }

                            if (id <= 0) continue;

                            // Dedupe on load so a hand-edited file cannot
                            // blow up memory with repeats.
                            bool dup = false;
                            for (int64_t existing : m_Voice_MutedPeers)
                            {
                                if (existing == id) { dup = true; break; }
                            }
                            if (!dup) m_Voice_MutedPeers.push_back(id);
                        }
                    }
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
		m_bVerbose = false;
		m_Render_LimitFramerate = true;
		m_Render_FramerateLimit_FPSVal = 60;
		m_Render_DrawStatsOverlay = true;
		m_Chat_LifeSeconds = 30;

        m_Social_Notification_FriendComesOnline_Menus = true;
        m_Social_Notification_FriendComesOnline_Gameplay = true;
        m_Social_Notification_FriendGoesOffline_Menus = true;
        m_Social_Notification_FriendGoesOffline_Gameplay = true;
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
					{SETTINGS_KEY_RENDER_LIMIT_FRAMERATE, m_Render_LimitFramerate},
					{SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL, m_Render_FramerateLimit_FPSVal},
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
            SETTINGS_KEY_VOICE,
                {
                    {SETTINGS_KEY_VOICE_ENABLED, m_Voice_Enabled},
                    {SETTINGS_KEY_VOICE_CAPTURE_DEVICE_ID, WStringToUtf8(m_Voice_CaptureDeviceID)},
                    {SETTINGS_KEY_VOICE_MIC_GAIN, m_Voice_MicGain},
                    {SETTINGS_KEY_VOICE_GLOBAL_VOLUME, m_Voice_GlobalVolume}
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
    };

	// Persistent voice ignore list. Stored as an array of decimal strings
	// to preserve full int64 precision (nlohmann::json uses double for raw
	// numbers, which truncates NGMP user IDs above ~2^53). Built after the
	// root initializer to avoid init-list template ambiguities.
	{
		nlohmann::json mutedArr = nlohmann::json::array();
		for (int64_t id : m_Voice_MutedPeers)
		{
			if (id <= 0) continue;
			mutedArr.push_back(std::to_string(id));
		}
		root[SETTINGS_KEY_VOICE][SETTINGS_KEY_VOICE_MUTED_PEERS] = mutedArr;
	}

	std::string strData = root.dump(1);

	std::string strSettingsFilePath = std::format("{}/GeneralsOnlineData/{}", TheGlobalData->getPath_UserData().str(), SETTINGS_FILENAME);
	FILE* file = fopen(strSettingsFilePath.c_str(), "wb");
	if (file)
	{
		fwrite(strData.data(), 1, strData.size(), file);
		fclose(file);
	}
}
