#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/transport/fwd.hpp>

#include <string>
#include <variant>
#include <vector>

namespace nano
{
class network_constants;
}

namespace nano::transport
{
class handshake_provider;
}

namespace nano::transport::handshake_events
{
// Driver wants the adapter to send its initial query (client-side only).
struct send_request
{
	nano::messages::node_id_handshake message;
};

// Driver wants the adapter to send a response (bundled with our own query).
struct send_response
{
	nano::messages::node_id_handshake message;
};

// Handshake succeeded — promote the connection to realtime with these credentials.
struct promote_realtime
{
	nano::account peer_node_id;
	nano::node_capabilities_flags peer_flags;
};

// Legacy bootstrap promotion. Kept for symmetry with the original state machine;
// currently unreachable from the driver (bootstrap listener has been removed).
struct promote_bootstrap
{
};

// Driver is waiting for the next handshake message from the peer.
struct wait_for_message
{
};

// Terminal failure. `reason` is a short human-readable tag for logs/stats.
struct abort
{
	std::string reason;
};
}

namespace nano::transport
{
using handshake_event = std::variant<
handshake_events::send_request,
handshake_events::send_response,
handshake_events::promote_realtime,
handshake_events::promote_bootstrap,
handshake_events::wait_for_message,
handshake_events::abort>;

enum class handshake_role
{
	server,
	client,
};

/**
 * Pure handshake state machine. Holds no sockets, no strands, no node reference.
 *
 * Usage:
 *   handshake_driver d { provider, net, role, remote };
 *   for (auto const & e : d.begin()) act_on(e);
 *   while (!d.is_terminal()) {
 *       auto msg = read_next_handshake_message();
 *       for (auto const & e : d.on_message(msg)) act_on(e);
 *   }
 *
 * `on_message()` can emit up to two events from a single inbound message: a
 * `send_response` (for the peer's query) followed by a `promote_realtime` /
 * `abort` (for the peer's response verification). The emission order matches
 * the original `tcp_server::process_handshake` sequence.
 */
class handshake_driver
{
public:
	handshake_driver (nano::transport::handshake_provider &,
	nano::network_constants const &,
	handshake_role,
	nano::endpoint remote,
	bool disable_tcp_realtime = false);

	/// Initial events for this role. Client → {send_request, wait_for_message}
	/// (unless the provider declined to produce a query, in which case → abort).
	/// Server → {wait_for_message}.
	std::vector<handshake_event> begin ();

	/// Feed a received handshake message. May emit up to two events.
	std::vector<handshake_event> on_message (nano::messages::node_id_handshake const &);

	bool is_terminal () const
	{
		return terminal;
	}

	// Number of peer messages fed into on_message so far.
	std::size_t messages_received () const
	{
		return received_count;
	}

private:
	nano::transport::handshake_provider & provider;
	nano::network_constants const & network;
	handshake_role role;
	nano::endpoint remote;
	bool disable_tcp_realtime;

	// Becomes true after the first peer message is processed. A second query
	// after this point is illegal (only a response is permitted).
	bool first_message_seen{ false };
	std::size_t received_count{ 0 };
	bool terminal{ false };

	// Cap peer messages at 2 (matches original two-step loop). If we still
	// aren't promoted after that, the driver aborts.
	static constexpr std::size_t max_messages = 2;
};
}
