#include <nano/transport/tcp/handshake_protocol.hpp>

namespace nano::transport::tcp
{
handshake_protocol::handshake_protocol (nano::network_constants const & network_constants_a, handshake_provider & handshake_a, nano::endpoint const & remote_endpoint_a, connection_type connection_a) :
	network_constants{ network_constants_a },
	handshake{ handshake_a },
	remote_endpoint{ remote_endpoint_a },
	connection{ connection_a }
{
}

std::optional<nano::messages::node_id_handshake> handshake_protocol::start () const
{
	if (connection == connection_type::outbound)
	{
		return make_query_message ();
	}
	return std::nullopt;
}

auto handshake_protocol::process (nano::messages::node_id_handshake const & incoming) -> step_result
{
	step_result result;
	if (!incoming.query && !incoming.response)
	{
		result.abort = true;
		result.failure = error::invalid_message;
		return result;
	}

	if (incoming.query)
	{
		if (query_received)
		{
			result.abort = true;
			result.failure = error::duplicate_query;
			return result;
		}
		query_received = true;
		result.reply = make_response_message (incoming);
	}

	if (incoming.response)
	{
		if (handshake.verify_handshake_response (*incoming.response, remote_endpoint))
		{
			result.remote_node_id = incoming.response->node_id;
		}
		else
		{
			result.abort = true;
			result.failure = error::invalid_response;
		}
	}

	return result;
}

nano::messages::node_id_handshake handshake_protocol::make_query_message () const
{
	auto query = handshake.prepare_handshake_query (remote_endpoint);
	if (!query)
	{
		throw boost::system::system_error{ boost::system::error_code{ boost::system::errc::protocol_error, boost::system::system_category () } };
	}

	return nano::messages::node_id_handshake{ network_constants, query };
}

nano::messages::node_id_handshake handshake_protocol::make_response_message (nano::messages::node_id_handshake const & incoming) const
{
	debug_assert (incoming.query.has_value ());

	auto response = handshake.prepare_handshake_response (*incoming.query, incoming.is_v2 ());
	auto own_query = handshake.prepare_handshake_query (remote_endpoint);
	return nano::messages::node_id_handshake{ network_constants, own_query, response };
}
}
