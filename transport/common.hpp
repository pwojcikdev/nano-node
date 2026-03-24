#pragma once

#include <boost/asio/ip/address.hpp>

#include <functional>

namespace nano::transport
{
enum class connection_type
{
	inbound,
	outbound
};

// Returns true if connection should be accepted, false to reject
using connection_filter = std::function<bool (boost::asio::ip::address const &, connection_type)>;
}
