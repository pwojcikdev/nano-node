#include <nano/lib/keypair.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/secure/common.hpp>
#include <nano/transport/handshake_driver.hpp>
#include <nano/transport/handshake_provider.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace
{
// Stub provider — records calls, returns configurable outputs. No node, no crypto.
class stub_handshake_provider : public nano::transport::handshake_provider
{
public:
	// Inputs controlled by the test.
	bool verify_result{ true };
	std::optional<nano::messages::node_id_handshake::query_payload> query_to_return;
	nano::account response_node_id{};
	// When true, prepare_handshake_query returns nullopt.
	bool suppress_query{ false };

	// Call counts — useful to assert on behavior.
	unsigned verify_calls{ 0 };
	unsigned prepare_query_calls{ 0 };
	unsigned prepare_response_calls{ 0 };

	// Last arguments captured for assertions.
	nano::endpoint last_verify_endpoint{};

	stub_handshake_provider ()
	{
		// Default: predictable non-zero cookie so serialization works.
		nano::messages::node_id_handshake::query_payload q;
		q.cookie = nano::uint256_union{ 42 };
		query_to_return = q;
	}

	bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) override
	{
		++verify_calls;
		last_verify_endpoint = remote_endpoint;
		return verify_result;
	}

	std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const & remote_endpoint) override
	{
		++prepare_query_calls;
		if (suppress_query)
		{
			return std::nullopt;
		}
		return query_to_return;
	}

	nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version) const override
	{
		++const_cast<stub_handshake_provider *> (this)->prepare_response_calls;
		nano::messages::node_id_handshake::response_payload r;
		r.node_id = response_node_id;
		r.ext = std::monostate{};
		return r;
	}
};

nano::network_constants const & test_net ()
{
	return nano::dev::network_params.network;
}

nano::endpoint test_endpoint ()
{
	return nano::endpoint{ boost::asio::ip::address_v6::loopback (), 7075 };
}

// Build an inbound message that contains only a query.
nano::messages::node_id_handshake make_query_only (nano::uint256_union cookie = { 7 })
{
	nano::messages::node_id_handshake::query_payload q;
	q.cookie = cookie;
	return nano::messages::node_id_handshake{ test_net (), q };
}

// Build an inbound message that contains only a response.
nano::messages::node_id_handshake make_response_only (nano::account node_id = {})
{
	nano::messages::node_id_handshake::response_payload r;
	r.node_id = node_id;
	r.ext = std::monostate{};
	return nano::messages::node_id_handshake{ test_net (), std::nullopt, r };
}

// Build an inbound message that contains both (what a client sees after its own query).
nano::messages::node_id_handshake make_query_plus_response (nano::account node_id, nano::uint256_union cookie = { 9 })
{
	nano::messages::node_id_handshake::query_payload q;
	q.cookie = cookie;
	nano::messages::node_id_handshake::response_payload r;
	r.node_id = node_id;
	r.ext = std::monostate{};
	return nano::messages::node_id_handshake{ test_net (), q, r };
}

// Helper: true if the event list holds exactly one alternative of type E.
template <typename E>
bool is_single (std::vector<nano::transport::handshake_event> const & events)
{
	return events.size () == 1 && std::holds_alternative<E> (events.front ());
}
}

using nano::transport::handshake_driver;
using nano::transport::handshake_event;
using nano::transport::handshake_role;
namespace e = nano::transport::handshake_events;

// Begin ---------------------------------------------------------------------

TEST (handshake_driver, server_begin_waits_for_message)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };

	auto events = d.begin ();
	EXPECT_TRUE (is_single<e::wait_for_message> (events));
	EXPECT_FALSE (d.is_terminal ());
	// Server does not consult the provider until it receives a message.
	EXPECT_EQ (0, provider.prepare_query_calls);
}

TEST (handshake_driver, client_begin_emits_send_request_then_wait)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::client, test_endpoint () };

	auto events = d.begin ();
	ASSERT_EQ (2, events.size ());
	EXPECT_TRUE (std::holds_alternative<e::send_request> (events.at (0)));
	EXPECT_TRUE (std::holds_alternative<e::wait_for_message> (events.at (1)));
	EXPECT_FALSE (d.is_terminal ());
	EXPECT_EQ (1, provider.prepare_query_calls);

	// The emitted message should carry exactly our query and no response.
	auto const & req = std::get<e::send_request> (events.at (0));
	EXPECT_TRUE (req.message.query.has_value ());
	EXPECT_FALSE (req.message.response.has_value ());
}

TEST (handshake_driver, disabled_realtime_aborts_immediately)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint (), /*disable*/ true };

	auto events = d.begin ();
	ASSERT_TRUE (is_single<e::abort> (events));
	EXPECT_TRUE (d.is_terminal ());
}

// Server path ---------------------------------------------------------------

TEST (handshake_driver, server_query_emits_send_response_and_waits)
{
	stub_handshake_provider provider;
	nano::keypair peer;
	provider.response_node_id = peer.pub;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();

	auto events = d.on_message (make_query_only ());
	ASSERT_EQ (2, events.size ());
	auto * send = std::get_if<e::send_response> (&events.at (0));
	ASSERT_NE (send, nullptr);
	EXPECT_TRUE (std::holds_alternative<e::wait_for_message> (events.at (1)));
	EXPECT_FALSE (d.is_terminal ());

	// Response message carries both our own query and the response payload.
	EXPECT_TRUE (send->message.query.has_value ());
	EXPECT_TRUE (send->message.response.has_value ());

	EXPECT_EQ (1, provider.prepare_response_calls);
	EXPECT_EQ (1, provider.prepare_query_calls);
}

TEST (handshake_driver, server_response_only_promotes_on_valid)
{
	stub_handshake_provider provider;
	nano::keypair peer;
	provider.verify_result = true;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();

	// msg1: query → send_response
	(void)d.on_message (make_query_only ());
	// msg2: response → promote_realtime
	auto response = nano::messages::node_id_handshake::response_payload{};
	response.node_id = peer.pub;
	response.ext = std::monostate{};
	nano::messages::node_id_handshake inbound{ test_net (), std::nullopt, response };

	auto events = d.on_message (inbound);
	ASSERT_TRUE (is_single<e::promote_realtime> (events));
	auto const & promote = std::get<e::promote_realtime> (events.front ());
	EXPECT_EQ (peer.pub, promote.peer_node_id);
	EXPECT_TRUE (d.is_terminal ());
	EXPECT_EQ (1, provider.verify_calls);
	EXPECT_EQ (test_endpoint (), provider.last_verify_endpoint);
}

TEST (handshake_driver, server_response_only_aborts_on_invalid_verification)
{
	stub_handshake_provider provider;
	provider.verify_result = false;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();
	(void)d.on_message (make_query_only ());

	auto events = d.on_message (make_response_only ());
	ASSERT_TRUE (is_single<e::abort> (events));
	EXPECT_EQ ("invalid_response", std::get<e::abort> (events.front ()).reason);
	EXPECT_TRUE (d.is_terminal ());
}

// Client path ---------------------------------------------------------------

// The client sends its query first, then typically receives a query+response bundle.
// A single inbound message must emit BOTH send_response (for peer's query) AND
// promote_realtime (for the verified response).
TEST (handshake_driver, client_combined_query_and_response_emits_both)
{
	stub_handshake_provider provider;
	nano::keypair peer;
	provider.verify_result = true;
	handshake_driver d{ provider, test_net (), handshake_role::client, test_endpoint () };

	auto init = d.begin ();
	ASSERT_EQ (2, init.size ());

	auto events = d.on_message (make_query_plus_response (peer.pub));
	ASSERT_EQ (2, events.size ());
	EXPECT_TRUE (std::holds_alternative<e::send_response> (events.at (0)));
	auto const & promote = std::get<e::promote_realtime> (events.at (1));
	EXPECT_EQ (peer.pub, promote.peer_node_id);
	EXPECT_TRUE (d.is_terminal ());
}

// Error paths ---------------------------------------------------------------

TEST (handshake_driver, empty_handshake_aborts)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();

	// Build a message with neither query nor response.
	nano::messages::node_id_handshake empty{ test_net (), std::nullopt, std::nullopt };

	auto events = d.on_message (empty);
	ASSERT_TRUE (is_single<e::abort> (events));
	EXPECT_EQ ("empty_handshake", std::get<e::abort> (events.front ()).reason);
	EXPECT_TRUE (d.is_terminal ());
}

TEST (handshake_driver, second_query_is_rejected)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();
	(void)d.on_message (make_query_only ());

	// Peer sends another query instead of a response — illegal.
	auto events = d.on_message (make_query_only ());
	ASSERT_TRUE (is_single<e::abort> (events));
	EXPECT_EQ ("multiple_queries", std::get<e::abort> (events.front ()).reason);
	EXPECT_TRUE (d.is_terminal ());
}

TEST (handshake_driver, is_terminal_reflects_state_transitions)
{
	stub_handshake_provider provider;
	nano::keypair peer;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	EXPECT_FALSE (d.is_terminal ());
	(void)d.begin ();
	EXPECT_FALSE (d.is_terminal ());
	(void)d.on_message (make_query_only ());
	EXPECT_FALSE (d.is_terminal ());
	(void)d.on_message (make_response_only (peer.pub));
	EXPECT_TRUE (d.is_terminal ());
}

TEST (handshake_driver, messages_received_counts)
{
	stub_handshake_provider provider;
	handshake_driver d{ provider, test_net (), handshake_role::server, test_endpoint () };
	(void)d.begin ();
	EXPECT_EQ (0, d.messages_received ());
	(void)d.on_message (make_query_only ());
	EXPECT_EQ (1, d.messages_received ());
}
