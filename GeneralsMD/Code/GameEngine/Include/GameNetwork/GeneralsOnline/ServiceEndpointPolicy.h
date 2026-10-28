#pragma once
#include <string>
#include <vector>
#include <curl/curl.h>

struct ServiceConnectionSettings
{
	// "automatic" picks a working route, "custom" uses only serverAddress
	std::string serverMode = "automatic";
	// https origin of the custom server, empty unless serverMode is custom
	std::string serverAddress;
	// "automatic" follows the Windows proxy settings, "disabled" connects directly, "custom" uses proxyURL
	std::string proxyMode = "automatic";
	std::string proxyURL;
	// try the current Windows account when the proxy asks for a login
	bool windowsLogin = true;
};

namespace ServiceConnection
{
// host[:port] or https://host[:port] to its origin (https, lower case host, default port dropped); false with a reason for anything else
bool NormalizeCustomEndpoint(const std::string& typed, std::string& origin, std::string& error);
// The key the login token is stored under: one for every GO route, one per custom origin, empty when a custom server has no valid address
std::string CredentialServerKey(const ServiceConnectionSettings& settings);
// remembered is the host of the route that worked last, ignored unless it is one of the automatic routes
std::vector<std::string> BootstrapRoutes(const std::string& environment,
    const ServiceConnectionSettings& settings, const std::string& remembered);
// host name of a route url
std::string RouteHost(const std::string& route);
bool IsTrustedServiceURL(const std::string& url, const std::string& base, bool websocket);
// wss address of the websocket on a service base: the scheme swapped from https and ws appended to the path; empty for a base that is not https
std::string WebSocketURLForBase(const std::string& base);
std::string RequestURLForLog(const std::string& url, const std::string& serviceBase);
bool IsExpectedVersionManifest(const std::string& body);
// Verified TLS (not in _DEBUG builds), no redirects, no embedded credentials, direct connection; HTTPS or WSS only.
bool ConfigureServiceTransport(CURL* curl, const std::string& url, std::string& error);
}
