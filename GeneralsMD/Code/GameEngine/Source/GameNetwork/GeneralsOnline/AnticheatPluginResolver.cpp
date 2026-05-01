#include "GameNetwork/GeneralsOnline/AnticheatPluginResolver.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace
{
	std::string Trim(std::string value)
	{
		auto notSpace = [](unsigned char c)
		{
			return !std::isspace(c);
		};

		value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
		value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());

		return value;
	}

	std::string ToLowerAscii(std::string value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});

		return value;
	}

	bool ContainsPathSeparator(const std::string& value)
	{
		return value.find('\\') != std::string::npos || value.find('/') != std::string::npos;
	}

	bool EndsWithDll(const std::string& value)
	{
		const std::string lower = ToLowerAscii(value);
		constexpr const char* suffix = ".dll";
		constexpr size_t suffixLen = 4;

		return lower.size() >= suffixLen &&
			lower.compare(lower.size() - suffixLen, suffixLen, suffix) == 0;
	}

	bool IsSafePluginName(const std::string& value)
	{
		if (value.empty())
		{
			return false;
		}

		for (unsigned char c : value)
		{
			const bool ok = std::isalnum(c) || c == '_' || c == '-';
			if (!ok)
			{
				return false;
			}
		}

		return true;
	}

	fs::path GetExecutableDirectory()
	{
		std::vector<char> buffer(MAX_PATH);

		for (;;)
		{
			const DWORD len = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (len == 0)
			{
				return fs::current_path();
			}

			if (len < buffer.size() - 1)
			{
				return fs::path(buffer.data()).parent_path();
			}

			buffer.resize(buffer.size() * 2);
		}
	}

	bool IsInsideDirectoryLexical(const fs::path& candidate, const fs::path& root)
	{
		const std::string candidateText = ToLowerAscii(candidate.lexically_normal().string());
		std::string rootText = ToLowerAscii(root.lexically_normal().string());

		while (!rootText.empty() && (rootText.back() == '\\' || rootText.back() == '/'))
		{
			rootText.pop_back();
		}

		if (candidateText == rootText)
		{
			return true;
		}

		return candidateText.rfind(rootText + "\\", 0) == 0 ||
			candidateText.rfind(rootText + "/", 0) == 0;
	}

	void AddCandidate(
		AnticheatPluginResolutionResult& result,
		std::set<std::string>& seen,
		const fs::path& pluginsDir,
		const fs::path& candidate,
		const char* reason)
	{
		const fs::path normalized = candidate.lexically_normal();

		if (!IsInsideDirectoryLexical(normalized, pluginsDir))
		{
			result.notes.push_back(
				std::string("Rejected candidate outside plugins directory: ") +
				normalized.string());
			return;
		}

		const std::string key = ToLowerAscii(normalized.string());
		if (seen.insert(key).second)
		{
			result.triedPaths.push_back(normalized);
			result.notes.push_back(
				std::string("Candidate added: ") +
				normalized.string() +
				" (" +
				reason +
				")");
		}
	}
}

std::string AnticheatPluginResolutionResult::ToLogString() const
{
	std::ostringstream out;

	out << "[AC] Plugin resolution\n";
	out << "  configuredValue='" << configuredValue << "'\n";
	out << "  normalizedPluginName='" << normalizedPluginName << "'\n";
	out << "  usedDefault=" << (usedDefault ? "true" : "false") << "\n";
	out << "  settingsUsedDefault=" << (settingsUsedDefault ? "true" : "false") << "\n";
	out << "  found=" << (found ? "true" : "false") << "\n";

	if (!selectedPath.empty())
	{
		out << "  selectedPath='" << selectedPath.string() << "'\n";
	}

	out << "  triedPaths:\n";
	for (const fs::path& path : triedPaths)
	{
		out << "    - " << path.string() << "\n";
	}

	out << "  notes:\n";
	for (const std::string& note : notes)
	{
		out << "    - " << note << "\n";
	}

	return out.str();
}

std::string AnticheatPluginResolutionResult::ToUserFacingString() const
{
	std::ostringstream out;

	out << "GeneralsOnline could not prepare the AntiCheat plugin.\n\n";
	out << "What is wrong:\n";

	if (settingsUsedDefault)
	{
		out << "The AntiCheat plugin setting was missing, empty, or invalid, so it was healed to easyanticheat.\n\n";
	}
	else if (configuredValue.empty())
	{
		out << "The AntiCheat plugin setting is empty.\n\n";
	}
	else
	{
		out << "The configured AntiCheat plugin value is: " << configuredValue << "\n\n";
	}

	out << "Expected plugin:\n";
	out << "plugins\\easyanticheat\\easyanticheat.dll\n\n";

	if (!triedPaths.empty())
	{
		out << "Paths checked:\n";
		for (const fs::path& path : triedPaths)
		{
			out << "- " << path.string() << "\n";
		}
	}

	return out.str();
}

AnticheatPluginResolutionResult AnticheatPluginResolver::Resolve(
	const std::string& configuredValue,
	bool settingsUsedDefault)
{
	AnticheatPluginResolutionResult result;
	result.configuredValue = configuredValue;
	result.settingsUsedDefault = settingsUsedDefault;
	result.usedDefault = settingsUsedDefault;

	const fs::path exeDir = GetExecutableDirectory();
	const fs::path pluginsDir = exeDir / "plugins";
	std::set<std::string> seen;

	const std::string raw = Trim(configuredValue);
	const std::string lower = ToLowerAscii(raw);

	if (lower.empty())
	{
		result.usedDefault = true;
		result.normalizedPluginName = DefaultPluginName;
		result.notes.push_back("Configured AntiCheat plugin was empty; defaulting to easyanticheat.");
	}
	else if (settingsUsedDefault)
	{
		result.normalizedPluginName = DefaultPluginName;
		result.notes.push_back("Settings healed the configured AntiCheat plugin value to easyanticheat.");
	}
	else if (ContainsPathSeparator(lower) || EndsWithDll(lower))
	{
		const fs::path rawPath(raw);

		if (rawPath.is_absolute())
		{
			AddCandidate(result, seen, pluginsDir, rawPath, "absolute configured path");
		}
		else if (EndsWithDll(lower) && !ContainsPathSeparator(lower))
		{
			const fs::path dllName = rawPath.filename();
			const fs::path stem = dllName.stem();

			AddCandidate(result, seen, pluginsDir, pluginsDir / dllName, "dll filename under plugins");
			AddCandidate(result, seen, pluginsDir, pluginsDir / stem / dllName, "dll filename under plugin subdirectory");
		}
		else
		{
			AddCandidate(result, seen, pluginsDir, exeDir / rawPath, "relative configured path");
		}

		result.normalizedPluginName = rawPath.stem().string();
	}
	else if (IsSafePluginName(lower))
	{
		result.normalizedPluginName = lower;
	}
	else
	{
		result.usedDefault = true;
		result.normalizedPluginName = DefaultPluginName;
		result.notes.push_back(
			"Configured AntiCheat plugin contained unsupported characters; defaulting to easyanticheat.");
	}

	if (!result.normalizedPluginName.empty() && IsSafePluginName(result.normalizedPluginName))
	{
		AddCandidate(
			result,
			seen,
			pluginsDir,
			pluginsDir / result.normalizedPluginName / (result.normalizedPluginName + ".dll"),
			"canonical plugin-name layout");

		AddCandidate(
			result,
			seen,
			pluginsDir,
			pluginsDir / (result.normalizedPluginName + ".dll"),
			"flat plugin-name layout");
	}

	if (result.normalizedPluginName != DefaultPluginName)
	{
		AddCandidate(
			result,
			seen,
			pluginsDir,
			pluginsDir / DefaultPluginName / (std::string(DefaultPluginName) + ".dll"),
			"current production fallback");
	}

	for (const fs::path& candidate : result.triedPaths)
	{
		std::error_code ec;

		if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec))
		{
			result.found = true;
			result.selectedPath = candidate;
			result.notes.push_back("Selected existing AntiCheat plugin: " + candidate.string());
			return result;
		}
	}

	result.notes.push_back("No AntiCheat plugin file was found in any approved candidate path.");
	return result;
}
