#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/network_filter.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/messages/fwd.hpp>
#include <nano/transport/bandwidth_limiter.hpp>
#include <nano/transport/fwd.hpp>
#include <nano/transport/handshake_provider.hpp>
#include <nano/transport/ports.hpp>
#include <nano/transport/tcp_config.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>

#include <array>
#include <functional>
#include <memory>

namespace nano::transport
{
/**
 * Pure dependency container for transport components.
 * All transport classes receive a reference to this instead of nano::node&.
 * The node creates and wires this at construction time.
 */
struct transport_context
{
	boost::asio::io_context & io_ctx;
	nano::network_params const & network_params;
	nano::stats & stats;
	nano::logger & logger;
	nano::transport::tcp_config const & tcp_config;
	nano::transport::bandwidth_limiter & outbound_limiter;

	// Optional dependencies injected by node
	nano::network_filter * network_filter{};
	nano::block_uniquer * block_uniquer{};
	nano::vote_uniquer * vote_uniquer{};
	nano::transport::handshake_provider * handshake{};
	nano::transport::message_sink * message_sink{};
	nano::transport::peer_policy * peer_policy{};
	nano::transport::connector * connector{};
	nano::transport::channel_events * channel_events{};

	struct
	{
		bool disable_tcp_realtime{};
		bool disable_bootstrap_listener{};
		bool disable_max_peers_per_ip{};
		bool disable_max_peers_per_subnetwork{};
		bool allow_local_peers{};
	} flags;

	// Remaining callback. `create_channel` is deferred to a later step because it
	// couples closely with tcp_channels' lifecycle (the factory both creates and
	// registers channels). Will be replaced by a channel_factory port.
	std::function<std::shared_ptr<nano::transport::tcp_channel> (std::shared_ptr<nano::transport::tcp_socket> const &, std::shared_ptr<nano::transport::tcp_server> const &, nano::account const &, nano::node_capabilities_flags)> create_channel;
};
}
