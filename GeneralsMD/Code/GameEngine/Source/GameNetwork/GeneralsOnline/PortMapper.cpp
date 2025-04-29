#if defined(_DEBUG)
//#define DISABLE_PORT_MAPPING 1
#endif

#include "GameNetwork/GeneralsOnline/PortMapper.h"
#include "GameNetwork/GeneralsOnline/NGMP_include.h"
#include "GameNetwork/GeneralsOnline/NGMP_interfaces.h"

#include "GameNetwork/GeneralsOnline/HTTP/HTTPManager.h"
#include "GameNetwork/GeneralsOnline/HTTP/HTTPRequest.h"
#include "GameNetwork/GeneralsOnline/json.hpp"

#include <random>
#include <chrono>
#include <winsock.h>
#include "GameNetwork/GameSpyOverlay.h"

struct IPCapsResult
{
	int ipversion;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(IPCapsResult, ipversion)
};

void PortMapper::Tick()
{
	// do we have work to do on main thread?
	if (m_bPortMapperWorkComplete.load())
	{
		NetworkLog("[NAT Check]: Port mapper is complete, starting NAT flow");

		m_bPortMapperWorkComplete.store(false);

		// start nat checker
		StartNATCheck();
	}

	if (m_bNATCheckInProgress)
	{
		// check here first, in case we early out
		bool bAllProbesReceived = true;
		for (int i = 0; i < m_probesExpected; ++i)
		{
			bAllProbesReceived &= m_probesReceived[i];
		}

		if (bAllProbesReceived)
		{
			m_bNATCheckInProgress = false;
			m_directConnect = ECapabilityState::SUPPORTED;

			// callback
			m_callbackDeterminedCaps();
			m_callbackDeterminedCaps = nullptr;

			closesocket(m_NATSocket);
			WSACleanup();
		}

		// timed out?
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		if (currTime - m_probeStartTime >= m_probeTimeout)
		{
			m_bNATCheckInProgress = false;
			m_directConnect = ECapabilityState::UNSUPPORTED;

			// callback
			m_callbackDeterminedCaps();
			m_callbackDeterminedCaps = nullptr;

			closesocket(m_NATSocket);
			WSACleanup();
		}

		// now recv again
		char buffer[1024] = { 0 };
		sockaddr_in clientAddr;
		int clientAddrLen = sizeof(clientAddr);

		int bytesReceived = recvfrom(m_NATSocket, buffer, sizeof(buffer), 0, (sockaddr*)&clientAddr, &clientAddrLen);
		if (bytesReceived == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
			{
				return;
			}
			else {
				NetworkLog("[NAT Check]: recvfrom failed");
				return;
			}
		}

		buffer[bytesReceived] = '\0';
		//NetworkLog("[NAT Check]: Received from server: %s", buffer);

		for (int i = 0; i < m_probesExpected; ++i)
		{
			char szBuffer[32] = { 0 };
			sprintf_s(szBuffer, "NATCHECK%d", i);

			if (strcmp(buffer, szBuffer) == 0)
			{
				m_probesReceived[i] = true;
			}
		}
	}
}

void PortMapper::StartNATCheck()
{
	NetworkLog("[NAT Checker]: Starting");
	for (int i = 0; i < m_probesExpected; ++i)
	{
		m_probesReceived[i] = false;
	}
	m_directConnect = ECapabilityState::UNDETERMINED;

	// init recv socket 
	WSADATA wsaData;
	
	sockaddr_in serverAddr;
	const int PORT = m_PreferredPort;

	// Initialize Winsock
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		NetworkLog("[NAT Check]: Failed to initialize Winsock. Error: %d", WSAGetLastError());
		m_directConnect = ECapabilityState::UNSUPPORTED;
		m_callbackDeterminedCaps();
		m_callbackDeterminedCaps = nullptr;
		return;
	}

	// Create UDP server socket
	m_NATSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_NATSocket == INVALID_SOCKET)
	{
		NetworkLog("[NAT Check]: Socket creation failed. Error: %d", WSAGetLastError());
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		m_callbackDeterminedCaps();
		m_callbackDeterminedCaps = nullptr;
		return;
	}

	// Set the socket to non-blocking mode
	u_long mode = 1;
	if (ioctlsocket(m_NATSocket, FIONBIO, &mode) != NO_ERROR)
	{
		NetworkLog("[NAT Check]: Failed to set non-blocking mode.");
		closesocket(m_NATSocket);
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		m_callbackDeterminedCaps();
		m_callbackDeterminedCaps = nullptr;
		return;
	}

	// Bind the socket to an address and port
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(PORT);
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(m_NATSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
	{
		NetworkLog("[NAT Check]: Binding failed. Error: %d", WSAGetLastError());
		closesocket(m_NATSocket);
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		m_callbackDeterminedCaps();
		m_callbackDeterminedCaps = nullptr;
		return;
	}

	NetworkLog("[NAT Check]: Really starting");
	// do NAT check
	m_bNATCheckInProgress = true;
	m_probeStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	std::string strToken = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("NATCheck", true);
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["preferred_port"] = m_PreferredPort;
	std::string strPostData = j.dump();

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::FORCE_IPV4, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			if (statusCode == 200)
			{

			}
			else
			{
				// return immediately, wont work
				m_directConnect = ECapabilityState::UNSUPPORTED;
				m_callbackDeterminedCaps();
				m_callbackDeterminedCaps = nullptr;

				NetworkLog("[NAT Checker]: Error code %d", statusCode);
			}
		});
}

void PortMapper::BackgroundThreadRun()
{
	NetworkLog("[PortMapper]: BackgroundThreadRun");

	// reset state
	m_directConnect = ECapabilityState::UNDETERMINED;
	m_capUPnP = ECapabilityState::UNDETERMINED;
	m_capNATPMP = ECapabilityState::UNDETERMINED;
	
	// TODO_NGMP: Do this on a background thread?

	int64_t startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// UPnP
	int errorCode = 0;
	m_pCachedUPnPDevice = upnpDiscover(0, nullptr, nullptr, 0, 0, 2, &errorCode);

	NetworkLog("[PortMapper]: UPnP device result: %d (errcode: %d)", m_pCachedUPnPDevice, errorCode);

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int IGDStatus = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	m_capUPnP = (IGDStatus == 1) ? ECapabilityState::SUPPORTED : ECapabilityState::UNSUPPORTED;

	NetworkLog("[PortMapper]: UPnP result: %s (LAN: %s, WAN: %s, Control URI)", m_capUPnP == ECapabilityState::SUPPORTED ? "Supported" : "Unsupported", lan_address, wan_address);
	NetworkLog("[PortMapper]: UPnP controlURL URI: %s", upnp_urls.controlURL);
	NetworkLog("[PortMapper]: UPnP ipcondescURL URI: %s", upnp_urls.ipcondescURL);
	NetworkLog("[PortMapper]: UPnP controlURL_CIF URI: %s", upnp_urls.controlURL_CIF);
	NetworkLog("[PortMapper]: UPnP controlURL_6FC URI: %s", upnp_urls.controlURL_6FC);
	NetworkLog("[PortMapper]: UPnP rootdescURL URI: %s", upnp_urls.rootdescURL);
	NetworkLog("[PortMapper]: UPnP cureltname URI: %s", upnp_data.cureltname);
	NetworkLog("[PortMapper]: UPnP urlbase URI: %s", upnp_data.urlbase);
	NetworkLog("[PortMapper]: UPnP presentationurl URI: %s", upnp_data.presentationurl);
	NetworkLog("[PortMapper]: UPnP cureltname URI: %s", upnp_data.cureltname);
	NetworkLog("[PortMapper]: UPnP cureltname URI: %s", upnp_data.cureltname);
	NetworkLog("[PortMapper]: UPnP cureltname URI: %s", upnp_data.cureltname);

	int64_t endTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	NetworkLog("[PortMapper]: UPnP took: %lld ms", endTime- startTime);

	// NAT-PMP
	startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	int res;
	natpmp_t natpmp;
	natpmpresp_t response;
	initnatpmp(&natpmp, 0, 0);

	sendpublicaddressrequest(&natpmp);
	do
	{
		fd_set fds;
		struct timeval timeout;
		FD_ZERO(&fds);
		FD_SET(natpmp.s, &fds);

		getnatpmprequesttimeout(&natpmp, &timeout);
		select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		res = readnatpmpresponseorretry(&natpmp, &response);

		if (res == NATPMP_TRYAGAIN)
		{
			NetworkLog("[PortMapper]: NAT-PMP asked for try again");
		}

		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		int64_t timeSpentInNATPMP = currTime - startTime;

		const int natpmpTimeout = 2000;
		if (timeSpentInNATPMP >= natpmpTimeout)
		{
			NetworkLog("[PortMapper]: NAT-PMP timeout reached, bailing");
			break;
		}
	}
	while (res == NATPMP_TRYAGAIN);

	closenatpmp(&natpmp);

	endTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	NetworkLog("[PortMapper]: NAT-PMP took: %lld ms", endTime - startTime);

	m_capNATPMP = (res == NATPMP_RESPTYPE_PUBLICADDRESS) ? ECapabilityState::SUPPORTED : ECapabilityState::UNSUPPORTED;;
	NetworkLog("[PortMapper]: NAT-PMP result: %s (%d)", m_capNATPMP == ECapabilityState::SUPPORTED ? "Supported" : "Unsupported", m_capNATPMP);

	// open ports
	startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	TryForwardPreferredPorts();
	endTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	NetworkLog("[PortMapper]: TryForwardPreferredPorts took: %lld ms", endTime - startTime);

	m_bPortMapperWorkComplete.store(true);
	NetworkLog("[PortMapper] Background thread work is done");
}

void PortMapper::DetermineLocalNetworkCapabilities(std::function<void(void)> callbackDeterminedCaps)
{
	// store callback
	m_callbackDeterminedCaps = callbackDeterminedCaps;

	if (TheGlobalData->m_firewallPortOverride != 0)
	{
		m_capUPnP = ECapabilityState::OVERRIDDEN;
		m_capNATPMP = ECapabilityState::OVERRIDDEN;
		m_PreferredPort = TheGlobalData->m_firewallPortOverride;

		NetworkLog("[PortMapper] Firewall port override is set (%d), skipping port mapping and going straight to connection check", m_PreferredPort);
		m_bPortMapperWorkComplete.store(true);

		// dont trigger callbakc, just say we did the mapping, so we'll continue with direct connect check - this is still valid
		
		return;
	}
	NetworkLog("[PortMapper] Start DetermineLocalNetworkCapabilities");
	

	// reset status
	m_bPortMapperWorkComplete.store(false);

	NetworkLog("[PortMapper] DetermineLocalNetworkCapabilities - starting background thread");

	// background thread, network ops are blocking
	m_backgroundThread = new std::thread(&PortMapper::BackgroundThreadRun, this);
	SetThreadDescription(static_cast<HANDLE>(m_backgroundThread->native_handle()), L"PortMapper Background Thread");
}



void PortMapper::TryForwardPreferredPorts()
{
	NetworkLog("[PortMapper]: TryForwardPreferredPorts");
	// clean up anything we had, might be a re-enter
	CleanupPorts();

	// TODO_NGMP: Better detection of if in use already
	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
	std::mt19937 gen(seed);
	std::uniform_int_distribution<> dis(5000, 25000);

	m_PreferredPort = dis(gen);

#if defined (DISABLE_PORT_MAPPING)
	m_capUPnP = ECapabilityState::UNSUPPORTED;
	m_capNATPMP = ECapabilityState::UNSUPPORTED;
	return;
#endif

	NetworkLog("PortMapper: Attempting to open ext port %d and forward to local port %d", m_PreferredPort, m_PreferredPort);

	if (m_capUPnP != ECapabilityState::SUPPORTED && m_capNATPMP != ECapabilityState::SUPPORTED)
	{
		NetworkLog("PortMapper: No port mappers are supported.");
		return;
	}

	// TODO_NGMP: If everything fails, try a new port?
	
	// TODO_NGMP: Handle state where we are too early
	// prefer UPnP over NAT-PMP if both available
	if (m_capUPnP == ECapabilityState::SUPPORTED)
	{
		NetworkLog("PortMapper: Using UPnP");

		bool bSuccess = ForwardPreferredPort_UPnP();
		// if failed, and natpmp is available, try that next
		if (!bSuccess)
		{
			if (m_capNATPMP == ECapabilityState::SUPPORTED)
			{
				NetworkLog("PortMapper: UPnP failed, local network supports NATPMP, trying NATPMP instead.");
				bool bSuccess = ForwardPreferredPort_NATPMP();
				if (!bSuccess)
				{
					NetworkLog("PortMapper: NATPMP fallback failed.");
				}
				else
				{
					NetworkLog("PortMapper: NATPMP fallback succeeded.");
				}
			}
			else
			{
				NetworkLog("PortMapper: UPnP failed, and the local network does not support NATPMP.");
			}
		}
		else
		{
			NetworkLog("PortMapper: UPnP was successful");
		}
	}
	else if (m_capNATPMP == ECapabilityState::SUPPORTED)
	{
		NetworkLog("PortMapper: Using NAT-PMP");

		bool bSuccess = ForwardPreferredPort_NATPMP();
		// if failed, nothing to do, we either didnt have upnp, or already tried it as our preference
		if (!bSuccess)
		{
			NetworkLog("PortMapper: NATPMP failed and we have no fallback.");
		}
		else
		{
			NetworkLog("PortMapper: NAT-PMP was successful");
		}
	}
}

void PortMapper::CleanupPorts()
{
	if (m_bHasPortOpenedViaUPNP)
	{
		RemovePortMapping_UPnP();
	}

	if (m_bHasPortOpenedViaNATPMP)
	{
		RemovePortMapping_NATPMP();
	}
}

// TODO_NGMP: remove port mappings on exit
bool PortMapper::ForwardPreferredPort_UPnP()
{
	int error = 0;

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog("PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog("PortMapper: UPnP gateway not found (%d)", status);
		return false;
	}

	std::string strPort = std::format("{}", m_PreferredPort);

	error = UPNP_AddPortMapping(
		upnp_urls.controlURL,
		upnp_data.first.servicetype,
		strPort.c_str(),
		strPort.c_str(),
		lan_address,
		"C&C Generals Online",
		"UDP",
		nullptr,
		"86400"); // 24 hours

	NetworkLog("PortMapper: UPnP Mapping added with result %d", error);

	// check our mapping was added correctly
	size_t index = 0;
	while (true)
	{
		char map_wan_port[200] = "";
		char map_lan_address[200] = "";
		char map_lan_port[200] = "";
		char map_protocol[200] = "";
		char map_description[200] = "";
		char map_mapping_enabled[200] = "";
		char map_remote_host[200] = "";
		char map_lease_duration[200] = ""; // original time

		error = UPNP_GetGenericPortMappingEntry(
			upnp_urls.controlURL,
			upnp_data.first.servicetype,
			std::to_string(index).c_str(),
			map_wan_port,
			map_lan_address,
			map_lan_port,
			map_protocol,
			map_description,
			map_mapping_enabled,
			map_remote_host,
			map_lease_duration);

		if (!error
			&& strcmp(map_wan_port, strPort.c_str()) == 0
			&& strcmp(map_lan_address, lan_address) == 0
			&& strcmp(map_lan_port, strPort.c_str()) == 0
			&& strcmp(map_protocol, "UDP") == 0
			&& strcmp(map_description, "C&C Generals Online") == 0
			)
		{
			NetworkLog("PortMapper: UPnP Mapping validated on router");
			m_bHasPortOpenedViaUPNP = true;

			return true;
		}

		if (error)
		{
			break; // no more port mappings available
		}

		++index;
	}

	// NOTE: dont hard fail here. not finding an exact match might be OK, some routers mangle data etc
	NetworkLog("PortMapper: UPnP Mapping was not validated on router, this is likely OK");
	m_bHasPortOpenedViaUPNP = true;

	return true;
}

bool PortMapper::ForwardPreferredPort_NATPMP()
{
	int r;
	natpmp_t natpmp;
	natpmpresp_t response;
	initnatpmp(&natpmp, 0, 0);

	sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, m_PreferredPort, m_PreferredPort, 86400);
	do
	{
		fd_set fds;
		struct timeval timeout;
		FD_ZERO(&fds);
		FD_SET(natpmp.s, &fds);
		getnatpmprequesttimeout(&natpmp, &timeout);
		select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		r = readnatpmpresponseorretry(&natpmp, &response);
	}
	while (r == NATPMP_TRYAGAIN);

	NetworkLog("PortMapper: NAT-PMP mapped external port %hu to internal port %hu with lifetime %u",
		response.pnu.newportmapping.mappedpublicport,
		response.pnu.newportmapping.privateport,
		response.pnu.newportmapping.lifetime);
	closenatpmp(&natpmp);

	m_bHasPortOpenedViaNATPMP = r >= 0;

	return m_bHasPortOpenedViaNATPMP;

}

void PortMapper::UPnP_RemoveAllMappingsToThisMachine()
{
	NetworkLog("PortMapper: UPnP unmapping all mappings to this machine");
	int error = 0;

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog("PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog("PortMapper: UPnP gateway not found (%d)", status);
		return;
	}

	size_t index = 0;
	while (true)
	{
		char map_wan_port[200] = "";
		char map_lan_address[200] = "";
		char map_lan_port[200] = "";
		char map_protocol[200] = "";
		char map_description[200] = "";
		char map_mapping_enabled[200] = "";
		char map_remote_host[200] = "";
		char map_lease_duration[200] = ""; // original time

		error = UPNP_GetGenericPortMappingEntry(
			upnp_urls.controlURL,
			upnp_data.first.servicetype,
			std::to_string(index).c_str(),
			map_wan_port,
			map_lan_address,
			map_lan_port,
			map_protocol,
			map_description,
			map_mapping_enabled,
			map_remote_host,
			map_lease_duration);

		if (!error
			&& strcmp(map_lan_address, lan_address) == 0
			&& strcmp(map_protocol, "UDP") == 0
			&& strcmp(map_description, "C&C Generals Online") == 0
			)
		{
			NetworkLog("PortMapper: UPnP mass remove, Found a mapping, removing it");
			
			error = UPNP_DeletePortMapping(
				upnp_urls.controlURL,
				upnp_data.first.servicetype,
				map_wan_port,
				"UDP",
				0);

			if (error != UPNPCOMMAND_SUCCESS)
			{
				NetworkLog("PortMapper: UPnP mass remove remove of port failed");
			}
			else
			{
				NetworkLog("PortMapper: UPnP mass remove of port succeeded");
			}
		}

		if (error)
		{
			break; // no more port mappings available
		}

		++index;
	}
}

void PortMapper::RemovePortMapping_UPnP()
{
	NetworkLog("PortMapper: UPnP starting unmapping of port");
	int error = 0;

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog("PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog("PortMapper: UPnP gateway not found (%d)", status);
		return;
	}

	std::string strPort = std::format("{}", m_PreferredPort);

	error = UPNP_DeletePortMapping(
		upnp_urls.controlURL,
		upnp_data.first.servicetype,
		strPort.c_str(),
		"UDP",
		0);

	if (error != UPNPCOMMAND_SUCCESS)
	{
		NetworkLog("PortMapper: UPnP unmapping of port failed");
	}
	else
	{
		NetworkLog("PortMapper: UPnP unmapping of port succeeded");
	}
}

void PortMapper::RemovePortMapping_NATPMP()
{
	NetworkLog("PortMapper: NAT-PMP starting unmapping of port");

	int r;
	natpmp_t natpmp;
	natpmpresp_t response;
	initnatpmp(&natpmp, 0, 0);

	sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, m_PreferredPort, m_PreferredPort, 0);
	do
	{
		fd_set fds;
		struct timeval timeout;
		FD_ZERO(&fds);
		FD_SET(natpmp.s, &fds);
		getnatpmprequesttimeout(&natpmp, &timeout);
		select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		r = readnatpmpresponseorretry(&natpmp, &response);
	} while (r == NATPMP_TRYAGAIN);

	if (r < 0)
	{
		NetworkLog("PortMapper: NAT-PMP unmapping of port failed");
	}
	else
	{
		NetworkLog("PortMapper: NAT-PMP unmapping of port succeeded");
	}
	closenatpmp(&natpmp);
}
