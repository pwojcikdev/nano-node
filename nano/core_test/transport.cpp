#include <nano/lib/config.hpp>
#include <nano/transport/tcp/handshake_protocol.hpp>

#include <gtest/gtest.h>

#include <unordered_map>

namespace
{
class test_handshake_provider final : public nano::transport::handshake_provider
{
public:
	explicit test_handshake_provider (bool allow_queries_a = true) :
		allow_queries{ allow_queries_a }
	{
	}

	bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) override
	{
		auto existing = cookies.find (remote_endpoint);
		if (existing == cookies.end ())
		{
			return false;
		}

		auto const cookie = existing->second;
		cookies.erase (existing);
		return response.validate (cookie);
	}

	std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const & remote_endpoint) override
	{
		if (!allow_queries)
		{
			return std::nullopt;
		}

		if (cookies.contains (remote_endpoint))
		{
			return std::nullopt;
		}

		nano::messages::node_id_handshake::query_payload query{};
		query.cookie = nano::uint256_union{ ++next_cookie };
		cookies.emplace (remote_endpoint, query.cookie);
		return query;
	}

	nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, bool v2) const override
	{
		nano::messages::node_id_handshake::response_payload response{};
		response.node_id = key.pub;
		if (v2)
		{
			nano::messages::node_id_handshake::response_payload::v2_payload payload{};
			payload.salt = nano::uint256_union{ 777 };
			payload.genesis = nano::block_hash{ 888 };
			response.v2 = payload;
		}
		response.sign (query.cookie, key);
		return response;
	}

	nano::account node_id () const
	{
		return key.pub;
	}

private:
	bool allow_queries{ true };
	mutable nano::keypair key;
	std::unordered_map<nano::endpoint, nano::uint256_union> cookies;
	uint64_t next_cookie{ 0 };
};

nano::endpoint const remote_endpoint{ boost::asio::ip::make_address_v6 ("::ffff:127.0.0.1"), 7075 };
}

TEST (transport_handshake_protocol, outbound_start_emits_query)
{
	test_handshake_provider provider;
	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, provider, remote_endpoint, nano::transport::connection_type::outbound };

	auto initial = protocol.start ();

	ASSERT_TRUE (initial.has_value ());
	ASSERT_TRUE (initial->query.has_value ());
	ASSERT_FALSE (initial->response.has_value ());
}

TEST (transport_handshake_protocol, inbound_query_produces_reply)
{
	test_handshake_provider provider;
	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, provider, remote_endpoint, nano::transport::connection_type::inbound };

	ASSERT_FALSE (protocol.start ().has_value ());

	nano::messages::node_id_handshake::query_payload incoming_query{};
	incoming_query.cookie = nano::uint256_union{ 123 };
	nano::messages::node_id_handshake incoming{ nano::dev::network_params.network, incoming_query };

	auto step = protocol.process (incoming);

	ASSERT_FALSE (step.abort);
	ASSERT_FALSE (step.remote_node_id.has_value ());
	ASSERT_TRUE (step.reply.has_value ());
	ASSERT_TRUE (step.reply->response.has_value ());
}

TEST (transport_handshake_protocol, inbound_rejects_multiple_queries)
{
	test_handshake_provider provider;
	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, provider, remote_endpoint, nano::transport::connection_type::inbound };

	nano::messages::node_id_handshake::query_payload query1{};
	query1.cookie = nano::uint256_union{ 1 };
	nano::messages::node_id_handshake::query_payload query2{};
	query2.cookie = nano::uint256_union{ 2 };

	auto first = protocol.process (nano::messages::node_id_handshake{ nano::dev::network_params.network, query1 });
	ASSERT_FALSE (first.abort);

	auto second = protocol.process (nano::messages::node_id_handshake{ nano::dev::network_params.network, query2 });
	ASSERT_TRUE (second.abort);
}

TEST (transport_handshake_protocol, outbound_completes_on_valid_response)
{
	test_handshake_provider outbound_provider;
	test_handshake_provider inbound_provider;

	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, outbound_provider, remote_endpoint, nano::transport::connection_type::outbound };

	auto initial = protocol.start ();
	ASSERT_TRUE (initial.has_value ());
	ASSERT_TRUE (initial->query.has_value ());

	auto response_payload = inbound_provider.prepare_handshake_response (*initial->query, false);
	nano::messages::node_id_handshake response{ nano::dev::network_params.network, std::nullopt, response_payload };

	auto step = protocol.process (response);

	ASSERT_FALSE (step.abort);
	ASSERT_TRUE (step.remote_node_id.has_value ());
	ASSERT_EQ (*step.remote_node_id, inbound_provider.node_id ());
}

TEST (transport_handshake_protocol, outbound_rejects_invalid_response)
{
	test_handshake_provider outbound_provider;
	test_handshake_provider inbound_provider;

	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, outbound_provider, remote_endpoint, nano::transport::connection_type::outbound };

	auto initial = protocol.start ();
	ASSERT_TRUE (initial.has_value ());

	nano::messages::node_id_handshake::query_payload wrong_query{};
	wrong_query.cookie = nano::uint256_union{ 999 };
	auto response_payload = inbound_provider.prepare_handshake_response (wrong_query, false);
	nano::messages::node_id_handshake response{ nano::dev::network_params.network, std::nullopt, response_payload };

	auto step = protocol.process (response);

	ASSERT_TRUE (step.abort);
	ASSERT_FALSE (step.remote_node_id.has_value ());
}

TEST (transport_handshake_protocol, outbound_throws_when_query_unavailable)
{
	test_handshake_provider provider{ false };
	nano::transport::tcp::handshake_protocol protocol{ nano::dev::network_params.network, provider, remote_endpoint, nano::transport::connection_type::outbound };

	ASSERT_THROW ({ auto initial = protocol.start (); }, boost::system::system_error);
}
