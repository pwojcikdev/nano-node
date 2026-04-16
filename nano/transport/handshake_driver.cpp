#include <nano/lib/networks.hpp>
#include <nano/transport/handshake_driver.hpp>
#include <nano/transport/handshake_provider.hpp>

nano::transport::handshake_driver::handshake_driver (
nano::transport::handshake_provider & provider_a,
nano::network_constants const & network_a,
handshake_role role_a,
nano::endpoint remote_a,
bool disable_tcp_realtime_a) :
	provider{ provider_a },
	network{ network_a },
	role{ role_a },
	remote{ remote_a },
	disable_tcp_realtime{ disable_tcp_realtime_a }
{
}

auto nano::transport::handshake_driver::begin () -> std::vector<handshake_event>
{
	if (disable_tcp_realtime)
	{
		terminal = true;
		return { handshake_events::abort{ "realtime_disabled" } };
	}

	if (role == handshake_role::client)
	{
		auto query = provider.prepare_handshake_query (remote);
		nano::messages::node_id_handshake msg{ network, query };
		return { handshake_events::send_request{ std::move (msg) }, handshake_events::wait_for_message{} };
	}

	// Server side just waits for the peer to send the first query.
	return { handshake_events::wait_for_message{} };
}

auto nano::transport::handshake_driver::on_message (nano::messages::node_id_handshake const & message) -> std::vector<handshake_event>
{
	if (terminal)
	{
		// Shouldn't happen in practice — adapter is expected to stop feeding us.
		return { handshake_events::abort{ "already_terminal" } };
	}

	if (disable_tcp_realtime)
	{
		terminal = true;
		return { handshake_events::abort{ "realtime_disabled" } };
	}

	if (!message.query && !message.response)
	{
		terminal = true;
		return { handshake_events::abort{ "empty_handshake" } };
	}

	if (message.query && first_message_seen)
	{
		// A second message with a query is not allowed — only a response is valid here.
		terminal = true;
		return { handshake_events::abort{ "multiple_queries" } };
	}

	first_message_seen = true;
	++received_count;

	std::vector<handshake_event> events;

	// Peer sent us a query → respond with our own response (+ optionally our own query).
	if (message.query)
	{
		auto response = provider.prepare_handshake_response (*message.query, message.version ());
		auto own_query = provider.prepare_handshake_query (remote);
		nano::messages::node_id_handshake reply{ network, own_query, response };
		events.push_back (handshake_events::send_response{ std::move (reply) });
	}

	// Peer sent us a response → verify and promote.
	if (message.response)
	{
		if (provider.verify_handshake_response (*message.response, remote))
		{
			terminal = true;
			events.push_back (handshake_events::promote_realtime{
			message.response->node_id,
			message.response->flags () });
			return events;
		}
		else
		{
			terminal = true;
			events.push_back (handshake_events::abort{ "invalid_response" });
			return events;
		}
	}

	// Still in handshake — need another message from the peer.
	if (received_count >= max_messages)
	{
		// We've used up the two-step budget without promotion. Abort.
		terminal = true;
		events.push_back (handshake_events::abort{ "handshake_incomplete" });
		return events;
	}

	events.push_back (handshake_events::wait_for_message{});
	return events;
}
