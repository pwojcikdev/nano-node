#include <nano/lib/logging.hpp>
#include <nano/lib/network_formatting.hpp>
#include <nano/lib/rpc_handler_interface.hpp>
#include <nano/lib/utility.hpp>
#include <nano/rpc/rpc_connection.hpp>
#include <nano/rpc/rpc_server.hpp>

#include <chrono>
#include <stdexcept>

using namespace std::chrono_literals;

nano::rpc_server::rpc_server (std::shared_ptr<asio::io_context> io_ctx_a, nano::rpc_config config_a, nano::rpc_handler_interface & rpc_handler_interface_a) :
	config{ std::move (config_a) },
	io_ctx{ std::move (io_ctx_a) },
	rpc_handler_interface{ rpc_handler_interface_a },
	strand{ io_ctx->get_executor () },
	acceptor{ strand },
	connection_ended{ strand },
	task{ strand }
{
	rpc_handler_interface.rpc_instance (*this);
}

nano::rpc_server::~rpc_server ()
{
	stop ();
}

void nano::rpc_server::start ()
{
	debug_assert (!task.joinable ());

	auto endpoint (asio::ip::tcp::endpoint (asio::ip::make_address_v6 (config.address), config.port));

	bool const is_loopback = (endpoint.address ().is_loopback () || (endpoint.address ().to_v6 ().is_v4_mapped () && asio::ip::make_address_v4 (asio::ip::v4_mapped, endpoint.address ().to_v6 ()).is_loopback ()));
	if (!is_loopback && config.enable_control)
	{
		logger.warn (nano::log::type::rpc, "WARNING: Control-level RPCs are enabled on non-local address {}, potentially allowing wallet access outside local computer", endpoint.address ());
	}

	// Nothing else can reach the acceptor until the coroutine below is spawned
	acceptor.open (endpoint.protocol ());
	acceptor.set_option (asio::ip::tcp::acceptor::reuse_address (true));

	boost::system::error_code ec;
	acceptor.bind (endpoint, ec);
	if (ec)
	{
		logger.critical (nano::log::type::rpc, "Error while binding for RPC on port: {} ({})", endpoint.port (), ec.message ());
		throw std::runtime_error (ec.message ());
	}
	port = acceptor.local_endpoint ().port ();
	logger.info (nano::log::type::rpc, "RPC listening address: {}", acceptor.local_endpoint ());
	acceptor.listen ();

	started = true;
	task = nano::async::task (strand, run ());
}

void nano::rpc_server::stop ()
{
	stop_async ();
	if (task.joinable ())
	{
		task.join ();
	}
}

void nano::rpc_server::stop_async ()
{
	// Posted once and only to a started server, so `stop` cannot return before this has run and `this` stays valid
	if (!started || stop_requested.exchange (true))
	{
		return;
	}
	asio::dispatch (strand, [this] () {
		stopping = true;
		close_acceptor (); // Completes the accept in flight, which sends `accept_connections` on its way out
	});
}

void nano::rpc_server::close_acceptor ()
{
	debug_assert (strand.running_in_this_thread ());
	if (acceptor.is_open ())
	{
		boost::system::error_code ec;
		acceptor.close (ec);
		if (ec)
		{
			logger.error (nano::log::type::rpc, "Error while closing RPC acceptor during shutdown: {}", ec.message ());
		}
	}
}

std::uint16_t nano::rpc_server::listening_port () const
{
	return port;
}

asio::awaitable<void> nano::rpc_server::run ()
{
	debug_assert (strand.running_in_this_thread ());
	try
	{
		co_await accept_connections ();
	}
	catch (std::exception const & ex)
	{
		// A failing connection never gets here, those are contained; the node carries on without its RPC server
		logger.error (nano::log::type::rpc, "RPC server stopped accepting connections after an unexpected error: {}", ex.what ());
	}
	catch (...)
	{
		logger.error (nano::log::type::rpc, "RPC server stopped accepting connections after an unknown error");
	}
	stopping = true;
	close_acceptor ();
	co_await close_connections ();
	debug_assert (connections.empty ());
}

asio::awaitable<void> nano::rpc_server::accept_connections ()
{
	while (!stopping)
	{
		nano::async::strand connection_strand{ io_ctx->get_executor () };
		auto [ec, socket] = co_await acceptor.async_accept (connection_strand, asio::as_tuple (asio::use_awaitable));
		debug_assert (strand.running_in_this_thread ());

		if (stopping)
		{
			break; // A connection accepted while the server was stopping is dropped here, which closes it
		}

		bool failed{ false };
		if (!ec)
		{
			try
			{
				start_connection (std::move (connection_strand), asio::ip::tcp::socket{ std::move (socket) });
			}
			catch (std::exception const & ex)
			{
				// One connection that cannot be set up must not end the listener
				logger.error (nano::log::type::rpc, "Error starting RPC connection: {}", ex.what ());
				failed = true;
			}
		}
		else
		{
			logger.error (nano::log::type::rpc, "Error accepting RPC connection: {}", ec.message ());
			failed = true;
		}
		if (failed)
		{
			co_await nano::async::sleep_for (100ms); // An error that persists must not turn into a busy loop
		}
	}
}

void nano::rpc_server::start_connection (nano::async::strand connection_strand, asio::ip::tcp::socket socket)
{
	debug_assert (strand.running_in_this_thread ());

	auto const id = next_connection_id++;
	auto connection = std::make_shared<nano::rpc_connection> (std::move (connection_strand), std::move (socket), config, rpc_handler_interface, logger);
	connections.emplace (id, connection);
	try
	{
		// The server outlives this: `run` does not finish before every connection has reported its end
		connection->start ([this, id] () {
			asio::dispatch (strand, [this, id] () {
				connections.erase (id);
				connection_ended.cancel ();
			});
		});
	}
	catch (...)
	{
		connections.erase (id); // It never started, so it will never report its end
		throw;
	}
}

asio::awaitable<void> nano::rpc_server::close_connections ()
{
	debug_assert (strand.running_in_this_thread ());

	for (auto const & [id, connection] : connections)
	{
		connection->cancel ();
	}
	while (!connections.empty ())
	{
		co_await wait_connection_ended ();
	}
}

asio::awaitable<void> nano::rpc_server::wait_connection_ended ()
{
	// Checking for connections and waiting here happen on the strand with nothing in between, so no wakeup is missed
	debug_assert (strand.running_in_this_thread ());
	connection_ended.expires_at (std::chrono::steady_clock::time_point::max ());
	co_await connection_ended.async_wait (asio::as_tuple (asio::use_awaitable));
}
