#include "networkInterfaceSolaris.h"
#include "networkSolarisWrapper.hpp"


std::shared_ptr<IOSNetwork> FactorySolarisNetwork::create(const std::shared_ptr<INetworkInterfaceWrapper>& interfaceWrapper)
{
    std::shared_ptr<IOSNetwork> ret;

    if (interfaceWrapper)
    {
        const auto family { interfaceWrapper->family() };

        if (AF_INET == family)
        {
            ret = std::make_shared<SolarisNetworkImpl<AF_INET>>(interfaceWrapper);
        }
        else if (AF_INET6 == family)
        {
            ret = std::make_shared<SolarisNetworkImpl<AF_INET6>>(interfaceWrapper);
        }
    }
    else
    {
        throw std::runtime_error { "Error nullptr interfaceWrapper instance." };
    }

    return ret;
}

template <>
void SolarisNetworkImpl<AF_INET>::buildNetworkData(nlohmann::json& network)
{
    // Get IPv4 address
    const auto address { m_interfaceAddress->address() };

    if (!address.empty())
    {
        nlohmann::json ipv4JS { };
        ipv4JS["address"] = address;
        //ipv4JS["netmask"] = m_interfaceAddress->netmask();
        //ipv4JS["broadcast"] = m_interfaceAddress->broadcast();
        //ipv4JS["metric"] = m_interfaceAddress->metrics();
        //ipv4JS["dhcp"]   = m_interfaceAddress->dhcp();

        network["IPv4"].push_back(ipv4JS);
    }
    else
    {
        throw std::runtime_error { "Invalid IpV4 address." };
    }
}
template <>
void SolarisNetworkImpl<AF_INET6>::buildNetworkData(nlohmann::json& network)
{
    const auto address { m_interfaceAddress->addressV6() };

    if (!address.empty())
    {
        nlohmann::json ipv6JS {};
        ipv6JS["address"] = address;
        //ipv6JS["netmask"] = m_interfaceAddress->netmaskV6();
        //ipv6JS["broadcast"] = m_interfaceAddress->broadcastV6();
        //ipv6JS["metric"] = m_interfaceAddress->metricsV6();
        //ipv6JS["dhcp"]   = m_interfaceAddress->dhcp();

        network["IPv6"].push_back(ipv6JS);
    }
    else
    {
        throw std::runtime_error { "Invalid IpV6 address." };
    }
}