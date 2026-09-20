#pragma once

#include <nano/boost/beast/core.hpp>
#include <nano/boost/beast/http.hpp>
#include <nano/lib/async.hpp>
#include <nano/lib/fwd.hpp>

#include <functional>
#include <memory>
#include <string>

namespace nano
{
/**
 * One accepted HTTP connection, served from start to end by a single coroutine on a strand of its own: it reads
 * one request, hands it to the backend, writes the response and closes. Nothing but that coroutine touches the
 * socket, and the backend, which answers through a callback from a thread of its choosing, runs off the strand.
 */
class rpc_connection final : public std::enable_shared_from_this<nano::rpc_connection>
{
public:
	rpc_connection (nano::async::strand, asio::ip::tcp::socket, nano::rpc_config const &, nano::rpc_handler_interface &, nano::logger &);

	// Starts serving; `on_done` is called on the connection's strand once the connection has ended
	void start (std::function<void ()> on_done);

	// Ends the connection wherever it is, callable from any thread
	void cancel ();

private:
	using request_type = boost::beast::http::request<boost::beast::http::string_body>;
	using response_type = boost::beast::http::response<boost::beast::http::string_body>;

	// The whole life of the connection
	asio::awaitable<void> run ();
	// Reads one request and writes its response
	asio::awaitable<void> serve ();
	// Hands a POST to the backend and awaits its response body
	asio::awaitable<std::string> process (request_type const &);
	// Writes the response and ends the sending side; a peer that is gone is not an error
	asio::awaitable<void> write (response_type);

	response_type make_response (unsigned version, std::string body) const;
	std::string request_id () const;

private: // Dependencies
	nano::rpc_config const & config;
	nano::rpc_handler_interface & rpc_handler_interface;
	nano::logger & logger;

private:
	nano::async::strand strand;
	boost::beast::tcp_stream stream; // Only touched by the coroutine
	nano::async::cancellation cancellation;
};
}
