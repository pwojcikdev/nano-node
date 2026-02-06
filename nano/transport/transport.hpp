#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/lib/stats.hpp>

#include <boost/asio/ip/network_v6.hpp>

#include <map>
#include <memory>

namespace nano::transport
{
nano::endpoint map_endpoint_to_v6 (nano::endpoint const &);
nano::endpoint map_tcp_to_endpoint (nano::tcp_endpoint const &);
nano::tcp_endpoint map_endpoint_to_tcp (nano::endpoint const &);
boost::asio::ip::address map_address_to_subnetwork (boost::asio::ip::address);
boost::asio::ip::address ipv4_address_or_ipv6_subnet (boost::asio::ip::address);
boost::asio::ip::address_v6 mapped_from_v4_bytes (unsigned long);
boost::asio::ip::address_v6 mapped_from_v4_or_v6 (boost::asio::ip::address const &);
bool is_ipv4_or_v4_mapped_address (boost::asio::ip::address const &);
bool is_same_ip (boost::asio::ip::address const &, boost::asio::ip::address const &);
bool is_same_subnetwork (boost::asio::ip::address const &, boost::asio::ip::address const &);

// Unassigned, reserved, self
bool reserved_address (nano::endpoint const &, bool allow_local_peers = false);

// The value type is intentionally opaque: only key distribution matters for subnetwork counting.
using address_socket_mmap = std::multimap<boost::asio::ip::address, std::weak_ptr<void>>;

namespace socket_functions
{
	boost::asio::ip::network_v6 get_ipv6_subnet_address (boost::asio::ip::address_v6 const &, std::size_t);
	boost::asio::ip::address first_ipv6_subnet_address (boost::asio::ip::address_v6 const &, std::size_t);
	boost::asio::ip::address last_ipv6_subnet_address (boost::asio::ip::address_v6 const &, std::size_t);
	std::size_t count_subnetwork_connections (nano::transport::address_socket_mmap const &, boost::asio::ip::address_v6 const &, std::size_t);
}

void throw_if_error (boost::system::error_code const & ec);
}

namespace nano
{
nano::stat::detail to_stat_detail (boost::system::error_code const &);
}
