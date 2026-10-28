#pragma once
#include <string>
#include <vector>
#include <curl/curl.h>

struct ServiceConnectionSettings
{
	// "automatic" picks a working route
	std::string serverMode = "automatic";
};

namespace ServiceConnection
{
// remembered is the host of the route that worked last, ignored unless it is one of the automatic routes
std::vector<std::string> BootstrapRoutes(const std::string& environment,
    const ServiceConnectionSettings& settings, const std::string& remembered);
// host name of a route url
std::string RouteHost(const std::string& route);
bool IsTrustedServiceURL(const std::string& url, const std::string& base, bool websocket);
// wss address of the websocket on a service base: the scheme swapped from https and ws appended to the path; empty for a base that is not https
std::string WebSocketURLForBase(const std::string& base);
bool IsExpectedVersionManifest(const std::string& body);
// Verified TLS (not in _DEBUG builds), no redirects, no embedded credentials, direct connection; HTTPS or WSS only.
bool ConfigureServiceTransport(CURL* curl, const std::string& url, std::string& error);
}
