#include "GameNetwork/GeneralsOnline/WifiLowLatency.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdarg.h>
#include <stdio.h>

#pragma comment(lib, "iphlpapi.lib")

// The few wlanapi.h declarations used, so no SDK header or import library is needed
enum
{
	WIFI_OPCODE_MEDIA_STREAMING = 3,	// wlan_intf_opcode_media_streaming_mode
	WIFI_STATE_CONNECTED = 1,			// wlan_interface_state_connected
	WIFI_CLIENT_VERSION = 2,			// the Vista and later API
	WIFI_SOURCE_NONE = 0,				// WLAN_NOTIFICATION_SOURCE_NONE
	WIFI_SOURCE_ACM = 8,				// WLAN_NOTIFICATION_SOURCE_ACM
	WIFI_ACM_CONNECTION_COMPLETE = 10,
	WIFI_ACM_DISCONNECTED = 21,
	WIFI_POLL_MS = 60000,				// slow fallback check, it only queries
	WIFI_SETTLE_MS = 1000,				// a connection change needs a moment before the route is final
	WIFI_BACKOFF_MIN_MS = 2000,
	WIFI_BACKOFF_MAX_MS = 60000,
	WIFI_END_WAIT_MS = 250,				// End does not wait longer for a slow WLAN service, the worker then ends on its own
	WIFI_MAX_TARGETS = 2,				// the IPv4 and the IPv6 route
	WIFI_LOG_GENERAL = WIFI_MAX_TARGETS,
	WIFI_LOG_NOTIFY = WIFI_MAX_TARGETS + 1,
	WIFI_LOG_SLOTS = WIFI_MAX_TARGETS + 2
};

struct WifiInterfaceInfo
{
	GUID guid;
	WCHAR description[256];
	DWORD state;
};
static_assert(sizeof(WifiInterfaceInfo) == 532, "must match WLAN_INTERFACE_INFO");

struct WifiInterfaceList
{
	DWORD count;
	DWORD index;
	WifiInterfaceInfo items[1];
};

struct WifiNotificationData
{
	DWORD source;
	DWORD code;
	GUID interfaceGuid;
	DWORD dataSize;
	PVOID data;
};

typedef VOID(WINAPI* WifiNotifyFn)(WifiNotificationData*, PVOID);
typedef DWORD(WINAPI* WifiOpenHandleFn)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD(WINAPI* WifiCloseHandleFn)(HANDLE, PVOID);
typedef DWORD(WINAPI* WifiEnumInterfacesFn)(HANDLE, PVOID, WifiInterfaceList**);
typedef DWORD(WINAPI* WifiSetInterfaceFn)(HANDLE, const GUID*, DWORD, DWORD, const PVOID, PVOID);
typedef DWORD(WINAPI* WifiQueryInterfaceFn)(HANDLE, const GUID*, DWORD, PVOID, PDWORD, PVOID*, PDWORD);
typedef DWORD(WINAPI* WifiRegisterNotificationFn)(HANDLE, DWORD, BOOL, WifiNotifyFn, PVOID, PVOID, PDWORD);
typedef VOID(WINAPI* WifiFreeMemoryFn)(PVOID);

struct WifiApi
{
	WifiOpenHandleFn open;
	WifiCloseHandleFn close;
	WifiEnumInterfacesFn enumInterfaces;
	WifiSetInterfaceFn set;
	WifiQueryInterfaceFn query;
	WifiRegisterNotificationFn registerNotification;	// optional, without it the poll does the work
	WifiFreeMemoryFn freeMemory;
};

// Shared by End and the worker, freed by whoever lets go last; the worker owns the WLAN handle, the library and its log
struct WifiState
{
	HANDLE stop;
	HANDLE wake;
	volatile LONG refs;
	volatile LONG detached;
};

// A line is logged only when it differs from the last one of its slot, so a failure shows once and so does its recovery
struct WifiLog
{
	WifiState* state;
	char last[WIFI_LOG_SLOTS][192];
};

static WifiState* s_state = NULL;
static HANDLE s_thread = NULL;
static volatile LONG s_workers = 0;

static void WifiRelease(WifiState* state)
{
	if (InterlockedDecrement(&state->refs) == 0)
	{
		CloseHandle(state->stop);
		CloseHandle(state->wake);
		delete state;
	}
}

static void WifiLogLine(WifiLog& log, int slot, const char* format, ...)
{
	char line[192];
	va_list args;
	va_start(args, format);
	_vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	line[sizeof(line) - 1] = '\0';
	if (strcmp(line, log.last[slot]) == 0 || log.state->detached)
	{
		return;
	}
	strcpy(log.last[slot], line);
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[WIFI] %s", line);
}

// Runs on a WLAN thread, so it only wakes the worker
static VOID WINAPI WifiNotify(WifiNotificationData* data, PVOID context)
{
	if (data != NULL && data->source == WIFI_SOURCE_ACM && (data->code == WIFI_ACM_CONNECTION_COMPLETE || data->code == WIFI_ACM_DISCONNECTED))
	{
		SetEvent(((WifiState*)context)->wake);
	}
}

// The handle is gone or the service restarted, so the handle has to be opened again
static bool WifiNeedsReopen(DWORD result)
{
	return result == ERROR_INVALID_HANDLE || result == ERROR_SERVICE_NOT_ACTIVE || (result >= 1700 && result <= 1899);	// 1700..1899 are the RPC_S_ errors
}

static bool WifiParseGuid(const char* text, GUID& guid)
{
	unsigned int d1, d2, d3, b[8];
	if (sscanf(text, "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
	{
		return false;
	}
	guid.Data1 = d1;
	guid.Data2 = (unsigned short)d2;
	guid.Data3 = (unsigned short)d3;
	for (int i = 0; i < 8; ++i)
	{
		guid.Data4[i] = (unsigned char)b[i];
	}
	return true;
}

// The Wi-Fi adapters on the route to the internet, none when the game traffic goes over Ethernet or a tunnel
static int WifiGameAdapters(GUID* targets)
{
	sockaddr_in peer4 = {};
	peer4.sin_family = AF_INET;
	peer4.sin_addr.S_un.S_addr = 0x01010101;	// 1.1.1.1, only used for the route lookup, nothing is sent
	sockaddr_in6 peer6 = {};
	peer6.sin6_family = AF_INET6;
	peer6.sin6_addr.u.Word[0] = htons(0x2606);
	peer6.sin6_addr.u.Word[1] = htons(0x4700);
	peer6.sin6_addr.u.Word[2] = htons(0x4700);
	peer6.sin6_addr.u.Word[7] = htons(0x1111);

	DWORD index4 = 0;
	DWORD index6 = 0;
	const bool have4 = GetBestInterfaceEx((sockaddr*)&peer4, &index4) == NO_ERROR;
	const bool have6 = GetBestInterfaceEx((sockaddr*)&peer6, &index6) == NO_ERROR;
	if (!have4 && !have6)
	{
		return 0;
	}

	ULONG size = 16 * 1024;
	IP_ADAPTER_ADDRESSES* adapters = NULL;
	ULONG result = ERROR_BUFFER_OVERFLOW;
	for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
	{
		free(adapters);
		adapters = (IP_ADAPTER_ADDRESSES*)malloc(size);
		if (adapters == NULL)
		{
			return 0;
		}
		result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, adapters, &size);
	}

	int count = 0;
	if (result == NO_ERROR)
	{
		for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != NULL && count < WIFI_MAX_TARGETS; adapter = adapter->Next)
		{
			const bool onRoute = (have4 && adapter->IfIndex == index4) || (have6 && adapter->Ipv6IfIndex == index6);
			if (adapter->IfType == IF_TYPE_IEEE80211 && onRoute && WifiParseGuid(adapter->AdapterName, targets[count]))
			{
				++count;
			}
		}
	}
	free(adapters);
	return count;
}

// Turns media streaming mode on where it is not on yet; returns an error that needs the handle reopened, else success
static DWORD WifiTune(const WifiApi& api, HANDLE client, const GUID* targets, int targetCount, WifiLog& log)
{
	WifiInterfaceList* list = NULL;
	const DWORD enumResult = api.enumInterfaces(client, NULL, &list);
	if (enumResult != ERROR_SUCCESS)
	{
		WifiLogLine(log, WIFI_LOG_GENERAL, "listing the interfaces failed, result=%lu", enumResult);
		return enumResult;
	}
	if (list == NULL)
	{
		return ERROR_SUCCESS;
	}

	DWORD fatal = ERROR_SUCCESS;
	int tuned = 0;
	for (DWORD i = 0; i < list->count && fatal == ERROR_SUCCESS; ++i)
	{
		const WifiInterfaceInfo& info = list->items[i];
		int slot = -1;
		for (int k = 0; k < targetCount; ++k)
		{
			if (IsEqualGUID(info.guid, targets[k]))
			{
				slot = k;
			}
		}
		if (slot < 0 || info.state != WIFI_STATE_CONNECTED)
		{
			continue;
		}
		++tuned;

		char description[256];
		if (WideCharToMultiByte(CP_UTF8, 0, info.description, -1, description, sizeof(description), NULL, NULL) == 0)
		{
			description[0] = '\0';
		}

		BOOL* current = NULL;
		DWORD size = 0;
		DWORD type = 0;
		const DWORD queryResult = api.query(client, &info.guid, WIFI_OPCODE_MEDIA_STREAMING, NULL, &size, (PVOID*)&current, &type);
		const bool alreadyOn = queryResult == ERROR_SUCCESS && current != NULL && size >= sizeof(BOOL) && *current;
		if (current != NULL)
		{
			api.freeMemory(current);
		}
		if (alreadyOn)
		{
			WifiLogLine(log, slot, "interface \"%s\" streaming=on (already)", description);
			continue;
		}
		if (WifiNeedsReopen(queryResult))
		{
			WifiLogLine(log, slot, "interface \"%s\" query failed, result=%lu", description, queryResult);
			fatal = queryResult;
			break;
		}

		BOOL on = TRUE;
		const DWORD setResult = api.set(client, &info.guid, WIFI_OPCODE_MEDIA_STREAMING, sizeof(on), &on, NULL);
		if (setResult == ERROR_SUCCESS)
		{
			WifiLogLine(log, slot, "interface \"%s\" streaming=on", description);
		}
		else
		{
			WifiLogLine(log, slot, "interface \"%s\" streaming failed, result=%lu", description, setResult);
			if (WifiNeedsReopen(setResult))
			{
				fatal = setResult;
			}
		}
	}
	if (tuned == 0)
	{
		WifiLogLine(log, WIFI_LOG_GENERAL, "no connected Wi-Fi interface on the game route, nothing to do");
	}
	api.freeMemory(list);
	return fatal;
}

static void WifiCloseClient(const WifiApi& api, HANDLE client, bool registered)
{
	if (registered)
	{
		api.registerNotification(client, WIFI_SOURCE_NONE, TRUE, NULL, NULL, NULL, NULL);
	}
	api.close(client, NULL);
}

static DWORD WINAPI WifiWorker(LPVOID param)
{
	WifiState* state = (WifiState*)param;
	WifiLog* log = new WifiLog();
	memset(log, 0, sizeof(*log));
	log->state = state;

	HMODULE dll = LoadLibraryExW(L"wlanapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	WifiApi api = {};
	if (dll != NULL)
	{
		api.open = (WifiOpenHandleFn)GetProcAddress(dll, "WlanOpenHandle");
		api.close = (WifiCloseHandleFn)GetProcAddress(dll, "WlanCloseHandle");
		api.enumInterfaces = (WifiEnumInterfacesFn)GetProcAddress(dll, "WlanEnumInterfaces");
		api.set = (WifiSetInterfaceFn)GetProcAddress(dll, "WlanSetInterface");
		api.query = (WifiQueryInterfaceFn)GetProcAddress(dll, "WlanQueryInterface");
		api.registerNotification = (WifiRegisterNotificationFn)GetProcAddress(dll, "WlanRegisterNotification");
		api.freeMemory = (WifiFreeMemoryFn)GetProcAddress(dll, "WlanFreeMemory");
	}
	if (api.open == NULL || api.close == NULL || api.enumInterfaces == NULL || api.set == NULL || api.query == NULL || api.freeMemory == NULL)
	{
		WifiLogLine(*log, WIFI_LOG_GENERAL, "the WLAN API is not available (%lu), no Wi-Fi tuning", GetLastError());
		if (dll != NULL)
		{
			FreeLibrary(dll);
		}
		delete log;
		WifiRelease(state);
		InterlockedDecrement(&s_workers);
		return 0;
	}

	HANDLE client = NULL;
	bool registered = false;
	DWORD backoff = WIFI_BACKOFF_MIN_MS;
	const HANDLE waits[2] = { state->stop, state->wake };
	for (;;)
	{
		bool retry = false;
		GUID targets[WIFI_MAX_TARGETS];
		const int targetCount = WifiGameAdapters(targets);
		if (targetCount == 0)
		{
			WifiLogLine(*log, WIFI_LOG_GENERAL, "the game traffic is not on Wi-Fi, nothing to do");
		}
		else
		{
			if (client == NULL)
			{
				DWORD version = 0;
				const DWORD openResult = api.open(WIFI_CLIENT_VERSION, NULL, &version, &client);
				if (openResult != ERROR_SUCCESS)
				{
					client = NULL;
					retry = true;
					WifiLogLine(*log, WIFI_LOG_GENERAL, "opening the WLAN client failed, result=%lu", openResult);
				}
				else if (api.registerNotification != NULL)
				{
					const DWORD registerResult = api.registerNotification(client, WIFI_SOURCE_ACM, TRUE, WifiNotify, state, NULL, NULL);
					registered = registerResult == ERROR_SUCCESS;
					if (!registered)
					{
						WifiLogLine(*log, WIFI_LOG_NOTIFY, "notifications failed, result=%lu, polling only", registerResult);
					}
				}
			}
			if (client != NULL)
			{
				const DWORD tuneResult = WifiTune(api, client, targets, targetCount, *log);
				if (WifiNeedsReopen(tuneResult))
				{
					WifiCloseClient(api, client, registered);
					client = NULL;
					registered = false;
					retry = true;
				}
			}
		}

		if (retry)
		{
			if (WaitForSingleObject(state->stop, backoff) == WAIT_OBJECT_0)
			{
				break;
			}
			backoff = (backoff * 2 < WIFI_BACKOFF_MAX_MS) ? backoff * 2 : WIFI_BACKOFF_MAX_MS;
			continue;
		}
		backoff = WIFI_BACKOFF_MIN_MS;

		const DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, WIFI_POLL_MS);
		if (waitResult == WAIT_OBJECT_0)
		{
			break;
		}
		if (waitResult == WAIT_OBJECT_0 + 1 && WaitForSingleObject(state->stop, WIFI_SETTLE_MS) == WAIT_OBJECT_0)
		{
			break;
		}
	}

	// closing the handle withdraws our request, other clients keep theirs
	if (client != NULL)
	{
		WifiCloseClient(api, client, registered);
	}
	FreeLibrary(dll);
	WifiLogLine(*log, WIFI_LOG_GENERAL, "released the Wi-Fi tuning");
	delete log;
	WifiRelease(state);
	InterlockedDecrement(&s_workers);
	return 0;
}

void WifiLowLatency::Begin()
{
	if (s_state != NULL)
	{
		return;
	}
	if (s_workers != 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[WIFI] the previous worker is still ending, no Wi-Fi tuning for this match");
		return;
	}
	WifiState* state = new WifiState();
	state->stop = CreateEventA(NULL, TRUE, FALSE, NULL);
	state->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
	state->refs = 2;
	state->detached = 0;
	if (state->stop == NULL || state->wake == NULL)
	{
		if (state->stop != NULL)
		{
			CloseHandle(state->stop);
		}
		if (state->wake != NULL)
		{
			CloseHandle(state->wake);
		}
		delete state;
		return;
	}

	InterlockedIncrement(&s_workers);
	s_thread = CreateThread(NULL, 0, WifiWorker, state, 0, NULL);
	if (s_thread == NULL)
	{
		InterlockedDecrement(&s_workers);
		CloseHandle(state->stop);
		CloseHandle(state->wake);
		delete state;
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[WIFI] starting the worker failed (%lu)", GetLastError());
		return;
	}
	s_state = state;
	// nothing here is time critical, the game threads come first
	SetThreadPriority(s_thread, THREAD_PRIORITY_LOWEST);
}

void WifiLowLatency::End()
{
	if (s_state == NULL)
	{
		return;
	}
	SetEvent(s_state->stop);
	if (WaitForSingleObject(s_thread, WIFI_END_WAIT_MS) != WAIT_OBJECT_0)
	{
		// the worker is stuck in the WLAN service, it cleans up after itself once the call returns
		InterlockedExchange(&s_state->detached, 1);
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[WIFI] the worker did not stop within %dms, it ends on its own", (int)WIFI_END_WAIT_MS);
	}
	CloseHandle(s_thread);
	WifiRelease(s_state);
	s_thread = NULL;
	s_state = NULL;
}
