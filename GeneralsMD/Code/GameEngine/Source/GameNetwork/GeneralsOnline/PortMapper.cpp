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
#include "../OnlineServices_Init.h"

struct IPCapsResult
{
	int ipversion;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(IPCapsResult, ipversion)
};

void pcpMappingCallback(int id, plum_state_t state, const plum_mapping_t* mapping)
{
	NGMP_OnlineServicesManager* pOnlineServiceMgr = NGMP_OnlineServicesManager::GetInstance();

	if (pOnlineServiceMgr == nullptr)
	{
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: PCP Mapping %d: state=%d\n", id, (int)state);
	switch (state) {
	case PLUM_STATE_SUCCESS:
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: PCP Mapping %d: success, internal=%hu, external=%s:%hu\n", id, mapping->internal_port,
			mapping->external_host, mapping->external_port);

		NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().StorePCPOutcome(true);
		break;
	}

	case PLUM_STATE_FAILURE:
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: PCP Mapping %d: failed\n", id);

		NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().StorePCPOutcome(false);
		break;

	default:
		break;
	}
}

void PortMapper::Tick()
{
	if (m_bPCPNeedsCleanup)
	{
		if (m_bPortMapper_PCP_Complete.load())
		{
			m_bPCPNeedsCleanup = false;
			
			// cleanup
			plum_cleanup();
		}
	}

	// do we have work to do on main thread?
	bool bEverythingComplete = m_bPortMapper_PCP_Complete.load() && m_bPortMapper_UPNP_Complete.load() && m_bPortMapper_NATPMP_Complete.load();
	bool bIPChecksComplete = m_IPV4 != ECapabilityState::UNDETERMINED && m_IPV6 != ECapabilityState::UNDETERMINED;
	// if one thing succeeded, bail, or if everything is done, also bail
	if ((m_bPortMapper_AnyMappingSuccess.load() || bEverythingComplete) && bIPChecksComplete)
	{
#if defined(ENABLE_DIRECTCONNECT_TEST)
		if (!m_bNATCheckStarted)
		{
			int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

			m_bNATCheckStarted = true;
			// If any mapping completed succesfully, just let the game continue, the other threads will finish silently and not affect anything
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Port mapper is complete (took %d ms), starting NAT flow", currTime - m_timeStartPortMapping);

			m_bPortMapperWorkComplete.store(false);

			// inform service
			EMappingTech mappingTechUsed = GetPortMappingTechnologyUsed();
			nlohmann::json j;
			j["mapping_tech"] = mappingTechUsed;
			j["ipv4"] = m_IPV4 == ECapabilityState::SUPPORTED;
			j["ipv6"] = m_IPV6 == ECapabilityState::SUPPORTED;
			std::string strPostData = j.dump();
			std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("Connectivity");
			NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Connectivity outcome - Mapping Tech is %d, IPv4 is %d, IPv6 is %d", mappingTechUsed, (int)(m_IPV4 == ECapabilityState::SUPPORTED), m_IPV6 == ECapabilityState::SUPPORTED);
			std::map<std::string, std::string> mapHeaders;
			NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendPOSTRequest(strURI.c_str(), EIPProtocolVersion::DONT_CARE, mapHeaders, strPostData.c_str(), [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
				{
					// dont care about the response
				});

			// start nat checker
			StartNATCheck();
#else
		// we're done, just return
		InvokeCallback();
#endif
		}
	}

#if defined(ENABLE_DIRECTCONNECT_TEST)
	if (m_bNATCheckInProgress)
	{
		// send outbound traffic too
		struct sockaddr_in punchAddr;
#if _DEBUG && !defined(USE_TEST_ENV)
		hostent* pEnt = gethostbyname("localhost");
#else
		hostent* pEnt = gethostbyname("cloud.playgenerals.online");
#endif
		if (pEnt != nullptr)
		{
			memcpy(&punchAddr.sin_addr, pEnt->h_addr_list[0], pEnt->h_length);
			punchAddr.sin_family = AF_INET;
			punchAddr.sin_port = htons(9000);

			const char* punchMsg = "NATPUNCH";
			for (int i = 0; i < 25; ++i)
			{
				sendto(m_NATSocket, punchMsg, static_cast<int>(strlen(punchMsg)), 0, (sockaddr*)&punchAddr, sizeof(punchAddr));
			}
		}

		// check here first, in case we early out
		if (m_bProbesReceived)
		{
			m_bNATCheckInProgress = false;
			m_directConnect = ECapabilityState::SUPPORTED;

			// callback
			InvokeCallback();

			if (m_NATSocket != INVALID_SOCKET)
			{
				closesocket(m_NATSocket);
				WSACleanup();
				m_NATSocket = INVALID_SOCKET;
			}
		}

		// timed out?
		int64_t currTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		if (currTime - m_probeStartTime >= m_probeTimeout)
		{
			m_bNATCheckInProgress = false;
			m_directConnect = ECapabilityState::UNSUPPORTED;

			// callback
			InvokeCallback();

			if (m_NATSocket != INVALID_SOCKET)
			{
				closesocket(m_NATSocket);
				WSACleanup();
				m_NATSocket = INVALID_SOCKET;
			}
		}

		if (m_NATSocket != INVALID_SOCKET)
		{
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
					return;
				}
			}

			buffer[bytesReceived] = '\0';
			if (strcmp(buffer, "NATCHECK") == 0)
			{
				m_bProbesReceived = true;
			}
		}
	}
}
#endif

#if defined(ENABLE_DIRECTCONNECT_TEST)
void PortMapper::StartNATCheck()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Checker]: Starting");
	m_bProbesReceived = false;
	m_directConnect = ECapabilityState::UNDETERMINED;

	// init recv socket 
	WSADATA wsaData;
	
	sockaddr_in serverAddr;
	const int PORT = m_PreferredPort.load();

	// Initialize Winsock
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Failed to initialize Winsock. Error: %d", WSAGetLastError());
		m_directConnect = ECapabilityState::UNSUPPORTED;
		InvokeCallback();
		return;
	}

	// Create UDP server socket
	m_NATSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if (m_NATSocket == INVALID_SOCKET)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Socket creation failed. Error: %d", WSAGetLastError());
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		InvokeCallback();
		return;
	}

	// Set the socket to non-blocking mode
	u_long mode = 1;
	if (ioctlsocket(m_NATSocket, FIONBIO, &mode) != NO_ERROR)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Failed to set non-blocking mode.");
		closesocket(m_NATSocket);
		m_NATSocket = INVALID_SOCKET;
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		InvokeCallback();
		return;
	}

	// Bind the socket to an address and port
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(PORT);
	serverAddr.sin_addr.s_addr = INADDR_ANY;

	if (bind(m_NATSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Binding failed. Error: %d", WSAGetLastError());
		closesocket(m_NATSocket);
		m_NATSocket = INVALID_SOCKET;
		WSACleanup();
		m_directConnect = ECapabilityState::UNSUPPORTED;
		InvokeCallback();
		return;
	}

	// TODO_NAT: This would be a lot more effective if we knew the response port too
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Start Holepunch");
	struct sockaddr_in punchAddr;
#if _DEBUG && !defined(USE_TEST_ENV)
	hostent* pEnt = gethostbyname("localhost");
#else
	hostent* pEnt = gethostbyname("cloud.playgenerals.online");
#endif
	if (pEnt != nullptr)
	{
		memcpy(&punchAddr.sin_addr, pEnt->h_addr_list[0], pEnt->h_length);
		punchAddr.sin_family = AF_INET;
		punchAddr.sin_port = htons(9000);

		const char* punchMsg = "NATPUNCH";
		for (int i = 0; i < 50; ++i)
		{
			sendto(m_NATSocket, punchMsg, static_cast<int>(strlen(punchMsg)), 0, (sockaddr*)&punchAddr, sizeof(punchAddr));
		}
	}
	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Finished Holepunch");


	NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Check]: Really starting");
	// do NAT check
	m_bNATCheckInProgress = true;
	m_probeStartTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
	std::string strToken = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("NATCheck");
	std::map<std::string, std::string> mapHeaders;

	nlohmann::json j;
	j["preferred_port"] = m_PreferredPort.load();
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
				InvokeCallback();

				NetworkLog(ELogVerbosity::LOG_RELEASE, "[NAT Checker]: Error code %d", statusCode);
			}
		});
}
#endif

void PortMapper::DetermineLocalNetworkCapabilities()
{
	CheckIPCapabilities();

	if (TheGlobalData->m_firewallPortOverride != 0)
	{
		m_PreferredPort.store(TheGlobalData->m_firewallPortOverride);

#if !defined(GENERALS_ONLINE_PORT_MAP_FIREWALL_OVERRIDE_PORT)
		m_bPortMapper_AnyMappingSuccess.store(true);
		m_bPortMapper_MappingTechUsed.store(EMappingTech::MANUAL);
		
		NetworkLog(ELogVerbosity::LOG_RELEASE, "[PortMapper] Firewall port override is set (%d), skipping port mapping and going straight to connection check", m_PreferredPort.load());
		m_bPortMapperWorkComplete.store(true);

		// dont trigger callbakc, just say we did the mapping, so we'll continue with direct connect check - this is still valid
		
		return;
#endif
	}
	else
	{
		unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
		std::mt19937 gen(seed);
		std::uniform_int_distribution<> dis(5000, 25000);

		m_PreferredPort.store(dis(gen));
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[PortMapper] Start DetermineLocalNetworkCapabilities");
	

	// reset status
	m_bPortMapperWorkComplete.store(false);

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[PortMapper] DetermineLocalNetworkCapabilities - starting background thread");

	m_timeStartPortMapping = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	// clear cleanup flag
	m_bPCPNeedsCleanup = true;

	// background threads, network ops are blocking
	m_backgroundThread_PCP = new std::thread(&PortMapper::ForwardPort_PCP, this);
	m_backgroundThread_UPNP = new std::thread(&PortMapper::ForwardPort_UPnP, this);
	m_backgroundThread_NATPMP = new std::thread(&PortMapper::ForwardPort_NATPMP, this);

#if defined(_DEBUG)
	SetThreadDescription(static_cast<HANDLE>(m_backgroundThread_PCP->native_handle()), L"PortMapper Background Thread (PCP)");
	SetThreadDescription(static_cast<HANDLE>(m_backgroundThread_UPNP->native_handle()), L"PortMapper Background Thread (UPnP)");
	SetThreadDescription(static_cast<HANDLE>(m_backgroundThread_NATPMP->native_handle()), L"PortMapper Background Thread (NAT-PMP)");
#endif
}

void PortMapper::ForwardPort_UPnP()
{
#if defined(DISABLE_UPNP)
	m_bPortMapper_UPNP_Complete.store(true);
	return;
#else
	const uint16_t port = m_PreferredPort.load();
	int error = 0;

	m_pCachedUPnPDevice = upnpDiscover(0, nullptr, nullptr, 0, 0, 2, &error);

	NetworkLog(ELogVerbosity::LOG_RELEASE, "[PortMapper]: UPnP device result: %d (errcode: %d)", m_pCachedUPnPDevice, error);

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway not found (%d)", status);

		// NOTE: dont hard fail here. not finding an exact match might be OK, some routers mangle data etc
		m_bPortMapper_UPNP_Complete.store(true);

		return;
	}

	std::string strPort = std::format("{}", port);

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

	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP Mapping added with result %d", error);

	bool bSucceeded = !error;

	// NOTE: dont hard fail here. not finding an exact match might be OK, some routers mangle data etc
	if (!m_bPortMapper_AnyMappingSuccess.load() && bSucceeded) // dont overwrite a positive value with a negative
	{
		m_bPortMapper_AnyMappingSuccess.store(true);
		m_bPortMapper_MappingTechUsed.store(EMappingTech::UPNP);
	}
	m_bPortMapper_UPNP_Complete.store(true);
#endif
}

void PortMapper::ForwardPort_NATPMP()
{
#if defined(DISABLE_NATPMP)
	m_bPortMapper_NATPMP_Complete.store(true);
	return;
#else

	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP started");

	// check for NATPMP first, quicker than trying to port map directly
	// NAT-PMP
	int res;
	natpmp_t natpmp;
	natpmpresp_t response;
	initnatpmp(&natpmp, 0, 0);

	bool bSucceeded = false;

	const int timeoutMS = 2000;
	int64_t startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

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
	} while (res == NATPMP_TRYAGAIN && ((std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() - startTime) < timeoutMS));

	if (res == NATPMP_RESPTYPE_PUBLICADDRESS)
	{
		const uint16_t port = m_PreferredPort.load();
		int r;

		startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();
		sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, port, port, 86400);
		do
		{
			fd_set fds;
			struct timeval timeout;
			FD_ZERO(&fds);
			FD_SET(natpmp.s, &fds);
			getnatpmprequesttimeout(&natpmp, &timeout);
			select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
			r = readnatpmpresponseorretry(&natpmp, &response);
		} while (r == NATPMP_TRYAGAIN && ((std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() - startTime) < timeoutMS));

		if (r >= 0)
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP mapped external port %hu to internal port %hu with lifetime %u",
				response.pnu.newportmapping.mappedpublicport,
				response.pnu.newportmapping.privateport,
				response.pnu.newportmapping.lifetime);

			bSucceeded = true;
		}
		else
		{
			NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP failed to map external port %hu to internal port %hu with lifetime %u",
				response.pnu.newportmapping.mappedpublicport,
				response.pnu.newportmapping.privateport,
				response.pnu.newportmapping.lifetime);

			bSucceeded = false;
		}
	}
	else // no NAT-PMP capable device
	{
		bSucceeded = false;
	}

	closenatpmp(&natpmp);

	// store outcome
	if (!m_bPortMapper_AnyMappingSuccess.load() && bSucceeded) // dont overwrite a positive value with a negative
	{
		m_bPortMapper_AnyMappingSuccess.store(true);
		m_bPortMapper_MappingTechUsed.store(EMappingTech::NATPMP);
	}
	m_bPortMapper_NATPMP_Complete.store(true);
#endif
}

void PortMapper::CleanupPorts()
{
	// try to remove everything
	RemovePortMapping_UPnP();
	RemovePortMapping_NATPMP();
}

void PortMapper::UPnP_RemoveAllMappingsToThisMachine()
{
	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP unmapping all mappings to this machine");
	int error = 0;

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway not found (%d)", status);
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
			NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP mass remove, Found a mapping, removing it");
			
			error = UPNP_DeletePortMapping(
				upnp_urls.controlURL,
				upnp_data.first.servicetype,
				map_wan_port,
				"UDP",
				0);

			if (error != UPNPCOMMAND_SUCCESS)
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP mass remove remove of port failed");
			}
			else
			{
				NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP mass remove of port succeeded");
			}
		}

		if (error)
		{
			break; // no more port mappings available
		}

		++index;
	}
}

void PortMapper::StorePCPOutcome(bool bSucceeded)
{
	// store outcome
	if (!m_bPortMapper_AnyMappingSuccess.load() && bSucceeded) // dont overwrite a positive value with a negative
	{
		m_bPortMapper_AnyMappingSuccess.store(true);
		m_bPortMapper_MappingTechUsed.store(EMappingTech::PCP);
	}
	m_bPortMapper_PCP_Complete.store(true);
}

struct IPCapabilitiesResponse
{
	int ipversion;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(IPCapabilitiesResponse, ipversion)
};

void PortMapper::CheckIPCapabilities()
{
	std::string strToken = NGMP_OnlineServicesManager::GetInstance()->GetAuthInterface()->GetAuthToken();
	std::string strURI = NGMP_OnlineServicesManager::GetAPIEndpoint("DetermineIPCapabilities");
	std::map<std::string, std::string> mapHeaders;

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::FORCE_IPV4, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			m_IPV4 = ECapabilityState::UNSUPPORTED;

			if (statusCode == 200)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					IPCapabilitiesResponse ipCapsResp = jsonObject.get<IPCapabilitiesResponse>();

					if (ipCapsResp.ipversion == 4)
					{
						m_IPV4 = ECapabilityState::SUPPORTED;
					}
				}
				catch (...)
				{
					m_IPV4 = ECapabilityState::UNSUPPORTED;
				}
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE, "IPV4 Support: %d", m_IPV4);
			InvokeCallback();
		}, nullptr, 2000);

	NGMP_OnlineServicesManager::GetInstance()->GetHTTPManager()->SendGETRequest(strURI.c_str(), EIPProtocolVersion::FORCE_IPV6, mapHeaders, [=](bool bSuccess, int statusCode, std::string strBody, HTTPRequest* pReq)
		{
			m_IPV6 = ECapabilityState::UNSUPPORTED;

			if (statusCode == 200)
			{
				try
				{
					nlohmann::json jsonObject = nlohmann::json::parse(strBody);
					IPCapabilitiesResponse ipCapsResp = jsonObject.get<IPCapabilitiesResponse>();

					if (ipCapsResp.ipversion == 6)
					{
						m_IPV6 = ECapabilityState::SUPPORTED;
					}
				}
				catch (...)
				{
					m_IPV6 = ECapabilityState::UNSUPPORTED;
				}
			}

			NetworkLog(ELogVerbosity::LOG_RELEASE, "IPV6 Support: %d", m_IPV6);
			InvokeCallback();
		}, nullptr, 2000);
}

void PortMapper::RemovePortMapping_UPnP()
{
	if (m_bPortMapper_MappingTechUsed.load() != EMappingTech::UPNP)
	{
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP starting unmapping of port");
	int error = 0;

	char lan_address[64];
	char wan_address[64];
	struct UPNPUrls upnp_urls;
	struct IGDdatas upnp_data;
	int status = UPNP_GetValidIGD(m_pCachedUPnPDevice, &upnp_urls, &upnp_data, lan_address, sizeof(lan_address), wan_address, sizeof(wan_address));

	if (status == 1)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway found");
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP gateway not found (%d)", status);
		return;
	}

	std::string strPort = std::format("{}", m_PreferredPort.load());

	error = UPNP_DeletePortMapping(
		upnp_urls.controlURL,
		upnp_data.first.servicetype,
		strPort.c_str(),
		"UDP",
		0);

	if (error != UPNPCOMMAND_SUCCESS)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP unmapping of port failed");
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: UPnP unmapping of port succeeded");
	}
}

void PortMapper::RemovePortMapping_NATPMP()
{
	if (m_bPortMapper_MappingTechUsed.load() != EMappingTech::NATPMP)
	{
		return;
	}

	NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP starting unmapping of port");

	int r;
	natpmp_t natpmp;
	natpmpresp_t response;
	initnatpmp(&natpmp, 0, 0);

	const int timeoutMS = 1000;
	int64_t startTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count();

	sendnewportmappingrequest(&natpmp, NATPMP_PROTOCOL_UDP, m_PreferredPort.load(), m_PreferredPort, 0);
	do
	{
		fd_set fds;
		struct timeval timeout;
		FD_ZERO(&fds);
		FD_SET(natpmp.s, &fds);
		getnatpmprequesttimeout(&natpmp, &timeout);
		select(FD_SETSIZE, &fds, NULL, NULL, &timeout);
		r = readnatpmpresponseorretry(&natpmp, &response);
	} while (r == NATPMP_TRYAGAIN && ((std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() - startTime) < timeoutMS));

	if (r < 0)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP unmapping of port failed");
	}
	else if ((std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::utc_clock::now().time_since_epoch()).count() - startTime) >= timeoutMS)
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP unmapping of port timed out");
	}
	else
	{
		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: NAT-PMP unmapping of port succeeded");
	}
	closenatpmp(&natpmp);
}

void PortMapper::ForwardPort_PCP()
{
	
#if defined(DISABLE_PCP)
	NGMP_OnlineServicesManager::GetInstance()->GetPortMapper().StorePCPOutcome(false);
	return;
#else
	const uint16_t port = m_PreferredPort.load();

	// Initialize
	plum_config_t config;
	memset(&config, 0, sizeof(config));
	config.log_level = PLUM_LOG_LEVEL_VERBOSE;
	plum_init(&config);

	// Create a first mapping
	plum_mapping_t pcpMapping;
	memset(&pcpMapping, 0, sizeof(pcpMapping));
	pcpMapping.protocol = PLUM_IP_PROTOCOL_UDP;
	pcpMapping.internal_port = port;
	pcpMapping.external_port = port;
	m_PCPMappingHandle = plum_create_mapping(&pcpMapping, pcpMappingCallback);
#endif
}

void PortMapper::RemovePortMapping_PCP()
{
	if (m_PCPMappingHandle != -1)
	{
		// Initialize
		plum_config_t config;
		memset(&config, 0, sizeof(config));
		config.log_level = PLUM_LOG_LEVEL_VERBOSE;
		plum_init(&config);

		NetworkLog(ELogVerbosity::LOG_RELEASE, "PortMapper: Removing PCP Mapping");
		plum_destroy_mapping(m_PCPMappingHandle);

		plum_cleanup();
	}
}

void PortMapper::InvokeCallback()
{
	if (NGMP_OnlineServicesManager::GetInstance()->m_cbPortMapperCallback != nullptr)
	{
		NGMP_OnlineServicesManager::GetInstance()->m_cbPortMapperCallback();
	}
}
