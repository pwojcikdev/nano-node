#include <nano/transport/tcp/tcp_service.hpp>

#include <nano/lib/thread_roles.hpp>
#include <nano/lib/utility.hpp>

#include <boost/asio/use_awaitable.hpp>

using namespace std::chrono_literals;

namespace nano::transport::tcp
{
tcp_service::tcp_service (asio::io_context & io_ctx_a, tcp_config const & config_a, nano::stats & stats_a, nano::logger & logger_a, tcp_service_params params) :
	io_ctx{ io_ctx_a },
	config{ config_a },
	stats{ stats_a },
	logger{ logger_a },
	acceptor{ io_ctx_a },
	port{ params.port },
	strand{ io_ctx.get_executor () },
	accept_task{ strand }
{
}

tcp_service::~tcp_service ()
{
	debug_assert (stopped);
	debug_assert (channels.empty ());
	debug_assert (connections.empty ());
	debug_assert (attempts.empty ());
}

void tcp_service::start ()
{
	debug_assert (!cleanup_thread.joinable ());
	debug_assert (!accept_task.joinable ());

	try
	{
		asio::ip::tcp::endpoint target{ asio::ip::address_v6::any (), port };

		acceptor.open (target.protocol ());
		acceptor.set_option (asio::ip::tcp::acceptor::reuse_address (true));
		acceptor.bind (target);
		acceptor.listen (asio::socket_base::max_listen_connections);

		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			local = acceptor.local_endpoint ();
		}

		logger.debug (nano::log::type::tcp_server, "Listening on: {}", local);
	}
	catch (boost::system::system_error const & ex)
	{
		logger.critical (nano::log::type::tcp_server, "Error binding TCP acceptor: {} (port: {})", ex.code ().message (), port);
		throw;
	}

	accept_task = nano::async::task (strand, [this] () { return run_accept (); });

	cleanup_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::tcp_listener);
		run_cleanup ();
	});
}

void tcp_service::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();

	// Close acceptor to interrupt accept loop
	boost::system::error_code ec;
	acceptor.close (ec);

	// Cancel accept task and wait
	if (accept_task.joinable ())
	{
		accept_task.cancel ();
		accept_task.join ();
	}

	// Wait for cleanup thread
	if (cleanup_thread.joinable ())
	{
		cleanup_thread.join ();
	}

	// Cancel all pending connection attempts
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		for (auto & attempt : attempts)
		{
			attempt.task.cancel ();
		}
	}

	// Wait for all attempts to complete
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		condition.wait_for (lock, 5s, [this] () {
			return attempts.empty ();
		});
		attempts.clear ();
	}

	// Clear connections and channels
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		connections.clear ();
		channels.clear ();
	}
}

bool tcp_service::connect (asio::ip::tcp::endpoint const & endpoint)
{
	nano::unique_lock<nano::mutex> lock{ mutex };

	if (stopped)
	{
		return false;
	}

	auto const ip = endpoint.address ();

	// Check max total attempts
	if (attempts.size () >= config.max_attempts)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::max_attempts, nano::stat::dir::out);
		return false;
	}

	// Check max attempts per IP
	if (count_attempts_per_ip (ip) >= config.max_attempts_per_ip)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::max_attempts_per_ip, nano::stat::dir::out);
		return false;
	}

	// Check if already attempting/connected to this endpoint
	if (attempts.get<tag_endpoint> ().count (endpoint) > 0)
	{
		return false;
	}
	if (connections.get<tag_endpoint> ().count (endpoint) > 0)
	{
		return false;
	}
	if (channels.get<tag_endpoint> ().count (endpoint) > 0)
	{
		return false;
	}

	// Apply external filter
	if (!filter (ip, connection_type::outbound))
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_rejected, nano::stat::dir::out);
		return false;
	}

	// Check internal limits
	if (auto result = check_limits (ip, connection_type::outbound); result != accept_result::accepted)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_rejected, nano::stat::dir::out);
		return false;
	}

	stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_initiate, nano::stat::dir::out);
	logger.debug (nano::log::type::tcp_server, "Initiating connection to: {}", endpoint);

	auto task = nano::async::task (strand, [this, endpoint] () { return connect_impl (endpoint); });

	attempt_entry entry{
		.endpoint = endpoint,
		.ip = ip,
		.subnetwork = ip, // TODO: map_address_to_subnetwork
		.task = std::move (task),
		.started = std::chrono::steady_clock::now ()
	};

	attempts.get<tag_endpoint> ().insert (std::move (entry));

	return true;
}

asio::awaitable<void> tcp_service::run_accept ()
{
	debug_assert (strand.running_in_this_thread ());

	while (!stopped && acceptor.is_open ())
	{
		co_await wait_available_slots ();

		if (stopped)
		{
			break;
		}

		try
		{
			auto socket = co_await accept_socket ();
			debug_assert (strand.running_in_this_thread ());

			auto result = accept_one (std::move (socket), connection_type::inbound);
			if (result != accept_result::accepted)
			{
				stats.inc (nano::stat::type::tcp_server, nano::stat::detail::accept_failure, nano::stat::dir::in);
			}
		}
		catch (boost::system::system_error const & ex)
		{
			// Expected during shutdown
			if (ex.code () != asio::error::operation_aborted)
			{
				stats.inc (nano::stat::type::tcp_server, nano::stat::detail::accept_error, nano::stat::dir::in);
				logger.debug (nano::log::type::tcp_server, "Accept error: {}", ex.code ().message ());
			}
		}

		// Small sleep to prevent busy loop
		co_await nano::async::sleep_for (10ms);
	}
}

asio::awaitable<void> tcp_service::wait_available_slots () const
{
	while (connection_count () >= config.max_inbound_connections && !stopped)
	{
		co_await nano::async::sleep_for (100ms);
	}
}

asio::awaitable<asio::ip::tcp::socket> tcp_service::accept_socket ()
{
	debug_assert (strand.running_in_this_thread ());

	asio::ip::tcp::socket socket{ strand };
	co_await acceptor.async_accept (socket, asio::use_awaitable);

	co_return socket;
}

asio::awaitable<asio::ip::tcp::socket> tcp_service::connect_socket (asio::ip::tcp::endpoint const & endpoint)
{
	debug_assert (strand.running_in_this_thread ());

	asio::ip::tcp::socket socket{ strand };
	co_await socket.async_connect (endpoint, asio::use_awaitable);

	co_return socket;
}

asio::awaitable<void> tcp_service::connect_impl (asio::ip::tcp::endpoint endpoint)
{
	debug_assert (strand.running_in_this_thread ());

	try
	{
		auto socket = co_await connect_socket (endpoint);
		debug_assert (strand.running_in_this_thread ());

		auto result = accept_one (std::move (socket), connection_type::outbound);
		if (result == accept_result::accepted)
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_success, nano::stat::dir::out);
			logger.debug (nano::log::type::tcp_server, "Connected to: {}", endpoint);
		}
		else
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_failure, nano::stat::dir::out);
		}
	}
	catch (boost::system::system_error const & ex)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_error, nano::stat::dir::out);
		logger.debug (nano::log::type::tcp_server, "Connect error to {}: {}", endpoint, ex.code ().message ());
	}

	// Remove from attempts
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		attempts.get<tag_endpoint> ().erase (endpoint);
	}
	condition.notify_all ();
}

auto tcp_service::accept_one (asio::ip::tcp::socket socket, connection_type type) -> accept_result
{
	debug_assert (strand.running_in_this_thread ());

	auto const endpoint = socket.remote_endpoint ();
	auto const ip = endpoint.address ();

	nano::lock_guard<nano::mutex> lock{ mutex };

	if (stopped)
	{
		return accept_result::rejected_filtered;
	}

	// Apply external filter
	if (!filter (ip, type))
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::connect_rejected, type == connection_type::inbound ? nano::stat::dir::in : nano::stat::dir::out);
		logger.debug (nano::log::type::tcp_server, "Connection filtered: {} ({})", endpoint, type == connection_type::inbound ? "inbound" : "outbound");
		return accept_result::rejected_filtered;
	}

	// Check internal limits
	auto result = check_limits (ip, type);
	if (result != accept_result::accepted)
	{
		return result;
	}

	// Add to connections
	connection_entry entry{
		.endpoint = endpoint,
		.ip = ip,
		.subnetwork = ip, // TODO: map_address_to_subnetwork
		.outbound = (type == connection_type::outbound),
		.started = std::chrono::steady_clock::now ()
	};

	auto [it, inserted] = connections.get<tag_endpoint> ().insert (std::move (entry));
	if (!inserted)
	{
		// Already have a connection to this endpoint
		return accept_result::rejected_filtered;
	}

	stats.inc (nano::stat::type::tcp_server, nano::stat::detail::accept_success, type == connection_type::inbound ? nano::stat::dir::in : nano::stat::dir::out);
	logger.debug (nano::log::type::tcp_server, "Accepted connection: {} ({})", endpoint, type == connection_type::inbound ? "inbound" : "outbound");

	return accept_result::accepted;
}

auto tcp_service::check_limits (asio::ip::address const & ip, connection_type type) const -> accept_result
{
	debug_assert (!mutex.try_lock ()); // Must hold mutex

	if (type == connection_type::inbound)
	{
		if (count_per_type (connection_type::inbound) >= config.max_inbound_connections)
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::max_connections, nano::stat::dir::in);
			return accept_result::rejected_max_inbound;
		}
	}

	if (type == connection_type::outbound)
	{
		if (count_per_type (connection_type::outbound) >= config.max_outbound_connections)
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::max_connections, nano::stat::dir::out);
			return accept_result::rejected_max_outbound;
		}
	}

	return accept_result::accepted;
}

std::size_t tcp_service::count_per_ip (asio::ip::address const & ip) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.get<tag_ip> ().count (ip) + channels.get<tag_ip> ().count (ip);
}

std::size_t tcp_service::count_per_subnetwork (asio::ip::address const & ip) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.get<tag_subnetwork> ().count (ip) + channels.get<tag_subnetwork> ().count (ip);
}

std::size_t tcp_service::count_per_type (connection_type type) const
{
	debug_assert (!mutex.try_lock ()); // Must hold mutex

	bool outbound = (type == connection_type::outbound);
	return std::count_if (connections.begin (), connections.end (), [outbound] (auto const & entry) {
		return entry.outbound == outbound;
	});
}

std::size_t tcp_service::count_attempts_per_ip (asio::ip::address const & ip) const
{
	debug_assert (!mutex.try_lock ()); // Must hold mutex
	return attempts.get<tag_ip> ().count (ip);
}

std::size_t tcp_service::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return channels.size ();
}

std::size_t tcp_service::connection_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return connections.size ();
}

std::size_t tcp_service::attempt_count () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return attempts.size ();
}

asio::ip::tcp::endpoint tcp_service::endpoint () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return local;
}

nano::container_info tcp_service::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	nano::container_info info;
	info.put ("channels", channels.size ());
	info.put ("connections", connections.size ());
	info.put ("attempts", attempts.size ());
	return info;
}

void tcp_service::run_cleanup ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::tcp_server, nano::stat::detail::cleanup);

		timeout ();
		purge (lock);

		debug_assert (!lock.owns_lock ());
		lock.lock ();

		condition.wait_for (lock, 1s, [this] () { return stopped.load (); });
	}
}

void tcp_service::timeout ()
{
	debug_assert (!mutex.try_lock ()); // Must hold mutex

	auto const now = std::chrono::steady_clock::now ();
	auto const connect_cutoff = now - config.connect_timeout;
	auto const handshake_cutoff = now - config.handshake_timeout;

	// Cancel timed out connection attempts
	for (auto & attempt : attempts)
	{
		if (!attempt.task.ready () && attempt.started < connect_cutoff)
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::attempt_timeout);
			logger.debug (nano::log::type::tcp_server, "Connection attempt timed out: {}", attempt.endpoint);
			attempt.task.cancel ();
		}
	}

	// Cancel timed out handshakes
	for (auto it = connections.begin (); it != connections.end (); ++it)
	{
		if (it->started < handshake_cutoff)
		{
			stats.inc (nano::stat::type::tcp_server, nano::stat::detail::handshake_timeout);
			logger.debug (nano::log::type::tcp_server, "Handshake timed out: {}", it->endpoint);
			// Mark for removal - will be purged
		}
	}
}

void tcp_service::purge (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!mutex.try_lock ());

	// Erase completed attempts
	erase_if (attempts, [] (auto const & attempt) {
		return attempt.task.ready ();
	});

	// Erase timed out connections
	auto const now = std::chrono::steady_clock::now ();
	auto const handshake_cutoff = now - config.handshake_timeout;

	erase_if (connections, [handshake_cutoff] (auto const & conn) {
		return conn.started < handshake_cutoff;
	});

	lock.unlock ();
}
}
