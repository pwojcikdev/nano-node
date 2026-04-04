#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/messages.hpp>

#include <optional>

namespace nano::transport
{
/**
 * Abstract interface for handshake operations.
 */
class handshake_provider
{
public:
	virtual ~handshake_provider () = default;

	virtual auto verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) -> bool = 0;
	virtual auto prepare_handshake_query (nano::endpoint const & remote_endpoint) -> std::optional<nano::messages::node_id_handshake::query_payload> = 0;
	virtual auto prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version) const -> nano::messages::node_id_handshake::response_payload = 0;
};
}
