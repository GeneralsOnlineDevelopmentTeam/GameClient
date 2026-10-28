#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <curl/curl.h>
#include "GameNetwork/GeneralsOnline/ServiceEndpointPolicy.h"

namespace ServiceConnection
{
struct ProxyRoute
{
	std::string url;
	bool direct = true;
	bool allowIntegratedAuth = false;
	// the Windows login can be used here when the player wants it; never for SOCKS
	bool integratedAuthPossible = false;
};

struct ResolvedProxyPolicy
{
	std::vector<ProxyRoute> routes;
	std::string error;
	unsigned long nativeError = 0;
	std::string failurePhase;
	std::string fallbackReason;
};

struct ProxyResolution
{
	std::atomic<bool> ready{false};
	ResolvedProxyPolicy result;
};
// Successful system policies expire after 60 seconds; settings saves invalidate immediately.
void InvalidateProxyPolicyCache();
bool ShouldLogProxyPolicy(bool websocket, const ResolvedProxyPolicy& result);
std::shared_ptr<ProxyResolution> StartProxyResolution(ServiceConnectionSettings settings, std::string destinationURL);

// Worker-only: PAC/WPAD resolution can take up to five seconds. Only absent optional WPAD permits system fallback.
ResolvedProxyPolicy ResolveProxyPolicy(const ServiceConnectionSettings& settings, const std::string& destinationURL);
bool ValidateProxyURL(const std::string& proxyURL, std::string& error);
// Configures policy only. The caller owns URL, transfer deadlines and ordered route retries.
bool ConfigureCurl(CURL* curl, const std::string& destinationURL, const ProxyRoute& route, std::string& error);
bool SaveProxyCredentials(const std::string& proxyURL, const std::string& username,
	const std::string& password, std::string& error);
bool DeleteProxyCredentials(const std::string& proxyURL, std::string& error);
bool HasProxyCredentials(const std::string& proxyURL);
// Login kept in memory for this session only; never written to disk
void SetSessionProxyCredentials(const std::string& proxyURL, const std::string& username, const std::string& password);
void ClearSessionProxyCredentials(const std::string& proxyURL);
// Logins are bound to one proxy: nothing typed for another proxy is ever offered to this one
void ClearSessionProxyCredentialsExcept(const std::string& keepProxyURL);
// The key a proxy's login is stored under: scheme://host:port in lower case with the default port filled in, empty for an invalid address
std::string CanonicalProxyKey(const std::string& proxyURL);
// User name of the session or saved login, empty when there is none
std::string ProxyUsername(const std::string& proxyURL);
// host:port of a proxy address for display
std::string ProxyDisplayName(const std::string& proxyURL);

struct ConnectionTestResult
{
	enum class Outcome { Connected, ProxyAuthRequired, ServiceRejected, Unreachable, Failed };
	Outcome outcome = Outcome::Failed;
	std::string route;
	std::string proxyURL;
	std::string detail;
	bool proxyAnswered = false;
	bool proxyIntegratedAuthPossible = false;
};
// Worker-only: probes the anonymous VersionManifest request with the given settings
ConnectionTestResult TestServiceConnection(const ServiceConnectionSettings& settings, const std::string& environment, int maxSeconds = 15);
std::string DescribeConnectionTest(const ConnectionTestResult& result);
const char* ProtocolName(long version);
const char* FailurePhase(CURLcode code, long status, long proxyStatus);
}