#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/node_id_handshake.hpp>

#include <optional>

namespace nano::transport
{
/**
 * Port: owns node-id handshake policy and state.
 *
 * Verifies incoming handshake responses and prepares the query/response
 * payloads used during TCP connection setup.
 */
class handshake_handler
{
public:
	virtual ~handshake_handler () = default;

	/// Verify that a remote response matches our outstanding query for this
	/// endpoint. Returns true when the handshake may continue.
	virtual auto verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) -> bool = 0;

	/// Prepare a query payload for an outgoing handshake, or `std::nullopt` if
	/// no query should be sent for this endpoint.
	virtual auto prepare_handshake_query (nano::endpoint const & remote_endpoint) -> std::optional<nano::messages::node_id_handshake::query_payload> = 0;

	/// Prepare the response payload for an incoming handshake query using the
	/// negotiated handshake version.
	virtual auto prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version) const -> nano::messages::node_id_handshake::response_payload = 0;
};
}
