#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/observer_set.hpp>
#include <nano/lib/random.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/transport/tcp_config.hpp>
#include <nano/transport/common.hpp>
#include <nano/transport/fwd.hpp>
#include <nano/transport/tcp/fwd.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index_container.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>

namespace mi = boost::multi_index;

namespace nano::transport::tcp
{
struct tcp_service_params
{
	uint16_t port{ 0 };
};

class tcp_service
{
public:
	tcp_service (asio::io_context &, tcp_config const &, nano::stats &, nano::logger &, tcp_service_params = {});
	~tcp_service ();

	void start ();
	void stop ();

	// === Connection Initiation ===
	bool connect (asio::ip::tcp::endpoint const &);

	// === Channel Access (post-handshake) ===
	// std::shared_ptr<tcp_channel> find (nano::tcp_endpoint const &) const;
	// std::shared_ptr<tcp_channel> find (nano::account const & node_id) const;
	// std::deque<std::shared_ptr<tcp_channel>> list () const;
	// std::deque<std::shared_ptr<tcp_channel>> list (uint8_t min_version) const;
	// std::unordered_set<std::shared_ptr<tcp_channel>> random (std::size_t count) const;

	// === Bootstrap Support ===
	// nano::tcp_endpoint bootstrap_peer ();

	// === Keepalive ===
	// void keepalive ();
	// std::optional<nano::messages::keepalive> sample_keepalive () const;

	// === Counts ===
	std::size_t size () const; // Active channels
	std::size_t connection_count () const; // Handshaking
	std::size_t attempt_count () const; // Connecting

	// === Info ===
	asio::ip::tcp::endpoint endpoint () const; // Local listening endpoint
	nano::container_info container_info () const;

	// === Limit Checks (for external filter use) ===
	std::size_t count_per_ip (asio::ip::address const &) const;
	std::size_t count_per_subnetwork (asio::ip::address const &) const;
	std::size_t count_per_type (connection_type) const;
	std::size_t count_attempts_per_ip (asio::ip::address const &) const;

	// === Events ===
	// nano::observer_set<std::shared_ptr<tcp_channel>> channel_connected;

	// External filter for connection policy (per-IP limits, excluded peers, etc.)
	// Returns true to accept, false to reject
	// Default: accept all connections
	connection_filter filter{ [] (asio::ip::address const &, connection_type) { return true; } };

private:
	enum class accept_result
	{
		accepted,
		rejected_filtered,
		rejected_max_inbound,
		rejected_max_outbound,
		rejected_max_attempts,
	};

	// === Connection Lifecycle ===
	asio::awaitable<void> run_accept ();
	asio::awaitable<void> wait_available_slots () const;
	asio::awaitable<asio::ip::tcp::socket> accept_socket ();
	asio::awaitable<asio::ip::tcp::socket> connect_socket (asio::ip::tcp::endpoint const &);
	asio::awaitable<void> connect_impl (asio::ip::tcp::endpoint);
	accept_result accept_one (asio::ip::tcp::socket, connection_type);

	// === Limit Checks ===
	accept_result check_limits (asio::ip::address const &, connection_type) const;

	// === Cleanup ===
	void run_cleanup ();
	void timeout ();
	void purge (nano::unique_lock<nano::mutex> &);

private:
	// === Dependencies ===
	asio::io_context & io_ctx;
	tcp_config const & config;
	nano::stats & stats;
	nano::logger & logger;

	// === Listener ===
	asio::ip::tcp::acceptor acceptor;
	asio::ip::tcp::endpoint local;
	uint16_t port;

	// === Unified Storage ===

	// Handshaking connections (socket created, awaiting handshake)
	struct connection_entry
	{
		// std::shared_ptr<tcp_socket> socket;
		// std::shared_ptr<tcp_server> server;
		asio::ip::tcp::endpoint endpoint;
		asio::ip::address ip;
		asio::ip::address subnetwork;
		bool outbound;
		std::chrono::steady_clock::time_point started;

		// Extractors for multi-index
		asio::ip::tcp::endpoint get_endpoint () const
		{
			return endpoint;
		}
		asio::ip::address get_ip () const
		{
			return ip;
		}
		asio::ip::address get_subnetwork () const
		{
			return subnetwork;
		}
	};

	// clang-format off
	class tag_endpoint {};
	class tag_ip {};
	class tag_subnetwork {};

	using connections_container = boost::multi_index_container<connection_entry,
		mi::indexed_by<
			mi::hashed_unique<mi::tag<tag_endpoint>,
				mi::const_mem_fun<connection_entry, asio::ip::tcp::endpoint, &connection_entry::get_endpoint>>,
			mi::hashed_non_unique<mi::tag<tag_ip>,
				mi::const_mem_fun<connection_entry, asio::ip::address, &connection_entry::get_ip>>,
			mi::hashed_non_unique<mi::tag<tag_subnetwork>,
				mi::const_mem_fun<connection_entry, asio::ip::address, &connection_entry::get_subnetwork>>
		>>;
	// clang-format on

	connections_container connections;

	// Active channels (handshake complete)
	struct channel_entry
	{
		// std::shared_ptr<tcp_channel> channel;
		// std::shared_ptr<tcp_socket> socket;
		// std::shared_ptr<tcp_server> server;
		asio::ip::tcp::endpoint endpoint;
		nano::account node_id;
		asio::ip::address ip;
		asio::ip::address subnetwork;
		std::chrono::steady_clock::time_point last_bootstrap_attempt;

		// Extractors for multi-index
		asio::ip::tcp::endpoint get_endpoint () const
		{
			return endpoint;
		}
		nano::account get_node_id () const
		{
			return node_id;
		}
		asio::ip::address get_ip () const
		{
			return ip;
		}
		asio::ip::address get_subnetwork () const
		{
			return subnetwork;
		}
	};

	// clang-format off
	class tag_random {};
	class tag_node_id {};
	class tag_bootstrap {};

	using channels_container = boost::multi_index_container<channel_entry,
		mi::indexed_by<
			mi::random_access<mi::tag<tag_random>>,
			mi::hashed_unique<mi::tag<tag_endpoint>,
				mi::const_mem_fun<channel_entry, asio::ip::tcp::endpoint, &channel_entry::get_endpoint>>,
			mi::hashed_non_unique<mi::tag<tag_node_id>,
				mi::const_mem_fun<channel_entry, nano::account, &channel_entry::get_node_id>>,
			mi::hashed_non_unique<mi::tag<tag_ip>,
				mi::const_mem_fun<channel_entry, asio::ip::address, &channel_entry::get_ip>>,
			mi::hashed_non_unique<mi::tag<tag_subnetwork>,
				mi::const_mem_fun<channel_entry, asio::ip::address, &channel_entry::get_subnetwork>>,
			mi::ordered_non_unique<mi::tag<tag_bootstrap>,
				mi::member<channel_entry, std::chrono::steady_clock::time_point, &channel_entry::last_bootstrap_attempt>>
		>>;
	// clang-format on

	channels_container channels;

	// Pending outbound attempts (before socket connected)
	struct attempt_entry
	{
		asio::ip::tcp::endpoint endpoint;
		asio::ip::address ip;
		asio::ip::address subnetwork;
		nano::async::task task;
		std::chrono::steady_clock::time_point started;

		// Extractors for multi-index
		asio::ip::tcp::endpoint get_endpoint () const
		{
			return endpoint;
		}
		asio::ip::address get_ip () const
		{
			return ip;
		}
		asio::ip::address get_subnetwork () const
		{
			return subnetwork;
		}
	};

	// clang-format off
	using attempts_container = boost::multi_index_container<attempt_entry,
		mi::indexed_by<
			mi::hashed_unique<mi::tag<tag_endpoint>,
				mi::const_mem_fun<attempt_entry, asio::ip::tcp::endpoint, &attempt_entry::get_endpoint>>,
			mi::hashed_non_unique<mi::tag<tag_ip>,
				mi::const_mem_fun<attempt_entry, asio::ip::address, &attempt_entry::get_ip>>,
			mi::hashed_non_unique<mi::tag<tag_subnetwork>,
				mi::const_mem_fun<attempt_entry, asio::ip::address, &attempt_entry::get_subnetwork>>
		>>;
	// clang-format on

	attempts_container attempts;

	// === Async Infrastructure ===
	nano::async::strand strand;
	nano::async::task accept_task;
	std::thread cleanup_thread;

	// === Synchronization ===
	std::atomic<bool> stopped{ false };
	nano::condition_variable condition;
	mutable nano::mutex mutex;
	mutable nano::random_generator rng;
};
}
