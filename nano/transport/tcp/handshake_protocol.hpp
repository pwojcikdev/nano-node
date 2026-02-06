#pragma once

#include <nano/lib/constants.hpp>
#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>
#include <nano/transport/common.hpp>
#include <nano/transport/handshake.hpp>

#include <optional>

namespace nano::transport::tcp
{
class handshake_protocol
{
public:
	static int constexpr max_handshake_messages = 2;
	enum class error
	{
		none,
		invalid_message,
		duplicate_query,
		invalid_response,
	};

	struct step_result
	{
		std::optional<nano::messages::node_id_handshake> reply;
		std::optional<nano::account> remote_node_id;
		bool abort{ false };
		error failure{ error::none };
	};

	handshake_protocol (nano::network_constants const &, handshake_provider &, nano::endpoint const &, connection_type);

	std::optional<nano::messages::node_id_handshake> start () const;
	step_result process (nano::messages::node_id_handshake const &);

private:
	nano::messages::node_id_handshake make_query_message () const;
	nano::messages::node_id_handshake make_response_message (nano::messages::node_id_handshake const & incoming) const;

private:
	nano::network_constants const & network_constants;
	handshake_provider & handshake;
	nano::endpoint const remote_endpoint;
	connection_type const connection;
	bool query_received{ false };
};
}
