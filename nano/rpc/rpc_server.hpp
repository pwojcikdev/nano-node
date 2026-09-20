#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/rpcconfig.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace nano
{
class rpc_connection;

/**
 * Listens for HTTP connections and serves each through a `rpc_connection`. A single coroutine on the server's
 * strand accepts connections until the server is told to stop. It then ends the connections that are not serving
 * a request, gives the others `drain_timeout` to be answered, ends what is left and finishes once the last
 * connection is gone, so nothing the server started outlives `stop`.
 * The acceptor and the set of connections are only ever touched on that strand; the only state shared between
 * threads is the pair of flags that lets any thread ask for the shutdown.
 */
class rpc_server final
{
public:
	rpc_server (std::shared_ptr<asio::io_context>, nano::rpc_config, nano::rpc_handler_interface &);
	~rpc_server ();

	// Binds the listener and starts accepting connections, throws if the port cannot be bound
	void start ();
	// Stops accepting, lets the requests being served finish within `drain_timeout` and blocks until the last connection is gone; for the owner's thread, while the IO threads run
	void stop ();
	// Begins the same shutdown without waiting for it; callable from any thread, a request handler included
	void stop_async ();

	// Port the listener is bound to, only meaningful after `start`
	std::uint16_t listening_port () const;

private:
	// The whole life of the server
	asio::awaitable<void> run ();
	// Accepts connections until the server is stopping
	asio::awaitable<void> accept_connections ();
	// Ends the idle connections, lets the others finish until the drain timeout, ends the rest, returns once none is left
	asio::awaitable<void> close_connections ();
	// Returns after a connection has ended or the deadline has passed
	asio::awaitable<void> wait_connection_ended (std::chrono::steady_clock::time_point deadline);

	void start_connection (nano::async::strand, asio::ip::tcp::socket);
	void close_acceptor ();

private: // Dependencies
	nano::rpc_config const config;
	std::shared_ptr<asio::io_context> io_ctx;
	nano::rpc_handler_interface & rpc_handler_interface;
	nano::logger logger{ "rpc" };

private:
	nano::async::strand strand;

	// Only touched on the strand
	asio::ip::tcp::acceptor acceptor;
	std::unordered_map<std::uint64_t, std::shared_ptr<nano::rpc_connection>> connections;
	std::uint64_t next_connection_id{ 0 };
	asio::steady_timer connection_ended; // Cancelled to wake `wait_connection_ended`
	bool stopping{ false };

	std::uint16_t port{ 0 }; // Written by `start` before anything runs
	std::atomic<bool> started{ false };
	std::atomic<bool> stop_requested{ false }; // The shutdown is posted to the strand at most once
	nano::async::task task;
};
}
