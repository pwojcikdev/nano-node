#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/beast/http.hpp>
#include <boost/property_tree/ptree.hpp>

#include <cstdint>
#include <memory>

namespace nano::test
{
struct rpc_call_state;

/*
 * Handle to an RPC call in flight. Value type, cheap to copy; copies observe the same call.
 * Safe to drop at any time: the async operation owns its state and completes independently.
 */
class rpc_response
{
public:
	// Call has completed, successfully or not
	bool finished () const;
	// Call completed with HTTP 200 and a parseable JSON body
	bool ok () const;
	// Parsed JSON body, valid once ok ()
	boost::property_tree::ptree const & json () const;
	// Raw HTTP response for inspecting headers, valid once finished ()
	boost::beast::http::response<boost::beast::http::string_body> const & raw () const;

private:
	explicit rpc_response (std::shared_ptr<rpc_call_state> state);

	std::shared_ptr<rpc_call_state> state;

	friend rpc_response rpc_post (boost::property_tree::ptree const & request, uint16_t port, boost::asio::io_context & io_ctx);
};

// Post a JSON request to the RPC server listening on the local port; pump the io context until finished ()
rpc_response rpc_post (boost::property_tree::ptree const & request, uint16_t port, boost::asio::io_context & io_ctx);
}
