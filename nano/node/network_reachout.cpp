#include <nano/lib/container_info.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/network_formatting.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>
#include <nano/messages/keepalive.hpp>
#include <nano/node/network.hpp>
#include <nano/node/network_reachout.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/peer_history.hpp>
#include <nano/node/transport/tcp_listener.hpp>
#include <nano/node/transport/transport.hpp>

#include <boost/asio/use_future.hpp>

using namespace std::chrono_literals;

namespace
{
auto constexpr preconfigured_log_interval = 1h;
}

nano::network_reachout::network_reachout (nano::node_config const & config_a, nano::node_flags const & flags_a, nano::network & network_a, nano::transport::tcp_listener & tcp_listener_a, nano::peer_history & peer_history_a, boost::asio::io_context & io_ctx_a, nano::stats & stats_a, nano::logger & logger_a) :
	config{ config_a },
	flags{ flags_a },
	network{ network_a },
	tcp_listener{ tcp_listener_a },
	peer_history{ peer_history_a },
	stats{ stats_a },
	logger{ logger_a },
	strand{ io_ctx_a.get_executor () },
	resolver{ strand },
	live_enabled{ !flags.disable_reachout && config.network->peer_reachout.count () > 0 },
	cached_enabled{ !flags.disable_reachout && config.network->cached_peer_reachout.count () > 0 },
	preconfigured_enabled{ !flags.disable_reachout_preconfigured }
{
	for (auto const & peer : config.preconfigured_peers)
	{
		preconfigured_logs.emplace (peer, peer_log{});
	}
}

nano::network_reachout::~network_reachout ()
{
	debug_assert (!thread.joinable ());
	debug_assert (tasks.empty ());
}

void nano::network_reachout::start ()
{
	debug_assert (!thread.joinable ());

	if (!live_enabled)
	{
		logger.warn (nano::log::type::network, "Peer reachout is disabled");
	}
	if (!cached_enabled)
	{
		logger.warn (nano::log::type::network, "Cached peer reachout is disabled");
	}

	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::network_reachout);
		run ();
	});
}

void nano::network_reachout::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	nano::join_or_pass (thread);

	// Nothing starts a reachout once the thread is joined and `stopped` is set, so these are the last tasks
	decltype (tasks) tasks_l;
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		tasks_l.swap (tasks);
	}
	if (!tasks_l.empty ())
	{
		for (auto & task : tasks_l)
		{
			task.cancel ();
		}
		// The resolver has no per-operation cancellation, so a pending resolve is cancelled through it, on the strand that uses it
		asio::dispatch (strand, asio::use_future ([this] () {
			resolver.cancel ();
		}))
		.wait ();
		for (auto & task : tasks_l)
		{
			task.join ();
		}
	}
}

void nano::network_reachout::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait_for (lock, config.network_params.network.merge_period, [this] { return stopped || triggered; });
		if (stopped)
		{
			return;
		}
		bool const triggered_l = std::exchange (triggered, false);
		purge ();
		lock.unlock ();

		if (live_enabled)
		{
			stats.inc (nano::stat::type::network, nano::stat::detail::loop_reachout);
			reachout_live ();
		}
		if (cached_enabled)
		{
			stats.inc (nano::stat::type::network, nano::stat::detail::loop_reachout_cached);
			reachout_cached ();
		}
		if (preconfigured_enabled)
		{
			stats.inc (nano::stat::type::network, nano::stat::detail::loop_reachout_preconfigured);
			if (triggered_l)
			{
				preconfigured_interval.reset ();
			}
			auto const target_interval = network.empty ()
			? config.network_params.network.reachout_preconfigured_warmup_period
			: config.network_params.network.reachout_preconfigured_period;
			if (preconfigured_interval.elapse (target_interval))
			{
				reachout_preconfigured ();
			}
		}

		lock.lock ();
	}
}

void nano::network_reachout::reachout_live ()
{
	if (live_peers.empty ())
	{
		if (auto keepalive = network.tcp_channels.sample_keepalive ())
		{
			live_peers.assign (keepalive->peers.begin (), keepalive->peers.end ());
		}
	}
	if (!live_peers.empty ())
	{
		stats.inc (nano::stat::type::network, nano::stat::detail::reachout_live);
		network.merge_peer (live_peers.front ());
		live_peers.pop_front ();
	}
}

void nano::network_reachout::reachout_cached ()
{
	if (cached_peers.empty ())
	{
		auto const peers = peer_history.peers ();
		cached_peers.assign (peers.begin (), peers.end ());
	}
	if (!cached_peers.empty ())
	{
		stats.inc (nano::stat::type::network, nano::stat::detail::reachout_cached);
		network.merge_peer (cached_peers.front ());
		cached_peers.pop_front ();
	}
}

void nano::network_reachout::reachout_preconfigured ()
{
	if (config.preconfigured_peers.empty ())
	{
		return;
	}

	stats.inc (nano::stat::type::network, nano::stat::detail::reachout_preconfigured);

	for (auto const & peer : config.preconfigured_peers)
	{
		reachout (peer, config.network_params.network.default_node_port);
	}
}

void nano::network_reachout::trigger ()
{
	stats.inc (nano::stat::type::network, nano::stat::detail::trigger_reachout);
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		triggered = true;
	}
	condition.notify_all ();
}

void nano::network_reachout::reachout (std::string const & address, uint16_t port)
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	if (stopped)
	{
		return;
	}
	tasks.emplace_back (strand, reachout_impl (address, port));
}

void nano::network_reachout::purge ()
{
	debug_assert (!mutex.try_lock ());
	tasks.remove_if ([] (auto const & task) {
		return task.ready ();
	});
}

asio::awaitable<void> nano::network_reachout::reachout_impl (std::string address, uint16_t port)
{
	debug_assert (strand.running_in_this_thread ());

	auto const [ec, results] = co_await resolver.async_resolve (address, std::to_string (port), asio::as_tuple (asio::use_awaitable));
	if (co_await nano::async::cancelled ())
	{
		co_return;
	}
	if (ec)
	{
		logger.error (nano::log::type::network, "Error resolving address for reachout: {}:{} ({})", address, port, ec.message ());
		co_return;
	}

	for (auto const & result : results)
	{
		auto const endpoint = nano::transport::map_endpoint_to_v6 (result.endpoint ());
		if (auto channel = network.find_channel (endpoint))
		{
			network.send_keepalive (channel);
		}
		else
		{
			auto const connect_ec = co_await tcp_listener.co_connect (endpoint);
			if (co_await nano::async::cancelled ())
			{
				co_return;
			}
			log_outcome (address, endpoint, connect_ec);
		}
	}
}

void nano::network_reachout::log_outcome (std::string const & address, nano::tcp_endpoint const & endpoint, std::error_code ec)
{
	debug_assert (strand.running_in_this_thread ());

	auto it = preconfigured_logs.find (address);
	if (it == preconfigured_logs.end ())
	{
		return;
	}
	auto & log = it->second;
	if (ec)
	{
		// A failure to reach an explicitly configured peer is reported so misconfiguration is visible
		if (log.failure.elapse (preconfigured_log_interval))
		{
			logger.warn (nano::log::type::network, "Failed to connect to preconfigured peer '{}' ({}): {}", address, endpoint, ec.message ());
		}
	}
	else if (log.success.elapse (preconfigured_log_interval))
	{
		logger.info (nano::log::type::network, "Connected to preconfigured peer '{}' ({})", address, endpoint);
	}
}

nano::container_info nano::network_reachout::container_info () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };

	nano::container_info info;
	info.put ("tasks", tasks.size ());
	return info;
}
