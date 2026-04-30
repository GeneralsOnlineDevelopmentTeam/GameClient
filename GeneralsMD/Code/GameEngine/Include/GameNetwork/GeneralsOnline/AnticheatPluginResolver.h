#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AnticheatPluginResolutionResult
{
	bool found = false;
	bool usedDefault = false;
	bool settingsUsedDefault = false;

	std::string configuredValue;
	std::string normalizedPluginName;

	std::filesystem::path selectedPath;
	std::vector<std::filesystem::path> triedPaths;
	std::vector<std::string> notes;

	std::string ToLogString() const;
	std::string ToUserFacingString() const;
};

class AnticheatPluginResolver final
{
public:
	static constexpr const char* DefaultPluginName = "easyanticheat";

	static AnticheatPluginResolutionResult Resolve(
		const std::string& configuredValue,
		bool settingsUsedDefault = false);
};
