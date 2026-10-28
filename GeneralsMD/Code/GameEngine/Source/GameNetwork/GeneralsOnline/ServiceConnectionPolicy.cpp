#include "GameNetwork/GeneralsOnline/ServiceConnectionPolicy.h"
#include "GameNetwork/GeneralsOnline/CredentialStore.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <thread>
#include <deque>
#include <mutex>
#include <map>
#include <chrono>

#pragma comment(lib, "winhttp.lib")

namespace ServiceConnection
{
namespace
{
// WinHTTP's proxy schemes; the precompiled header includes wininet.h, whose INTERNET_SCHEME_* values differ
constexpr INTERNET_SCHEME WinHttpSchemeHttp = (INTERNET_SCHEME)1;
constexpr INTERNET_SCHEME WinHttpSchemeHttps = (INTERNET_SCHEME)2;

std::string Lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return value;
}

std::wstring Wide(const std::string& value)
{
	if (value.empty())
		return {};
	int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0);
	if (!size)
		return {};
	std::wstring result(size, L'\0');
	MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), (int)value.size(), &result[0], size);
	return result;
}

std::string UTF8(const std::wstring& value)
{
	if (value.empty())
		return {};
	int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), nullptr, 0, nullptr, nullptr);
	if (!size)
		return {};
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), (int)value.size(), &result[0], size, nullptr, nullptr);
	return result;
}

std::string Part(CURLU* url, CURLUPart part, unsigned flags = 0)
{
	char* value = nullptr;
	if (curl_url_get(url, part, &value, flags) != CURLUE_OK)
		return {};
	std::string result(value);
	curl_free(value);
	return result;
}

bool CanonicalProxy(const std::string& typed, std::string& canonical, std::string& error)
{
	error.clear();
	// host:port without a scheme means an http proxy, so it maps to the same key as http://host:port
	const std::string input = typed.find("://") == std::string::npos && !typed.empty() ? "http://" + typed : typed;
	CURLU* url = curl_url();
	if (!url)
	{
		error = "Unable to allocate proxy URL parser.";
		return false;
	}
	bool valid = input.find_first_of("\r\n\t ") == std::string::npos &&
		curl_url_set(url, CURLUPART_URL, input.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK;
	const auto scheme = Lower(Part(url, CURLUPART_SCHEME));
	const auto host = Lower(Part(url, CURLUPART_HOST));
	auto port = Part(url, CURLUPART_PORT);
	const auto path = Part(url, CURLUPART_PATH);
	const auto zone = Part(url, CURLUPART_ZONEID);
	valid = valid && (scheme == "http" || scheme == "https" || scheme == "socks5h") && !host.empty() && zone.empty() && port != "0" &&
		input.find('@') == std::string::npos && Part(url, CURLUPART_USER).empty() && Part(url, CURLUPART_PASSWORD).empty() &&
		input.find('?') == std::string::npos && input.find('#') == std::string::npos && (path.empty() || path == "/");
	if (valid)
	{
		if (port.empty())
			port = scheme == "https" ? "443" : scheme == "http" ? "80" : "1080";
		canonical = scheme + "://" + host + ":" + port;
	}
	else
		error = "Enter an http://, https:// or socks5h:// proxy address without credentials, path or query.";
	curl_url_cleanup(url);
	return valid;
}


struct SessionLogin
{
	std::string user;
	std::string secret;
};

std::mutex& SessionLoginMutex()
{
	static std::mutex mutex;
	return mutex;
}

std::map<std::string, SessionLogin>& SessionLogins()
{
	static std::map<std::string, SessionLogin> logins;
	return logins;
}

void WipeSessionLogin(SessionLogin& login)
{
	if (!login.secret.empty())
		SecureZeroMemory(&login.secret[0], login.secret.size());
	login.secret.clear();
}

bool FindSessionLogin(const std::string& canonical, SessionLogin& login)
{
	std::lock_guard<std::mutex> lock(SessionLoginMutex());
	const auto found = SessionLogins().find(canonical);
	if (found == SessionLogins().end())
		return false;
	login = found->second;
	return true;
}

bool Wildcard(const std::string& pattern, const std::string& value)
{
	size_t p = 0, v = 0, star = std::string::npos, retry = 0;
	while (v < value.size())
	{
		if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v]))
		{
			++p;
			++v;
		}
		else if (p < pattern.size() && pattern[p] == '*')
		{
			star = p++;
			retry = v;
		}
		else if (star != std::string::npos)
		{
			p = star + 1;
			v = ++retry;
		}
		else
			return false;
	}
	while (p < pattern.size() && pattern[p] == '*')
		++p;
	return p == pattern.size();
}

std::vector<std::string> Tokens(std::string input)
{
	std::replace(input.begin(), input.end(), ';', ' ');
	std::istringstream stream(input);
	std::vector<std::string> result;
	for (std::string token; stream >> token;)
		result.push_back(token);
	return result;
}

bool StaticProxy(const std::wstring& proxy, const std::wstring& bypass, const std::string& destination,
	ResolvedProxyPolicy& result)
{
	CURLU* url = curl_url();
	if (!url || curl_url_set(url, CURLUPART_URL, destination.c_str(), 0) != CURLUE_OK)
	{
		if (url)
			curl_url_cleanup(url);
		result.error = "Invalid proxy lookup destination.";
		return false;
	}
	const auto host = Lower(Part(url, CURLUPART_HOST));
	const auto scheme = Lower(Part(url, CURLUPART_SCHEME));
	const auto port = Part(url, CURLUPART_PORT, CURLU_DEFAULT_PORT);
	curl_url_cleanup(url);
	for (auto token : Tokens(Lower(UTF8(bypass))))
	{
		if ((token == "<local>" && host.find('.') == std::string::npos && host.find(':') == std::string::npos) ||
			Wildcard(token, host) || Wildcard(token, host + ":" + port))
		{
			result.routes.push_back({});
			return true;
		}
	}
	bool specific = false;
	bool socksFallback = false;
	const auto entries = Tokens(UTF8(proxy));
	if (entries.empty())
	{
		result.error = "Windows has an empty static proxy list.";
		return false;
	}
	for (const auto& entry : entries)
	{
		const auto equal = entry.find('=');
		if (equal == std::string::npos)
			continue;
		const auto protocol = Lower(entry.substr(0, equal));
		if (protocol == scheme)
			specific = true;
		if (protocol == "socks")
			socksFallback = true;
	}
	for (auto entry : entries)
	{
		const auto equal = entry.find('=');
		if (equal != std::string::npos)
		{
			if (Lower(entry.substr(0, equal)) != scheme)
				continue;
			entry.erase(0, equal + 1);
		}
		else if (specific)
			continue;
		if (entry.find("://") == std::string::npos)
			entry = "http://" + entry;
		std::string canonical;
		if (!CanonicalProxy(entry, canonical, result.error))
		{
			result.routes.clear();
			return false;
		}
		result.routes.push_back({canonical, false, canonical.find("socks5h://") != 0});
	}
	// A protocol-specific system configuration without this protocol explicitly leaves it direct.
	if (result.routes.empty())
	{
		if (socksFallback)
		{
			result.error = "Windows SOCKS fallback is unsupported. Select an explicit supported proxy.";
			return false;
		}
		result.routes.push_back({});
	}
	return true;
}

struct ResolverContext
{
	std::atomic<unsigned> references{1};
	std::atomic<DWORD> error{ERROR_IO_PENDING};
	HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	HANDLE closing = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	void Release()
	{
		if (references.fetch_sub(1) == 1)
		{
			if (completed)
				CloseHandle(completed);
			if (closing)
				CloseHandle(closing);
			delete this;
		}
	}
};

void CALLBACK ProxyCallback(HINTERNET, DWORD_PTR value, DWORD status, void* information, DWORD)
{
	auto* context = reinterpret_cast<ResolverContext*>(value);
	if (!context)
		return;
	if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
	{
		// signal before releasing: this reference keeps the event alive
		if (context->closing)
			SetEvent(context->closing);
		context->Release();
		return;
	}
	if (status == WINHTTP_CALLBACK_STATUS_GETPROXYFORURL_COMPLETE || status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR)
	{
		DWORD error = status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR ?
			static_cast<WINHTTP_ASYNC_RESULT*>(information)->dwError : ERROR_SUCCESS;
		context->error.store(error, std::memory_order_release);
		SetEvent(context->completed);
	}
}

// websocket lookups use the matching http scheme
std::string ProxyDestination(std::string destination)
{
	if (destination.compare(0, 6, "wss://") == 0)
		destination.replace(0, 6, "https://");
	else if (destination.compare(0, 5, "ws://") == 0)
		destination.replace(0, 5, "http://");
	return destination;
}

ResolvedProxyPolicy AutoProxy(const std::wstring& destination, WINHTTP_AUTOPROXY_OPTIONS& options)
{
	ResolvedProxyPolicy result;
	HINTERNET session = WinHttpOpen(L"GeneralsOnline", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, WINHTTP_FLAG_ASYNC);
	if (!session)
	{
		result.nativeError = GetLastError();
		result.failurePhase = "proxy-discovery";
		result.error = "Unable to initialize Windows proxy resolution.";
		return result;
	}
	WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
	HINTERNET resolver = nullptr;
	DWORD status = ERROR_SUCCESS;
	if (WinHttpSetStatusCallback(session, ProxyCallback, WINHTTP_CALLBACK_FLAG_REQUEST_ERROR |
		WINHTTP_CALLBACK_FLAG_GETPROXYFORURL_COMPLETE | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
		status = GetLastError();
	if (!status)
		status = WinHttpCreateProxyResolver(session, &resolver);
	auto* context = new ResolverContext;
	bool callbackOwnsReference = false;
	if (!context->completed || !context->closing)
		status = GetLastError();
	if (!status)
	{
		DWORD_PTR value = reinterpret_cast<DWORD_PTR>(context);
		if (!WinHttpSetOption(resolver, WINHTTP_OPTION_CONTEXT_VALUE, &value, sizeof(value)))
			status = GetLastError();
		else
		{
			++context->references;
			callbackOwnsReference = true;
		}
	}
	if (!status)
	{
		status = WinHttpGetProxyForUrlEx(resolver, destination.c_str(), &options, reinterpret_cast<DWORD_PTR>(context));
		if (status == ERROR_IO_PENDING)
		{
			const DWORD wait = WaitForSingleObject(context->completed, 5000);
			status = wait == WAIT_OBJECT_0 ? context->error.load(std::memory_order_acquire) : ERROR_TIMEOUT;
		}
	}
	if (!status)
	{
		WINHTTP_PROXY_RESULT proxies{};
		status = WinHttpGetProxyResult(resolver, &proxies);
		if (!status)
		{
			for (DWORD i = 0; i < proxies.cEntries; ++i)
			{
				const auto& entry = proxies.pEntries[i];
				if (!entry.fProxy)
				{
					result.routes.push_back({});
					continue;
				}
				std::string scheme;
				if (entry.ProxyScheme == WinHttpSchemeHttp)
					scheme = "http";
				else if (entry.ProxyScheme == WinHttpSchemeHttps)
					scheme = "https";
				else
				{
					result.error = "Windows returned an unsupported proxy protocol.";
					break;
				}
				std::string host = entry.pwszProxy ? UTF8(entry.pwszProxy) : std::string();
				if (host.find(':') != std::string::npos && host.front() != '[')
					host = "[" + host + "]";
				std::string canonical;
				if (!CanonicalProxy(scheme + "://" + host + ":" + std::to_string(entry.ProxyPort), canonical, result.error))
					break;
				result.routes.push_back({canonical, false, canonical.find("socks5h://") != 0});
			}
			WinHttpFreeProxyResult(&proxies);
		}
	}
	if (resolver)
		WinHttpCloseHandle(resolver);
	// the resolver's final callback must run while its session is still open
	if (callbackOwnsReference)
		WaitForSingleObject(context->closing, 5000);
	WinHttpCloseHandle(session);
	// the callback owns the second reference, so a late or missed callback never frees a live context
	context->Release();
	result.nativeError = status;
	if (status)
		result.error = "Windows proxy resolution failed (" + std::to_string(status) + "). Check system proxy settings.";
	if (result.routes.empty() && result.error.empty())
		result.error = "Windows returned no permitted proxy route.";
	if (!result.error.empty())
	{
		result.routes.clear();
		result.failurePhase = status == ERROR_TIMEOUT || status == ERROR_WINHTTP_TIMEOUT ? "proxy-timeout" :
			(options.dwFlags & WINHTTP_AUTOPROXY_CONFIG_URL) ? "proxy-pac" : "proxy-discovery";
	}
	return result;
}

// the first route for display
std::string SourceOfRoutes(const ResolvedProxyPolicy& result)
{
	return result.routes.empty() || result.routes.front().direct ? std::string("Direct") : ProxyDisplayName(result.routes.front().url);
}

ResolvedProxyPolicy ApplyOptionalWpadFallback(ResolvedProxyPolicy result, bool autoDetect, bool explicitPac,
	const std::wstring& proxy, const std::wstring& bypass, const std::string& destination)
{
	// No discovered WPAD script is different from failing to load or execute a configured script.
	if (!autoDetect || explicitPac || result.nativeError != ERROR_WINHTTP_AUTODETECTION_FAILED || result.error.empty())
		return result;
	result.routes.clear();
	result.error.clear();
	result.failurePhase.clear();
	if (!proxy.empty())
	{
		if (!StaticProxy(proxy, bypass, destination, result))
		{
			result.failurePhase = "proxy-configuration";
			return result;
		}
		result.fallbackReason = "No WPAD configuration found; using the system static proxy policy.";
	}
	else
	{
		result.routes.push_back({});
		result.fallbackReason = "No WPAD configuration or static proxy found; using direct access.";
	}
	return result;
}
}

bool ValidateProxyURL(const std::string& proxyURL, std::string& error)
{
	std::string canonical;
	return CanonicalProxy(proxyURL, canonical, error);
}

namespace
{
ResolvedProxyPolicy ResolveProxyRoutes(const ServiceConnectionSettings& settings, const std::string& destinationURL)
{
	ResolvedProxyPolicy result;
	if (settings.proxyMode == "disabled")
	{
		result.routes.push_back({});
		result.source = "Direct";
		return result;
	}
	if (settings.proxyMode == "custom")
	{
		std::string canonical;
		if (CanonicalProxy(settings.proxyURL, canonical, result.error))
			result.routes.push_back({canonical, false, canonical.find("socks5h://") != 0});
		else
			result.failurePhase = "proxy-configuration";
		return result;
	}
	if (settings.proxyMode != "automatic")
	{
		result.error = "Unknown proxy mode.";
		result.failurePhase = "proxy-configuration";
		return result;
	}
	const std::string destination = ProxyDestination(destinationURL);
	WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
	if (!WinHttpGetIEProxyConfigForCurrentUser(&config))
	{
		result.nativeError = GetLastError();
		result.failurePhase = "proxy-settings";
		result.error = "Unable to read Windows proxy settings (" + std::to_string(result.nativeError) + ").";
		return result;
	}
	const std::wstring proxy = config.lpszProxy ? config.lpszProxy : L"";
	const std::wstring bypass = config.lpszProxyBypass ? config.lpszProxyBypass : L"";
	const std::wstring pac = config.lpszAutoConfigUrl ? config.lpszAutoConfigUrl : L"";
	const bool autoDetect = config.fAutoDetect != FALSE;
	GlobalFree(config.lpszProxy); GlobalFree(config.lpszProxyBypass); GlobalFree(config.lpszAutoConfigUrl);
	if (!pac.empty() || autoDetect)
	{
		WINHTTP_AUTOPROXY_OPTIONS options{};
		if (!pac.empty())
		{
			options.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
			options.lpszAutoConfigUrl = pac.c_str();
		}
		if (autoDetect)
		{
			options.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
			options.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
		}
		options.fAutoLogonIfChallenged = FALSE;
		result = ApplyOptionalWpadFallback(AutoProxy(Wide(destination), options), autoDetect, !pac.empty(),
			proxy, bypass, destination);
		result.source = !result.fallbackReason.empty() ? SourceOfRoutes(result) : !pac.empty() ? "PAC: " + UTF8(pac) : "Auto-detect";
		return result;
	}
	if (!proxy.empty())
	{
		if (!StaticProxy(proxy, bypass, destination, result))
			result.failurePhase = "proxy-configuration";
		result.source = SourceOfRoutes(result);
	}
	else
	{
		result.routes.push_back({});
		result.source = "Direct";
	}
	return result;
}
}

ResolvedProxyPolicy ResolveProxyPolicy(const ServiceConnectionSettings& settings, const std::string& destinationURL)
{
	ResolvedProxyPolicy result = ResolveProxyRoutes(settings, destinationURL);
	for (auto& route : result.routes)
	{
		route.integratedAuthPossible = route.allowIntegratedAuth;
		// the Windows login is only tried when the player wants it
		if (!settings.windowsLogin)
			route.allowIntegratedAuth = false;
	}
	return result;
}

namespace
{
using ProxyClock = std::chrono::steady_clock;
constexpr size_t MaxProxyJobs = 64;
constexpr size_t MaxProxyWaiters = 64;
constexpr size_t MaxProxyCache = 64;
constexpr auto ProxyCacheTTL = std::chrono::seconds(60);
constexpr auto ProxyFailureTTL = std::chrono::seconds(30);

void KeyPart(std::string& key, const std::string& value)
{
	key += std::to_string(value.size()) + ":" + value;
}

// Failures are shared per PAC server, not per URL; empty when no script is fetched
std::string PolicySource(const std::string& pacURL, bool autoDetect)
{
	if (pacURL.empty())
	{
		return autoDetect ? "wpad" : std::string();
	}
	const auto schemeEnd = pacURL.find("://");
	const auto start = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
	return "pac:" + Lower(pacURL.substr(0, pacURL.find_first_of("/?#", start)));
}

bool IsCacheableProxyFailure(const ResolvedProxyPolicy& result)
{
	return !result.error.empty() && (result.failurePhase == "proxy-timeout" ||
		result.failurePhase == "proxy-pac" || result.failurePhase == "proxy-discovery");
}

// In-memory only: PAC decisions can depend on the full path and query
bool ProxyKey(const ServiceConnectionSettings& settings, const std::string& destination,
	std::string& key, ResolvedProxyPolicy& failure, std::string* systemFingerprint = nullptr,
	std::string* failureSource = nullptr)
{
	KeyPart(key, ProxyDestination(destination));
	KeyPart(key, settings.proxyMode);
	KeyPart(key, settings.proxyURL);
	key += settings.windowsLogin ? "1" : "0";
	WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
	if (!WinHttpGetIEProxyConfigForCurrentUser(&config))
	{
		failure.nativeError = GetLastError();
		failure.failurePhase = "proxy-settings";
		failure.error = "Unable to read Windows proxy settings (" + std::to_string(failure.nativeError) + ").";
		return false;
	}
	const auto systemStart = key.size();
	key += config.fAutoDetect ? "1" : "0";
	KeyPart(key, config.lpszProxy ? UTF8(config.lpszProxy) : "");
	KeyPart(key, config.lpszProxyBypass ? UTF8(config.lpszProxyBypass) : "");
	KeyPart(key, config.lpszAutoConfigUrl ? UTF8(config.lpszAutoConfigUrl) : "");
	if (systemFingerprint)
	{
		*systemFingerprint = key.substr(systemStart);
	}
	if (failureSource)
	{
		*failureSource = PolicySource(config.lpszAutoConfigUrl ? UTF8(config.lpszAutoConfigUrl) : "", config.fAutoDetect != FALSE);
	}
	GlobalFree(config.lpszProxy);
	GlobalFree(config.lpszProxyBypass);
	GlobalFree(config.lpszAutoConfigUrl);
	return true;
}

struct ProxyWork
{
	ServiceConnectionSettings settings;
	std::string destination;
	std::string key;
	std::string failureKey;
	unsigned long long generation = 0;
	std::vector<std::weak_ptr<ProxyResolution>> waiters;
};
struct ProxyCacheEntry
{
	ResolvedProxyPolicy result;
	ProxyClock::time_point expires;
};
struct ProxyQueue
{
	std::mutex mutex;
	std::deque<std::shared_ptr<ProxyWork>> pending;
	std::map<std::string, std::shared_ptr<ProxyWork>> active;
	std::map<std::string, ProxyCacheEntry> cache;
	std::map<std::string, ProxyCacheEntry> failures;
	std::string lastSystemKey;
	unsigned long long generation = 0;
	unsigned workers = 0;
};
std::shared_ptr<ProxyQueue> ProxyQueueState()
{
	static auto queue = std::make_shared<ProxyQueue>();
	return queue;
}
void CompleteProxy(ProxyResolution& state, const ResolvedProxyPolicy& result)
{
	state.result = result;
	state.ready.store(true, std::memory_order_release);
}
void ProxyQueueFailure(ProxyResolution& state, const char* message)
{
	ResolvedProxyPolicy result;
	result.error = message;
	result.failurePhase = "proxy-policy";
	CompleteProxy(state, result);
}
void PruneProxyEntries(std::map<std::string, ProxyCacheEntry>& entries, ProxyClock::time_point now)
{
	for (auto it = entries.begin(); it != entries.end();)
	{
		if (it->second.expires <= now)
		{
			it = entries.erase(it);
		}
		else
		{
			++it;
		}
	}
}
void PruneProxyCache(ProxyQueue& queue, ProxyClock::time_point now)
{
	PruneProxyEntries(queue.cache, now);
	PruneProxyEntries(queue.failures, now);
}
void FinishProxyWork(ProxyQueue& queue, const std::shared_ptr<ProxyWork>& work,
	const ResolvedProxyPolicy& result, ProxyClock::time_point now)
{
	if (work->generation == queue.generation && result.error.empty() && !result.routes.empty())
	{
		PruneProxyCache(queue, now);
		if (queue.cache.size() >= MaxProxyCache)
		{
			queue.cache.erase(queue.cache.begin());
		}
		queue.cache[work->key] = {result, now + ProxyCacheTTL};
	}
	else if (work->generation == queue.generation && !work->failureKey.empty() && IsCacheableProxyFailure(result))
	{
		PruneProxyCache(queue, now);
		if (queue.failures.size() >= MaxProxyCache)
		{
			queue.failures.erase(queue.failures.begin());
		}
		queue.failures[work->failureKey] = {result, now + ProxyFailureTTL};
	}
	for (auto& waiter : work->waiters)
	{
		if (auto state = waiter.lock())
		{
			CompleteProxy(*state, result);
		}
	}
	const auto active = queue.active.find(work->key);
	if (active != queue.active.end() && active->second == work)
	{
		queue.active.erase(active);
	}
}
std::shared_ptr<ProxyWork> QueueProxyWork(ProxyQueue& queue, const ServiceConnectionSettings& settings,
	const std::string& destinationURL, const std::string& key, const std::shared_ptr<ProxyResolution>& state,
	const std::string& failureKey = std::string(), ProxyClock::time_point now = ProxyClock::now())
{
	const auto cached = queue.cache.find(key);
	if (cached != queue.cache.end())
	{
		CompleteProxy(*state, cached->second.result);
		return {};
	}
	// a dead PAC server is probed once per TTL, not once per URL
	const auto failed = failureKey.empty() ? queue.failures.end() : queue.failures.find(failureKey);
	if (failed != queue.failures.end() && failed->second.expires > now)
	{
		CompleteProxy(*state, failed->second.result);
		return {};
	}
	const auto active = queue.active.find(key);
	if (active != queue.active.end())
	{
		auto& waiters = active->second->waiters;
		waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
			[](const std::weak_ptr<ProxyResolution>& waiter) { return waiter.expired(); }), waiters.end());
		if (waiters.size() >= MaxProxyWaiters)
		{
			ProxyQueueFailure(*state, "Too many pending proxy lookups. Try again shortly.");
		}
		else
		{
			waiters.push_back(state);
		}
		return {};
	}
	if (queue.pending.size() + queue.workers >= MaxProxyJobs)
	{
		ProxyQueueFailure(*state, "Too many pending proxy lookups. Try again shortly.");
		return {};
	}
	auto work = std::make_shared<ProxyWork>();
	work->settings = settings;
	work->destination = destinationURL;
	work->key = key;
	work->failureKey = failureKey;
	work->generation = queue.generation;
	work->waiters.push_back(state);
	queue.active[work->key] = work;
	queue.pending.push_back(work);
	return work;
}

}

void InvalidateProxyPolicyCache()
{
	auto queue = ProxyQueueState();
	std::lock_guard<std::mutex> lock(queue->mutex);
	++queue->generation;
	queue->cache.clear();
	queue->failures.clear();
	// callers already waiting keep their lookup, new callers start a fresh one
	queue->active.clear();
}

bool ShouldLogProxyPolicy(bool websocket, const ResolvedProxyPolicy& result)
{
	static std::mutex mutex;
	static std::string previous[2];
	std::string signature;
	KeyPart(signature, result.fallbackReason);
	for (const auto& route : result.routes)
	{
		signature += route.direct ? "D" : "P";
		KeyPart(signature, route.url);
		signature += route.allowIntegratedAuth ? "1" : "0";
	}
	std::lock_guard<std::mutex> lock(mutex);
	auto& last = previous[websocket ? 1 : 0];
	if (last == signature)
	{
		return false;
	}
	last = std::move(signature);
	return true;
}

std::shared_ptr<ProxyResolution> StartProxyResolution(ServiceConnectionSettings settings, std::string destinationURL)
{
	auto state = std::make_shared<ProxyResolution>();
	if (destinationURL.size() > 16384 || settings.proxyURL.size() > 2048)
	{
		ProxyQueueFailure(*state, "Proxy lookup configuration is too long.");
		return state;
	}
	// Explicit direct/custom policy needs neither discovery nor a background worker
	if (settings.proxyMode != "automatic")
	{
		CompleteProxy(*state, ResolveProxyPolicy(settings, destinationURL));
		return state;
	}
	std::string key, systemFingerprint, failureSource;
	ResolvedProxyPolicy failure;
	if (!ProxyKey(settings, destinationURL, key, failure, &systemFingerprint, &failureSource))
	{
		CompleteProxy(*state, failure);
		return state;
	}
	auto queue = ProxyQueueState();
	std::lock_guard<std::mutex> lock(queue->mutex);
	if (queue->lastSystemKey != systemFingerprint)
	{
		++queue->generation;
		queue->cache.clear();
		queue->failures.clear();
		queue->active.clear();
		queue->lastSystemKey = std::move(systemFingerprint);
	}
	const auto now = ProxyClock::now();
	PruneProxyCache(*queue, now);
	auto work = QueueProxyWork(*queue, settings, destinationURL, key, state, failureSource, now);
	if (!work)
	{
		return state;
	}
	if (queue->workers >= 4)
	{
		return state;
	}
	++queue->workers;
	try
	{
		std::thread([queue]() {
			for (;;)
			{
				std::shared_ptr<ProxyWork> work;
				{
					std::lock_guard<std::mutex> lock(queue->mutex);
					if (queue->pending.empty())
					{
						--queue->workers;
						return;
					}
					work = std::move(queue->pending.front());
					queue->pending.pop_front();
					const bool canceled = std::all_of(work->waiters.begin(), work->waiters.end(),
						[](const std::weak_ptr<ProxyResolution>& waiter) { return waiter.expired(); });
					if (canceled)
					{
						const auto active = queue->active.find(work->key);
						if (active != queue->active.end() && active->second == work)
						{
							queue->active.erase(active);
						}
						continue;
					}
				}
				ResolvedProxyPolicy result;
				try
				{
					result = ResolveProxyPolicy(work->settings, work->destination);
					std::string currentKey;
					ResolvedProxyPolicy changed;
					if (!ProxyKey(work->settings, work->destination, currentKey, changed))
					{
						result = std::move(changed);
					}
					else if (currentKey != work->key)
					{
						result = {};
						result.error = "Windows proxy settings changed during lookup. Reconnect to retry.";
						result.failurePhase = "proxy-settings";
					}
				}
				catch (...)
				{
					result.error = "Proxy resolution failed.";
					result.failurePhase = "proxy-policy";
				}
				std::lock_guard<std::mutex> lock(queue->mutex);
				FinishProxyWork(*queue, work, result, ProxyClock::now());
			}
		}).detach();
	}
	catch (...)
	{
		--queue->workers;
		queue->pending.pop_back();
		queue->active.erase(work->key);
		ProxyQueueFailure(*state, "Unable to start proxy resolution.");
	}
	return state;
}

bool SaveProxyCredentials(const std::string& proxyURL, const std::string& username,
	const std::string& password, std::string& error)
{
	std::string canonical;
	if (!CanonicalProxy(proxyURL, canonical, error))
		return false;
	if (username.empty() || username.find('\0') != std::string::npos || password.find('\0') != std::string::npos)
	{
		error = "Proxy credentials are invalid.";
		return false;
	}
	if (!SaveProxyLogin(canonical, username, password))
	{
		error = "Unable to save proxy credentials. They are used until the game closes.";
		return false;
	}
	return true;
}

void SetSessionProxyCredentials(const std::string& proxyURL, const std::string& username, const std::string& password)
{
	std::string canonical, error;
	if (!CanonicalProxy(proxyURL, canonical, error))
		return;
	std::lock_guard<std::mutex> lock(SessionLoginMutex());
	auto& login = SessionLogins()[canonical];
	WipeSessionLogin(login);
	login.user = username;
	login.secret = password;
}

void ClearSessionProxyCredentials(const std::string& proxyURL)
{
	std::string canonical, error;
	if (!CanonicalProxy(proxyURL, canonical, error))
		return;
	std::lock_guard<std::mutex> lock(SessionLoginMutex());
	const auto found = SessionLogins().find(canonical);
	if (found == SessionLogins().end())
		return;
	WipeSessionLogin(found->second);
	SessionLogins().erase(found);
}

void ClearSessionProxyCredentialsExcept(const std::string& keepProxyURL)
{
	std::string keep, error;
	CanonicalProxy(keepProxyURL, keep, error);
	std::lock_guard<std::mutex> lock(SessionLoginMutex());
	for (auto it = SessionLogins().begin(); it != SessionLogins().end();)
	{
		if (it->first == keep)
		{
			++it;
			continue;
		}
		WipeSessionLogin(it->second);
		it = SessionLogins().erase(it);
	}
}

std::string CanonicalProxyKey(const std::string& proxyURL)
{
	std::string canonical, error;
	return CanonicalProxy(proxyURL, canonical, error) ? canonical : std::string();
}

std::string ProxyUsername(const std::string& proxyURL)
{
	std::string canonical, error;
	if (!CanonicalProxy(proxyURL, canonical, error))
		return {};
	SessionLogin session;
	if (FindSessionLogin(canonical, session))
	{
		WipeSessionLogin(session);
		return session.user;
	}
	std::string user, password;
	const bool found = LoadProxyLogin(canonical, user, password);
	if (!password.empty())
		SecureZeroMemory(&password[0], password.size());
	return found ? user : std::string();
}

std::string ProxyDisplayName(const std::string& proxyURL)
{
	const auto schemeEnd = proxyURL.find("://");
	return schemeEnd == std::string::npos ? proxyURL : proxyURL.substr(schemeEnd + 3);
}

bool HasProxyCredentials(const std::string& proxyURL)
{
	std::string canonical, error;
	std::string user, password;
	const bool found = CanonicalProxy(proxyURL, canonical, error) && LoadProxyLogin(canonical, user, password);
	if (!password.empty())
		SecureZeroMemory(&password[0], password.size());
	return found;
}

bool DeleteProxyCredentials(const std::string& proxyURL, std::string& error)
{
	std::string canonical;
	if (!CanonicalProxy(proxyURL, canonical, error))
		return false;
	if (DeleteProxyLogin(canonical))
		return true;
	error = "Unable to remove proxy credentials.";
	return false;
}

bool ConfigureCurl(CURL* curl, const std::string& destinationURL, const ProxyRoute& route, std::string& error)
{
	if (!ConfigureServiceTransport(curl, destinationURL, error))
		return false;
	const bool websocket = destinationURL.compare(0, 6, "wss://") == 0;
	auto set = [&](CURLoption option, auto value) {
		CURLcode code = curl_easy_setopt(curl, option, value);
		if (code != CURLE_OK && error.empty())
			error = "Unable to configure service transport: " + std::string(curl_easy_strerror(code));
		return code == CURLE_OK;
	};
	const auto* version = curl_version_info(CURLVERSION_NOW);
	const long protocol = websocket ? CURL_HTTP_VERSION_1_1 :
		route.direct && (version->features & CURL_VERSION_HTTP3) ? CURL_HTTP_VERSION_3 :
		(version->features & CURL_VERSION_HTTP2) ? CURL_HTTP_VERSION_2TLS : CURL_HTTP_VERSION_1_1;
	if (!set(CURLOPT_HTTP_VERSION, protocol) || !set(CURLOPT_PROXY_SSL_VERIFYPEER, 1L) || !set(CURLOPT_PROXY_SSL_VERIFYHOST, 2L) ||
		!set(CURLOPT_PROXY_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA) || !set(CURLOPT_SOCKS5_AUTH, (long)CURLAUTH_BASIC) ||
		!set(CURLOPT_HEADEROPT, (long)CURLHEADER_SEPARATE) || !set(CURLOPT_NOPROXY, "") || !set(CURLOPT_PROXYAUTH, (long)CURLAUTH_NONE))
		return false;
	if (route.direct)
		return true;
	std::string canonical;
	if (!CanonicalProxy(route.url, canonical, error))
		return false;
	if (!set(CURLOPT_PROXY, canonical.c_str()))
		return false;
	if (route.allowIntegratedAuth)
	{
		if (!(version->features & CURL_VERSION_SSPI))
		{
			error = "This build does not support Windows proxy authentication.";
			return false;
		}
		return set(CURLOPT_PROXYAUTH, (long)CURLAUTH_NEGOTIATE) && set(CURLOPT_PROXYUSERPWD, ":");
	}
	// otherwise a login typed for this session wins over the saved one
	SessionLogin session;
	if (FindSessionLogin(canonical, session))
	{
		const bool success = set(CURLOPT_PROXYUSERNAME, session.user.c_str()) && set(CURLOPT_PROXYPASSWORD, session.secret.c_str()) &&
			set(CURLOPT_PROXYAUTH, (long)(CURLAUTH_BASIC | CURLAUTH_DIGEST | CURLAUTH_NEGOTIATE));
		WipeSessionLogin(session);
		return success;
	}
	std::string user, secret;
	if (LoadProxyLogin(canonical, user, secret))
	{
		const bool success = set(CURLOPT_PROXYUSERNAME, user.c_str()) && set(CURLOPT_PROXYPASSWORD, secret.c_str()) &&
			set(CURLOPT_PROXYAUTH, (long)(CURLAUTH_BASIC | CURLAUTH_DIGEST | CURLAUTH_NEGOTIATE));
		if (!secret.empty())
			SecureZeroMemory(&secret[0], secret.size());
		return success;
	}
	return true;
}

const char* ProtocolName(long version)
{
	switch (version)
	{
		case CURL_HTTP_VERSION_1_0: return "HTTP/1.0";
		case CURL_HTTP_VERSION_1_1: return "HTTP/1.1";
		case CURL_HTTP_VERSION_2_0: return "HTTP/2";
		case CURL_HTTP_VERSION_3: return "HTTP/3";
		default: return "unknown";
	}
}

const char* FailurePhase(CURLcode code, long status, long proxyStatus)
{
	if (proxyStatus == 407 || status == 407)
		return "proxy authentication";
	if (code == CURLE_COULDNT_RESOLVE_PROXY)
		return "proxy DNS";
	if (code == CURLE_COULDNT_RESOLVE_HOST)
		return "endpoint DNS";
	if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR ||
		code == CURLE_SSL_CACERT_BADFILE || code == CURLE_SSL_CERTPROBLEM)
		return "TLS";
	if (code == CURLE_COULDNT_CONNECT)
		return "connection";
	if (code == CURLE_OPERATION_TIMEDOUT)
		return "timeout";
	if (status == 401 || status == 403)
		return "service authentication";
	if (code != CURLE_OK)
		return "transport";
	if (status >= 400)
		return "service response";
	return "none";
}

namespace
{
size_t ReceiveProbe(void* data, size_t size, size_t count, void* target)
{
	if (size && count > SIZE_MAX / size)
		return 0;
	const size_t bytes = size * count;
	auto& body = *static_cast<std::string*>(target);
	if (bytes > 4096 || body.size() > 4096 - bytes)
		return 0;
	body.append(static_cast<const char*>(data), bytes);
	return bytes;
}
}

ConnectionTestResult TestServiceConnection(const ServiceConnectionSettings& settings, const std::string& environment, int maxSeconds)
{
	using Outcome = ConnectionTestResult::Outcome;
	ConnectionTestResult result;
	result.outcome = Outcome::Unreachable;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(maxSeconds);
	for (const auto& base : BootstrapRoutes(environment, settings, ""))
	{
		const std::string url = base + "VersionManifest";
		const std::string routeName = RouteHost(base);
		const auto policy = ResolveProxyPolicy(settings, url);
		if (!policy.error.empty())
		{
			result.outcome = Outcome::Failed;
			result.detail = policy.error;
			return result;
		}
		for (const auto& route : policy.routes)
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return result;
			std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
			std::string error, body;
			if (!ConfigureCurl(curl.get(), url, route, error))
			{
				result.outcome = Outcome::Failed;
				result.detail = error;
				return result;
			}
			if (curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()) != CURLE_OK ||
				curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 4000L) != CURLE_OK ||
				curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 6000L) != CURLE_OK ||
				curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, ReceiveProbe) != CURLE_OK ||
				curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body) != CURLE_OK)
			{
				result.outcome = Outcome::Failed;
				result.detail = "Could not set up the test.";
				return result;
			}
			const auto code = curl_easy_perform(curl.get());
			long status = 0, proxyStatus = 0;
			curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
			curl_easy_getinfo(curl.get(), CURLINFO_HTTP_CONNECTCODE, &proxyStatus);
			result.route = routeName;
			result.proxyURL = route.direct ? std::string() : route.url;
			result.proxyAnswered = proxyStatus != 0;
			result.proxyIntegratedAuthPossible = !route.direct && route.integratedAuthPossible;
			if (status == 407 || proxyStatus == 407)
			{
				result.outcome = Outcome::ProxyAuthRequired;
				result.detail = ProxyUsername(route.url).empty() ? "required" : "refused";
				return result;
			}
			if (status == 401 || status == 403)
			{
				result.outcome = Outcome::ServiceRejected;
				return result;
			}
			if (code == CURLE_OK && status == 200 && IsExpectedVersionManifest(body))
			{
				result.outcome = Outcome::Connected;
				return result;
			}
			if (code == CURLE_PEER_FAILED_VERIFICATION || code == CURLE_SSL_CONNECT_ERROR)
			{
				result.outcome = Outcome::Failed;
				result.detail = "Secure connection failed. Check certificates or TLS inspection.";
			}
			else if (code == CURLE_OK)
			{
				result.outcome = Outcome::Failed;
				result.detail = "Unexpected reply from the service (HTTP " + std::to_string(status) + ").";
			}
			else
				result.outcome = Outcome::Unreachable;
		}
	}
	return result;
}

std::string DescribeConnectionTest(const ConnectionTestResult& result)
{
	using Outcome = ConnectionTestResult::Outcome;
	switch (result.outcome)
	{
		case Outcome::Connected:
			return result.proxyURL.empty() ? "Connected through the " + result.route + " route, no proxy." : "Connected through proxy " + ProxyDisplayName(result.proxyURL) + ".";
		case Outcome::ProxyAuthRequired:
			return result.detail == "required" ? "The proxy needs a login." : "The proxy refused the login.";
		case Outcome::ServiceRejected:
			return "The service refused the connection.";
		case Outcome::Unreachable:
			return std::string("Could not reach the service.") + (result.proxyURL.empty() ? "\nCheck your internet connection." : "\nCheck the proxy address.");
		default:
			return result.detail.empty() ? "Could not test the connection." : result.detail;
	}
}
}
