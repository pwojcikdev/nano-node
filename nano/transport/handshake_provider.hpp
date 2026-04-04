#pragma once

#include <nano/lib/endpoint.hpp>
#include <nano/messages/messages.hpp>

#include <optional>

namespace nano::transport
{
/**
 * Abstract interface for handshake operations.
 * The node-side (nano::network) implements this and injects it into transport_context.
 */
class handshake_provider
{
public:
	virtual ~handshake_provider () = default;

	virtual std::optional<nano::messages::node_id_handshake::query_payload> prepare_handshake_query (nano::endpoint const & remote_endpoint) = 0;
	virtual nano::messages::node_id_handshake::response_payload prepare_handshake_response (nano::messages::node_id_handshake::query_payload const & query, nano::messages::handshake_version version) const = 0;
	/** Verifies that handshake response matches our query. @returns true if OK */
	virtual bool verify_handshake_response (nano::messages::node_id_handshake::response_payload const & response, nano::endpoint const & remote_endpoint) = 0;
};
}
