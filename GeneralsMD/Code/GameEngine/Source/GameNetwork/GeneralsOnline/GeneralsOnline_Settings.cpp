#include "GameNetwork/GeneralsOnline/GeneralsOnline_Settings.h"
#include "Common/ArchiveFileSystem.h"
#include "../json.hpp"
#include "../OnlineServices_LobbyInterface.h"
#include "../OnlineServices_Init.h"
#include <algorithm>
#include "GameNetwork/GeneralsOnline/CredentialStore.h"
#include "GameNetwork/GeneralsOnline/OnlineServices_Auth.h"
#include "Common/OptionPreferences.h"

#define SETTINGS_KEY_CAMERA "camera"
#define SETTINGS_KEY_CAMERA_MIN_HEIGHT "min_height"
#define SETTINGS_KEY_CAMERA_LEGACY_MOVE_SPEED_RATIO "move_speed_ratio"
#define SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST "max_height_only_when_lobby_host"

#define SETTINGS_KEY_RENDER "render"
#define SETTINGS_KEY_RENDER_LIMIT_FRAMERATE "limit_framerate"
#define SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL "fps_limit"
#define SETTINGS_KEY_RENDER_LEGACY_DRAW_STATS_OVERLAY "stats_overlay"

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
#define SETTINGS_KEY_NETWORK_LEGACY_HTTP_VERSION "http_version"
#define SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT "use_alternative_endpoint"
#define SETTINGS_KEY_NETWORK_SERVER "server"
#define SETTINGS_KEY_NETWORK_SERVER_MODE "mode"
#define SETTINGS_KEY_NETWORK_SERVER_ADDRESS "address"
#define SETTINGS_KEY_NETWORK_PROXY "proxy"
#define SETTINGS_KEY_NETWORK_PROXY_MODE "mode"
#define SETTINGS_KEY_NETWORK_PROXY_ADDRESS "address"
#define SETTINGS_KEY_NETWORK_PROXY_WINDOWS_LOGIN "windows_login"
#define SETTINGS_KEY_NETWORK_LAST_ROUTE "last_route"
#define LEGACY_ALTERNATIVE_ROUTE_HOST "api-ru.playgenerals.online"

#define SETTINGS_KEY_PLUGINS "plugins"
#define SETTINGS_KEY_PLUGINS_ANTICHEAT "anticheat"

#define SETTINGS_KEY_DATAPACKS "data_packs"
#define SETTINGS_KEY_DATAPACKS_USER "user"
#define SETTINGS_KEY_DATAPACKS_GENERALS_ONLINE "generals_online"
#define SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH "community_patch"
#define SETTINGS_KEY_DATAPACKS_ENABLED "enabled"
#define SETTINGS_KEY_DATAPACKS_INCLUDE_PATHS "include_paths"
#define SETTINGS_KEY_DATAPACKS_EXCLUDE_PATHS "exclude_paths"
#define SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH_LEGACY "use_community_data_patch"	// before the layers, read once and dropped on save

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

// Reads a list of paths of data_packs, an entry that is not valid is dropped. Returns FALSE and keeps the list when the field is missing or not a list.
static bool LoadDataPackPaths(const nlohmann::json& jsonLayer, const char* layerName, const char* key, bool excludes, std::vector<std::string>& paths)
{
    if (!jsonLayer.contains(key))
    {
        return false;
    }

    if (!jsonLayer[key].is_array())
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s: %s is not a list, using the default", layerName, key);
        return false;
    }

    paths.clear();
    for (const auto& path : jsonLayer[key])
    {
        AsciiString reason;
        bool bValid = false;
        if (path.is_string())
        {
            bValid = excludes ? PathExcludeList::parse(path.get<std::string>(), nullptr, &reason) : ArchiveFileSystem::parseMountEntry(AsciiString(path.get<std::string>().c_str()), nullptr, &reason);
        }

        if (bValid)
        {
            paths.push_back(path.get<std::string>());
        }
        else
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s: %s entry dropped from the settings (%s)", layerName, key, path.is_string() ? reason.str() : "not a string");
        }
    }
    return true;
}

// Reads one layer of data_packs. A missing or invalid field keeps what the layer holds, its default. excludePaths is only read for the user layer.
static void LoadDataPackLayer(const nlohmann::json& dataPacks, const char* name, GenOnlineDataPackLayer& layer, std::vector<std::string>* excludePaths)
{
    if (!dataPacks.contains(name))
    {
        return;
    }

    const nlohmann::json& jsonLayer = dataPacks[name];
    if (!jsonLayer.is_object())
    {
        NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s in the settings is not an object, using the default", name);
        return;
    }

    if (jsonLayer.contains(SETTINGS_KEY_DATAPACKS_ENABLED))
    {
        if (jsonLayer[SETTINGS_KEY_DATAPACKS_ENABLED].is_boolean())
        {
            layer.m_enabled = jsonLayer[SETTINGS_KEY_DATAPACKS_ENABLED].get<bool>();
        }
        else
        {
            NetworkLog(ELogVerbosity::LOG_RELEASE, "Data pack layer %s: enabled is not a boolean, using the default", name);
        }
    }

    LoadDataPackPaths(jsonLayer, name, SETTINGS_KEY_DATAPACKS_INCLUDE_PATHS, false, layer.m_includePaths);

    if (excludePaths != nullptr)
    {
        LoadDataPackPaths(jsonLayer, name, SETTINGS_KEY_DATAPACKS_EXCLUDE_PATHS, true, *excludePaths);
    }
}

static nlohmann::json DataPackLayerToJson(const GenOnlineDataPackLayer& layer, const std::vector<std::string>* excludePaths)
{
    nlohmann::json jsonLayer = nlohmann::json::object();
    jsonLayer[SETTINGS_KEY_DATAPACKS_ENABLED] = layer.m_enabled;
    jsonLayer[SETTINGS_KEY_DATAPACKS_INCLUDE_PATHS] = layer.m_includePaths;
    if (excludePaths != nullptr)
    {
        jsonLayer[SETTINGS_KEY_DATAPACKS_EXCLUDE_PATHS] = *excludePaths;
    }
    return jsonLayer;
}

// The old right mouse scroll speed was ratio * Options.ini scroll speed * (screen width / 2) pixels. The scroll now measures the drag at 800x600
// and uses the 1.6 horizontal speed factor of GameData.ini, so the same speed needs a scroll speed of
// old scroll speed * ratio * (width / 2) / (1.6 * 400), within the range of the options slider.
static void MigrateMoveSpeedRatioToOptions(float ratio)
{
	OptionPreferences options;
	Int width = 0;
	Int height = 0;
	options.getResolution(&width, &height);
	const auto scrollFactor = options.find("ScrollFactor");
	const Int oldSlider = scrollFactor == options.end() ? 50 : (std::max)(1, atoi(scrollFactor->second.str()));
	const Int newSlider = (std::max)(1, static_cast<Int>(oldSlider * ratio * (width / 2.0f) / (1.6f * 400.0f) + 0.5f));
	AsciiString text;
	text.format("%d", newSlider);
	options["ScrollFactor"] = text;
	options.write();
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[Settings] Moved camera.move_speed_ratio=%.2f to Options.ini ScrollFactor: %d -> %d at %dx%d", ratio, oldSlider, newSlider, width, height);
}

void GenOnlineSettings::Load(void)
{
	ServiceConnection::ConfigureCredentialStore(&NGMP_OnlineServices_AuthInterface::GetCredentialsFilePath);
	ServiceConnection::SetCredentialLogSink([](const std::string& line) { NetworkLog(ELogVerbosity::LOG_RELEASE, "%s", line.c_str()); });
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
			if (!jsonSettings.is_object())
				throw std::runtime_error("Settings must be an object");
			m_PreservedJSON = jsonSettings.dump();

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

				// the right mouse scroll speed is the Options.ini scroll speed now, an old ratio is converted once
				if (cameraSettings.contains(SETTINGS_KEY_CAMERA_LEGACY_MOVE_SPEED_RATIO) && cameraSettings[SETTINGS_KEY_CAMERA_LEGACY_MOVE_SPEED_RATIO].is_number())
				{
					MigrateMoveSpeedRatioToOptions(std::clamp<float>(static_cast<float>(cameraSettings[SETTINGS_KEY_CAMERA_LEGACY_MOVE_SPEED_RATIO]), 0.05f, 25.f));
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
			}

            if (jsonSettings.contains(SETTINGS_KEY_NETWORK))
            {
                // Any network value that is unknown, malformed or of the wrong type goes back to its default and is logged, one line per field.
                // The load ends with a save, so the file never keeps the invalid value. Addresses can hold a secret, so their old value is not logged.
                auto resetField = [](const std::string& path, const nlohmann::json& old, bool secret)
                {
                    const std::string oldText = secret ? std::string("hidden") : old.is_string() ? old.get<std::string>().substr(0, 64) : std::string(old.type_name());
                    NetworkLog(ELogVerbosity::LOG_RELEASE, "[Settings] %s is not valid (%s), reset to its default", path.c_str(), oldText.c_str());
                };
                auto choice = [&](const nlohmann::json& section, const std::string& path, const char* key, std::string& value, std::initializer_list<const char*> allowed)
                {
                    if (!section.contains(key))
                    {
                        return;
                    }
                    const auto& found = section[key];
                    if (found.is_string() && std::find_if(allowed.begin(), allowed.end(), [&](const char* name) { return found.get<std::string>() == name; }) != allowed.end())
                    {
                        value = found.get<std::string>();
                        return;
                    }
                    resetField(path + "." + key, found, false);
                };
                // a section that is present but not an object goes back to its defaults
                auto section = [&](const nlohmann::json& parent, const std::string& path, const char* key) -> const nlohmann::json*
                {
                    if (!parent.contains(key))
                    {
                        return nullptr;
                    }
                    if (!parent[key].is_object())
                    {
                        resetField(path + "." + key, parent[key], false);
                        return nullptr;
                    }
                    return &parent[key];
                };

                if (!jsonSettings[SETTINGS_KEY_NETWORK].is_object())
                {
                    resetField(SETTINGS_KEY_NETWORK, jsonSettings[SETTINGS_KEY_NETWORK], false);
                }
                else
                {
                    const auto& networkSettings = jsonSettings[SETTINGS_KEY_NETWORK];

                    if (const auto* serverSettings = section(networkSettings, SETTINGS_KEY_NETWORK, SETTINGS_KEY_NETWORK_SERVER))
                    {
                        const std::string path = std::string(SETTINGS_KEY_NETWORK) + "." + SETTINGS_KEY_NETWORK_SERVER;
                        choice(*serverSettings, path, SETTINGS_KEY_NETWORK_SERVER_MODE, m_ConnectionSettings.serverMode, { "automatic", "custom" });
                        if (serverSettings->contains(SETTINGS_KEY_NETWORK_SERVER_ADDRESS))
                        {
                            const auto& found = (*serverSettings)[SETTINGS_KEY_NETWORK_SERVER_ADDRESS];
                            std::string endpointError;
                            if (found.is_string() && (found.get<std::string>().empty() ||
                                ServiceConnection::NormalizeCustomEndpoint(found.get<std::string>(), m_ConnectionSettings.serverAddress, endpointError)))
                            {
                                // an empty address stays empty
                            }
                            else
                            {
                                resetField(path + "." + SETTINGS_KEY_NETWORK_SERVER_ADDRESS, found, true);
                                m_ConnectionSettings.serverAddress.clear();
                            }
                        }
                    }

                    if (const auto* proxySettings = section(networkSettings, SETTINGS_KEY_NETWORK, SETTINGS_KEY_NETWORK_PROXY))
                    {
                        const std::string path = std::string(SETTINGS_KEY_NETWORK) + "." + SETTINGS_KEY_NETWORK_PROXY;
                        choice(*proxySettings, path, SETTINGS_KEY_NETWORK_PROXY_MODE, m_ConnectionSettings.proxyMode, { "automatic", "disabled", "custom" });
                        if (proxySettings->contains(SETTINGS_KEY_NETWORK_PROXY_ADDRESS))
                        {
                            const auto& found = (*proxySettings)[SETTINGS_KEY_NETWORK_PROXY_ADDRESS];
                            std::string proxyError;
                            if (found.is_string() && (found.get<std::string>().empty() || ServiceConnection::ValidateProxyURL(found.get<std::string>(), proxyError)))
                            {
                                m_ConnectionSettings.proxyURL = found.get<std::string>();
                            }
                            else
                            {
                                resetField(path + "." + SETTINGS_KEY_NETWORK_PROXY_ADDRESS, found, true);
                            }
                        }
                        if (proxySettings->contains(SETTINGS_KEY_NETWORK_PROXY_WINDOWS_LOGIN))
                        {
                            const auto& found = (*proxySettings)[SETTINGS_KEY_NETWORK_PROXY_WINDOWS_LOGIN];
                            if (found.is_boolean())
                            {
                                m_ConnectionSettings.windowsLogin = found.get<bool>();
                            }
                            else
                            {
                                resetField(path + "." + SETTINGS_KEY_NETWORK_PROXY_WINDOWS_LOGIN, found, false);
                            }
                        }
                    }

                    if (networkSettings.contains(SETTINGS_KEY_NETWORK_LAST_ROUTE))
                    {
                        const auto& found = networkSettings[SETTINGS_KEY_NETWORK_LAST_ROUTE];
                        if (found.is_string() && found.get<std::string>().size() <= 253 &&
                            found.get<std::string>().find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") == std::string::npos)
                        {
                            m_LastRoute = found.get<std::string>();
                        }
                        else
                        {
                            resetField(std::string(SETTINGS_KEY_NETWORK) + "." + SETTINGS_KEY_NETWORK_LAST_ROUTE, found, false);
                        }
                    }
                    else if (networkSettings.contains(SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT) &&
                        networkSettings[SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT].is_boolean() &&
                        networkSettings[SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT].get<bool>())
                    {
                        // the old switch of the released client now only prefers that route
                        m_LastRoute = LEGACY_ALTERNATIVE_ROUTE_HOST;
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
                const nlohmann::json& datapackSettings = jsonSettings[SETTINGS_KEY_DATAPACKS];

                LoadDataPackLayer(datapackSettings, SETTINGS_KEY_DATAPACKS_USER, m_DataPacks_User, &m_DataPacks_UserExcludePaths);
                LoadDataPackLayer(datapackSettings, SETTINGS_KEY_DATAPACKS_GENERALS_ONLINE, m_DataPacks_GeneralsOnline, nullptr);
                LoadDataPackLayer(datapackSettings, SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH, m_DataPacks_CommunityPatch, nullptr);

                // a file from before the layers: the old community patch switch becomes community_patch.enabled
                if (!datapackSettings.contains(SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH)
                    && datapackSettings.contains(SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH_LEGACY)
                    && datapackSettings[SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH_LEGACY].is_boolean())
                {
                    m_DataPacks_CommunityPatch.m_enabled = datapackSettings[SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH_LEGACY].get<bool>();
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
		m_Render_LimitFramerate = true;
		m_Render_FramerateLimit_FPSVal = 60;
		m_Chat_LifeSeconds = 30;

        m_Social_Notification_FriendComesOnline_Menus = true;
        m_Social_Notification_FriendComesOnline_Gameplay = true;
        m_Social_Notification_FriendGoesOffline_Menus = true;
        m_Social_Notification_FriendGoesOffline_Gameplay = true;
	}
	
	if (bReadFailed)
	{
		m_CanSave = false;
		// keep the file on disk, the defaults above only apply to this session
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NGMP] Could not read settings file, keeping it untouched");
		return;
	}

	if (bParseFailed)
	{
		// keep the unparseable file for recovery before the defaults replace it
		std::filesystem::rename(strSettingsFilePath, strSettingsFilePath + ".bad", ec);
		if (ec)
		{
			m_CanSave = false;
			return;
		}
		m_PreservedJSON = "{}";
	}

	// Always save so we re-serialize anything new or missing
	Save();
}

bool GenOnlineSettings::Save()
{
	if (!m_bInitialized)
	{
		Initialize();
	}

	if (!m_CanSave)
	{
		return false;
	}

	nlohmann::json root = {
		  {
				SETTINGS_KEY_CAMERA,
				{
					{SETTINGS_KEY_CAMERA_MIN_HEIGHT, m_Camera_MinHeight},
					{SETTINGS_KEY_CAMERA_MAX_HEIGHT_WHEN_LOBBY_HOST, m_Camera_MaxHeight_LobbyHost},
				}
		  },

		{
			SETTINGS_KEY_RENDER,
				{
					{SETTINGS_KEY_RENDER_LIMIT_FRAMERATE, m_Render_LimitFramerate},
					{SETTINGS_KEY_RENDER_FRAMERATE_LIMIT_FPS_VAL, m_Render_FramerateLimit_FPSVal},
				}
		},

        {
            SETTINGS_KEY_NETWORK,
                {
                    {SETTINGS_KEY_NETWORK_SERVER, {
                        {SETTINGS_KEY_NETWORK_SERVER_MODE, m_ConnectionSettings.serverMode},
                        {SETTINGS_KEY_NETWORK_SERVER_ADDRESS, m_ConnectionSettings.serverAddress}}},
                    {SETTINGS_KEY_NETWORK_PROXY, {
                        {SETTINGS_KEY_NETWORK_PROXY_MODE, m_ConnectionSettings.proxyMode},
                        {SETTINGS_KEY_NETWORK_PROXY_ADDRESS, m_ConnectionSettings.proxyURL},
                        {SETTINGS_KEY_NETWORK_PROXY_WINDOWS_LOGIN, m_ConnectionSettings.windowsLogin}}},
                    {SETTINGS_KEY_NETWORK_LAST_ROUTE, m_LastRoute}
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
                    {SETTINGS_KEY_DATAPACKS_USER, DataPackLayerToJson(m_DataPacks_User, &m_DataPacks_UserExcludePaths)},
                    {SETTINGS_KEY_DATAPACKS_GENERALS_ONLINE, DataPackLayerToJson(m_DataPacks_GeneralsOnline, nullptr)},
                    {SETTINGS_KEY_DATAPACKS_COMMUNITY_PATCH, DataPackLayerToJson(m_DataPacks_CommunityPatch, nullptr)}
                }
        }
    };
	
	auto preserved = nlohmann::json::parse(m_PreservedJSON);
	preserved.merge_patch(root);
	preserved[SETTINGS_KEY_DATAPACKS] = root[SETTINGS_KEY_DATAPACKS];	// the client owns data_packs completely, stale keys go
	// settings this build no longer has are dropped from the file
	preserved[SETTINGS_KEY_RENDER].erase(SETTINGS_KEY_RENDER_LEGACY_DRAW_STATS_OVERLAY);
	preserved[SETTINGS_KEY_CAMERA].erase(SETTINGS_KEY_CAMERA_LEGACY_MOVE_SPEED_RATIO);
	preserved[SETTINGS_KEY_NETWORK].erase(SETTINGS_KEY_NETWORK_USE_ALTERNATIVE_ENDPOINT);
	preserved[SETTINGS_KEY_NETWORK].erase(SETTINGS_KEY_NETWORK_LEGACY_HTTP_VERSION);
	std::string strData = preserved.dump(1);

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
	else
	{
		m_PreservedJSON = strData;
	}
	return bSaved;
}

bool GenOnlineSettings::Network_SaveConnectionSettings(const ServiceConnectionSettings& requested, std::string& error)
{
	error.clear();
	ServiceConnectionSettings settings = requested;
	if ((settings.serverMode != "automatic" && settings.serverMode != "custom") ||
		(settings.proxyMode != "automatic" && settings.proxyMode != "disabled" && settings.proxyMode != "custom"))
	{
		error = "Choose a valid service and proxy mode.";
		return false;
	}
	if (settings.serverMode == "custom")
	{
		if (!ServiceConnection::NormalizeCustomEndpoint(settings.serverAddress, settings.serverAddress, error))
		{
			return false;
		}
	}
	else
	{
		settings.serverAddress.clear();
	}
	if ((settings.proxyMode == "custom" || !settings.proxyURL.empty()) && !ServiceConnection::ValidateProxyURL(settings.proxyURL, error))
	{
		return false;
	}
	if (!m_bInitialized)
	{
		Initialize();
	}
	std::lock_guard<std::mutex> lock(m_NetworkMutex);
	const auto previous = m_ConnectionSettings;
	m_ConnectionSettings = settings;
	if (Save())
	{
		// a changed proxy starts without anything typed for the old one
		const std::string previousProxy = previous.proxyMode == "custom" ? ServiceConnection::CanonicalProxyKey(previous.proxyURL) : std::string();
		const std::string proxy = settings.proxyMode == "custom" ? ServiceConnection::CanonicalProxyKey(settings.proxyURL) : std::string();
		if (previousProxy != proxy)
			ServiceConnection::ClearSessionProxyCredentialsExcept(proxy);
		ServiceConnection::InvalidateProxyPolicyCache();
		return true;
	}
	m_ConnectionSettings = previous;
	error = "Could not save connection settings. Check the game user-data folder permissions.";
	return false;
}

bool GenOnlineSettings::Network_SaveWindowsLogin(bool windowsLogin)
{
	if (!m_bInitialized)
	{
		Initialize();
	}
	std::lock_guard<std::mutex> lock(m_NetworkMutex);
	if (m_ConnectionSettings.windowsLogin == windowsLogin)
	{
		return true;
	}
	m_ConnectionSettings.windowsLogin = windowsLogin;
	if (!Save())
	{
		m_ConnectionSettings.windowsLogin = !windowsLogin;
		return false;
	}
	ServiceConnection::InvalidateProxyPolicyCache();
	return true;
}

bool GenOnlineSettings::Network_SaveLastRoute(const std::string& host)
{
	if (host.empty() || host.size() > 253 || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string::npos)
	{
		return false;
	}
	if (!m_bInitialized)
	{
		Initialize();
	}
	std::lock_guard<std::mutex> lock(m_NetworkMutex);
	if (m_LastRoute == host)
	{
		return true;
	}
	const auto previous = m_LastRoute;
	m_LastRoute = host;
	if (Save())
	{
		return true;
	}
	m_LastRoute = previous;
	return false;
}
