#include "GameNetwork/GeneralsOnline/ServiceEndpointPolicy.h"
#include "GameNetwork/GeneralsOnline/json.hpp"
#include <algorithm>
#include <memory>
#include <cstdint>

namespace ServiceConnection
{
static std::string Lower(std::string value);
static std::string Part(CURLU* url, CURLUPart part);

bool NormalizeCustomEndpoint(const std::string& typed, std::string& origin, std::string& error)
{
	const std::string input = typed;
	origin.clear();
	error.clear();
	size_t first = input.find_first_not_of(" \t");
	if (first == std::string::npos)
	{
		error = "Enter the server address.";
		return false;
	}
	std::string value = input.substr(first, input.find_last_not_of(" \t") - first + 1);
	if (std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c >= 127 || c == '\\'; }))
	{
		error = "The server address has characters that are not allowed.";
		return false;
	}
	const std::string lower = Lower(value);
	if (lower.compare(0, 7, "http://") == 0)
	{
		error = "The server must use https. Plain http is not allowed.";
		return false;
	}
	if (lower.compare(0, 8, "https://") == 0)
		value.erase(0, 8);
	else if (value.find("://") != std::string::npos)
	{
		error = "The server must use https.";
		return false;
	}
	if (value.find('@') != std::string::npos)
	{
		error = "The server address can't contain a user name or password.";
		return false;
	}
	if (value.find('?') != std::string::npos)
	{
		error = "The server address can't contain a query.";
		return false;
	}
	if (value.find('#') != std::string::npos)
	{
		error = "The server address can't contain a fragment.";
		return false;
	}
	const auto slash = value.find('/');
	if (slash != std::string::npos && slash + 1 != value.size())
	{
		error = "Enter only the host and port, without a path.";
		return false;
	}
	if (slash != std::string::npos)
		value.erase(slash);
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
	if (!url || curl_url_set(url.get(), CURLUPART_URL, ("https://" + value).c_str(), 0) != CURLUE_OK)
	{
		error = "Enter a host name with an optional port, like server.example:8443.";
		return false;
	}
	const auto host = Lower(Part(url.get(), CURLUPART_HOST));
	const auto port = Part(url.get(), CURLUPART_PORT);
	if (host.empty() || host.find('%') != std::string::npos || !Part(url.get(), CURLUPART_ZONEID).empty() || port == "0")
	{
		error = "Enter a host name with an optional port, like server.example:8443.";
		return false;
	}
	origin = "https://" + host + (port.empty() || port == "443" ? "" : ":" + port);
	return true;
}

std::string CredentialServerKey(const ServiceConnectionSettings& settings)
{
	if (settings.serverMode != "custom")
		return "go";
	std::string origin, error;
	return NormalizeCustomEndpoint(settings.serverAddress, origin, error) ? "custom:" + origin : std::string();
}

std::vector<std::string> BootstrapRoutes(const std::string& environment,
	const ServiceConnectionSettings& settings, const std::string& remembered)
{
	// a custom server is the only route there is: no other server is tried for it
	if (settings.serverMode == "custom")
	{
		std::string origin, error;
		if ((environment != "dev" && environment != "test" && environment != "prod") || !NormalizeCustomEndpoint(settings.serverAddress, origin, error))
			return {};
		return {origin + "/env/" + environment + "/contract/1/"};
	}
	if (environment == "dev")
		return {"https://localhost:9000/env/dev/contract/1/"};
	if (environment == "test")
		return {"https://api.playgenerals.online:2087/env/test/contract/1/"};
	if (environment != "prod")
		return {};
	if (settings.serverMode != "automatic")
		return {};
	std::vector<std::string> routes = {
		"https://api.playgenerals.online/env/prod/contract/1/",
		"https://api-ru.playgenerals.online/env/prod/contract/1/",
	};
	const auto remembered_route = std::find_if(routes.begin(), routes.end(), [&](const std::string& route) { return RouteHost(route) == remembered; });
	if (remembered_route != routes.end())
		std::rotate(routes.begin(), remembered_route, remembered_route + 1);
	return routes;
}

std::string RouteHost(const std::string& route)
{
	const auto schemeEnd = route.find("://");
	if (schemeEnd == std::string::npos)
		return {};
	const auto start = schemeEnd + 3;
	return route.substr(start, route.find_first_of(":/", start) - start);
}

static std::string Lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
	return value;
}

static bool EndsWith(const std::string& value, const std::string& suffix)
{
	return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string Part(CURLU* url, CURLUPart part)
{
	char* value = nullptr;
	if (curl_url_get(url, part, &value, CURLU_DEFAULT_PORT) != CURLUE_OK)
		return {};
	std::string result(value);
	curl_free(value);
	return result;
}

bool IsTrustedServiceURL(const std::string& value, const std::string& base, bool websocket)
{
	if (value.find_first_of("\\\r\n\t #") != std::string::npos)
		return false;
	const auto schemeEnd = value.find("://");
	if (schemeEnd == std::string::npos)
		return false;
	const auto authorityEnd = value.find_first_of("/?", schemeEnd + 3);
	const auto authority = value.substr(schemeEnd + 3,
		authorityEnd == std::string::npos ? std::string::npos : authorityEnd - schemeEnd - 3);
	if (authority.find('@') != std::string::npos || (websocket && value.find('?') != std::string::npos))
		return false;
	std::string rawPath;
	if (authorityEnd != std::string::npos && value[authorityEnd] == '/')
		rawPath = value.substr(authorityEnd, value.find('?', authorityEnd) - authorityEnd);
	// query escaping is legitimate; path separators and dot segments must not change after validation
	const auto lowerPath = Lower(rawPath);
	if ((websocket && rawPath.find('%') != std::string::npos) || lowerPath.find("%2e") != std::string::npos ||
		lowerPath.find("%2f") != std::string::npos || lowerPath.find("%5c") != std::string::npos ||
		rawPath.find("/../") != std::string::npos || rawPath.find("/./") != std::string::npos ||
		EndsWith(rawPath, "/..") || EndsWith(rawPath, "/."))
		return false;
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> expected(curl_url(), curl_url_cleanup);
	if (!url || !expected || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK ||
		curl_url_set(expected.get(), CURLUPART_URL, base.c_str(), 0) != CURLUE_OK)
		return false;
	if (Part(url.get(), CURLUPART_SCHEME) != (websocket ? "wss" : "https"))
		return false;
	auto port = Part(url.get(), CURLUPART_PORT);
	if (port.empty() && websocket)
		port = "443";
	if (Lower(Part(url.get(), CURLUPART_HOST)) != Lower(Part(expected.get(), CURLUPART_HOST)) ||
		port != Part(expected.get(), CURLUPART_PORT))
		return false;
	const auto path = Part(url.get(), CURLUPART_PATH);
	const auto expectedPath = Part(expected.get(), CURLUPART_PATH);
	if (!websocket)
		return path.compare(0, expectedPath.size(), expectedPath) == 0;
	return EndsWith(path, expectedPath + "ws");
}

std::string WebSocketURLForBase(const std::string& base)
{
	if (base.compare(0, 8, "https://") != 0)
		return {};
	return "wss://" + base.substr(8) + "ws";
}

std::string RequestURLForLog(const std::string& value, const std::string& serviceBase)
{
	if (value.size() > 8192 || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c >= 127; }))
		return "[invalid-url]";
	const auto schemeEnd = value.find("://");
	if (schemeEnd == std::string::npos)
		return "[invalid-url]";
	const auto authority = value.substr(schemeEnd + 3, value.find_first_of("/?#", schemeEnd + 3) - schemeEnd - 3);
	if (authority.find('@') != std::string::npos)
		return "[redacted-url]";
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
	if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK || Part(url.get(), CURLUPART_SCHEME) != "https")
		return "[invalid-url]";
	const auto host = Lower(Part(url.get(), CURLUPART_HOST));
	if (host.empty() || host.find('%') != std::string::npos || !Part(url.get(), CURLUPART_ZONEID).empty())
		return "[invalid-url]";
	const auto port = Part(url.get(), CURLUPART_PORT);
	const std::string origin = "https://" + host + (port.empty() || port == "443" ? "" : ":" + port);
	const auto path = Part(url.get(), CURLUPART_PATH);
	// only known service routes are logged; other paths may carry private identifiers
	std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> base(curl_url(), curl_url_cleanup);
	if (path.find('%') != std::string::npos || !IsTrustedServiceURL(origin + path, serviceBase, false) ||
		!base || curl_url_set(base.get(), CURLUPART_URL, serviceBase.c_str(), 0) != CURLUE_OK)
		return origin + "/[redacted]";
	const auto prefix = Part(base.get(), CURLUPART_PATH);
	const auto action = path.substr(prefix.size());
	static const char* const routes[] = {
		"Matchmaking/Playlists", "Matchmaking/Widen", "PlayerStats/Batch",
		"Social/Friends/Requests", "Social/Friends", "Social/Blocked",
		"VersionManifest", "VersionCheck", "CheckLogin", "LoginWithToken", "LoginCode", "RefreshToken",
		"ProvideMWToken", "ConnectionOutcome", "GlobalStats", "ServiceConfig", "Matchmaking",
		"PlayerStats", "Lobbies", "Lobby", "Rooms", "MOTD", "User"
	};
	for (const std::string name : routes)
	{
		if (action == name || action.compare(0, name.size() + 1, name + "/") == 0)
			return origin + prefix + name + (action.size() == name.size() ? "" : "/[redacted]");
	}
	return origin + prefix + "[redacted]";
}

bool IsExpectedVersionManifest(const std::string& body)
{
	const auto json = nlohmann::json::parse(body, nullptr, false);
	return json.is_object() && json.contains("execrc_30") && json.contains("execrc_60") &&
		json["execrc_30"].is_number_unsigned() && json["execrc_60"].is_number_unsigned() &&
		json["execrc_30"].get<uint64_t>() <= UINT32_MAX && json["execrc_60"].get<uint64_t>() <= UINT32_MAX;
}

bool ConfigureServiceTransport(CURL* curl, const std::string& url, std::string& error)
{
	error.clear();
	const bool websocket = url.compare(0, 6, "wss://") == 0;
	if (!curl || (!websocket && url.compare(0, 8, "https://") != 0))
	{
		error = "Service connections require HTTPS or WSS.";
		return false;
	}
	auto set = [&](CURLoption option, auto value) {
		const CURLcode code = curl_easy_setopt(curl, option, value);
		if (code != CURLE_OK && error.empty())
			error = "Unable to configure service transport: " + std::string(curl_easy_strerror(code));
		return code == CURLE_OK;
	};
#if _DEBUG
	// debug builds skip certificate checks for local service testing
	constexpr long verifyPeer = 0L;
	constexpr long verifyHost = 0L;
#else
	constexpr long verifyPeer = 1L;
	constexpr long verifyHost = 2L;
#endif
	return set(CURLOPT_SSL_VERIFYPEER, verifyPeer) && set(CURLOPT_SSL_VERIFYHOST, verifyHost) &&
		set(CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA)) &&
		set(CURLOPT_DISALLOW_USERNAME_IN_URL, 1L) && set(CURLOPT_FOLLOWLOCATION, 0L) &&
		set(CURLOPT_PROTOCOLS_STR, websocket ? "wss" : "https") && set(CURLOPT_PROXY, "");
}
}
