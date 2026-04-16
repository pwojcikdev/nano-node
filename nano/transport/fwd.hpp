#pragma once

namespace nano::transport
{
class channel;
class channel_events;
class channel_factory;
class connector;
class loopback_channel;
class message_deserializer;
class message_sink;
class peer_policy;
class tcp_channel;
class tcp_channels;
class tcp_server;
class tcp_service;
class tcp_socket;
class transport_service;
struct transport_context;
}

namespace nano::transport::fake
{
class channel;
}

namespace nano::transport::inproc
{
class channel;
}
