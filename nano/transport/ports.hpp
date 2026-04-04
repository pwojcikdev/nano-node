#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/messages/fwd.hpp>
#include <nano/transport/fwd.hpp>

#include <boost/asio/ip/address.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace nano::transport
{
/**
 * Port: delivers inbound messages to the node.
 */
class message_sink
{
public:
	virtual ~message_sink () = default;

	/// Async delivery: message is handed off to a queue. Returns true if accepted.
	virtual bool enqueue (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::channel>) = 0;

	/// Synchronous inbound processing — used by the loopback channel to deliver
	/// locally-generated messages back into the node without a queue.
	virtual void process (nano::messages::message const &, std::shared_ptr<nano::transport::channel>) = 0;
};

/**
 * Port: peer-management decisions.
 *
 * Consolidates `is_excluded`, `is_not_a_peer`, `bootstrap_count`, `random_fill`.
 * All of these are read-only queries about the node's view of its peer set.
 */
class peer_policy
{
public:
	virtual ~peer_policy () = default;

	/// True if this address is on the exclusion list (misbehaved, banned, etc.).
	virtual bool is_excluded (boost::asio::ip::address const &) = 0;

	/// True if this endpoint is us or otherwise not a valid peer candidate.
	virtual bool is_not_a_peer (nano::endpoint const &) = 0;

	/// Current number of active bootstrap-kind connections.
	virtual std::size_t bootstrap_count () = 0;

	/// Populate the array with up to 8 randomly-chosen known peers. Used when
	/// responding to keepalive requests.
	virtual void random_fill (std::array<nano::endpoint, 8> &) = 0;
};

/**
 * Port: initiates outbound TCP connections on behalf of the transport layer.
 */
class connector
{
public:
	virtual ~connector () = default;

	/// Request an outbound connection attempt. Returns true if the attempt was
	/// queued; the result of the handshake is reported via channel_events.
	virtual bool connect (boost::asio::ip::address, std::uint16_t port) = 0;
};

/**
 * Port: notifies interested parties about channel-lifecycle events.
 *
 * Currently only surfaces `channel_connected` — the listener's own
 * `connection_accepted` observer is kept separate for now (it also publishes
 * raw sockets, which is outside the port's remit).
 */
class channel_events
{
public:
	virtual ~channel_events () = default;

	virtual void on_connected (std::shared_ptr<nano::transport::channel>) = 0;
};

/**
 * Port: creates and registers a realtime tcp_channel.
 *
 * This factory both constructs the channel *and* stores it in tcp_channels.
 * Tests that want a standalone `tcp_server` can supply a stub returning a
 * `nullptr` or a prebuilt fake channel.
 */
class channel_factory
{
public:
	virtual ~channel_factory () = default;

	/// Returns the created channel, or nullptr if registration was rejected
	/// (e.g. duplicate node_id, exceeded per-peer limits).
	virtual std::shared_ptr<nano::transport::tcp_channel> create (
	std::shared_ptr<nano::transport::tcp_socket> const &,
	std::shared_ptr<nano::transport::tcp_server> const &,
	nano::account const & node_id,
	nano::node_capabilities_flags)
	= 0;
};
}
