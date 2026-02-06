#pragma once

#include <boost/asio.hpp>

namespace asio = boost::asio;

namespace nano::transport
{
class block_deserializer;
class channel;
class handshake_provider;
class loopback_channel;
class message_deserializer;
class tcp_channel;
class tcp_acceptor;
class tcp_channels;
class tcp_listener;
class tcp_server;
class tcp_socket;
class transport_service;
}
