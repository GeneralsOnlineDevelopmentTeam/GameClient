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
	// if one thing succeeded, bail, or if everything is done, also bail
	if ((m_bPortMapper_AnyMappingSuccess.load() || bEverythingComplete))
	{
		// we're done
		InvokeCallback();
	}
}

void PortMapper::DetermineLocalNetworkCapabilities()
{
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
