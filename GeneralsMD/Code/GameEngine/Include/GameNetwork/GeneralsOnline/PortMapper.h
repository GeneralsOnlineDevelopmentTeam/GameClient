#pragma once

#define NATPMP_STATICLIB 1
#pragma comment(lib, "iphlpapi.lib")

#include "GameNetwork/GeneralsOnline/Vendor/libnatpmp/natpmp.h"
#include "GameNetwork/GeneralsOnline/Vendor/miniupnpc/miniupnpc.h"
#include "GameNetwork/GeneralsOnline/Vendor/miniupnpc/miniupnpctypes.h"
#include "GameNetwork/GeneralsOnline/Vendor/miniupnpc/upnperrors.h"
#include "GameNetwork/GeneralsOnline/Vendor/miniupnpc/upnpcommands.h"
#include <functional>
#include <thread>

enum ECapabilityState : uint8_t
{
	UNDETERMINED,
	UNSUPPORTED,
	SUPPORTED
};

class PortMapper
{
public:
	~PortMapper()
	{
		CleanupPorts();
	}

	void Tick();
	void StartNATCheck();

	void BackgroundThreadRun();
	std::thread* m_backgroundThread = nullptr;

	void DetermineLocalNetworkCapabilities(std::function<void(void)> callbackDeterminedCaps);

	void TryForwardPreferredPorts();
	void CleanupPorts();

	ECapabilityState HasDirectConnect()
	{
		return m_directConnect;
	}

	ECapabilityState HasUPnP()
	{
		return m_capUPnP;
	}

	ECapabilityState HasNATPMP()
	{
		return m_capNATPMP;
	}

	ECapabilityState HasIPv6()
	{
		return m_capIPv6;
	}

	ECapabilityState HasIPv4()
	{
		return m_capIPv4;
	}

	bool HasPortOpen() const { return m_bHasPortOpenedViaUPNP || m_bHasPortOpenedViaNATPMP; }
	bool HasPortOpenUPnP() const { return m_bHasPortOpenedViaUPNP; }
	bool HasPortOpenNATPMP() const { m_bHasPortOpenedViaNATPMP; }
	int GetOpenPort_External() const { return m_PreferredPortExternal; }
	int GetOpenPort_Internal() const { return m_PreferredPortInternal; }

	void UPnP_RemoveAllMappingsToThisMachine();

private:
	bool ForwardPreferredPort_UPnP();
	bool ForwardPreferredPort_NATPMP();

	void RemovePortMapping_UPnP();
	void RemovePortMapping_NATPMP();

private:
	std::function<void(void)> m_callbackDeterminedCaps = nullptr;
	ECapabilityState m_directConnect = ECapabilityState::UNDETERMINED;
	ECapabilityState m_capUPnP = ECapabilityState::UNDETERMINED;
	ECapabilityState m_capNATPMP = ECapabilityState::UNDETERMINED;
	ECapabilityState m_capIPv4 = ECapabilityState::UNDETERMINED;
	ECapabilityState m_capIPv6 = ECapabilityState::UNDETERMINED;

	bool m_bHasPortOpenedViaUPNP = false;
	bool m_bHasPortOpenedViaNATPMP = false;

	uint16_t m_PreferredPortInternal = 0;
	uint16_t m_PreferredPortExternal = 0;

	SOCKET m_NATSocket;
	bool m_bNATCheckInProgress = false;

	std::atomic<bool> m_bPortMapperWorkComplete = false;

	const int m_probeTimeout = 3000;
	int64_t m_probeStartTime = -1;
	static const int m_probesExpected = 5;
	bool m_probesReceived[m_probesExpected];
};