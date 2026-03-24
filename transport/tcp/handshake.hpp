#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>

#include <optional>

namespace nano::transport::tcp
{
class handshake_provider
{
public:
	virtual ~handshake_provider () = default;

	virtual std::optional<nano::messages::node_id_handshake::query_payload> prepare_query (nano::endpoint const & remote_endpoint) = 0;
	virtual nano::messages::node_id_handshake::response_payload prepare_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version, nano::endpoint const & remote_endpoint) = 0;
	virtual bool verify_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) = 0;
};
}
