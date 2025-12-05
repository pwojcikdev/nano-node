#pragma once

#include <nano/boost/asio/ip/tcp.hpp>

#include <optional>
#include <string>

namespace nano
{
using ip_address = boost::asio::ip::address;
using endpoint = boost::asio::ip::basic_endpoint<boost::asio::ip::tcp>;
using tcp_endpoint = endpoint; // TODO: Remove this alias
}

namespace nano
{
enum class database_backend
{
	lmdb,
	rocksdb
};

std::string to_string (database_backend);
std::optional<database_backend> parse_database_backend (std::string);
}
