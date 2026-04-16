#pragma once

#include <nano/lib/endpoint.hpp>
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
 * Port: delivers inbound messages and new channels to the node.
 *
 * This is the typed replacement for `transport_context::on_message` and
 * `transport_context::process_inbound`. `nano::node` supplies a concrete
 * adapter that forwards into `message_processor` / `inbound()`.
 */
class message_sink
{
public:
	virtual ~message_sink () = default;

	/// Async delivery: message is handed off to a queue. Returns true if accepted.
	virtual bool on_message (std::unique_ptr<nano::messages::message>, std::shared_ptr<nano::transport::channel>) = 0;

	/// Synchronous inbound processing — used by the loopback channel to deliver
	/// locally-generated messages back into the node without a queue.
	virtual void process_inbound (nano::messages::message const &, std::shared_ptr<nano::transport::channel>) = 0;
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
}
