#pragma once

#include <nano/boost/asio/ip/tcp.hpp>

namespace nano
{
using ip_address = boost::asio::ip::address;
using endpoint = boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>;
using tcp_endpoint = endpoint; // TODO: Remove this alias
}
