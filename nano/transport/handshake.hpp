#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>

#include <optional>

namespace nano::transport
{
class handshake_provider
{
public:
	virtual ~handshake_provider () = default;

	virtual bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const &, nano::endpoint const & remote_endpoint) = 0;
	virtual std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const & remote_endpoint) = 0;
	virtual nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const &, bool v2) const = 0;
};
}
