#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/election.hpp>
#include <nano/node/endpoint.hpp>
#include <nano/node/local_vote_history.hpp>
#include <nano/node/network.hpp>
#include <nano/node/node.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/request_aggregator.hpp>
#include <nano/node/vote_router.hpp>
#include <nano/node/voting/vote_replier.hpp>
#include <nano/node/wallet.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_cemented.hpp>

nano::request_aggregator::request_aggregator (request_aggregator_config const & config_a, nano::node & node_a, nano::vote_replier & replier_a, nano::local_vote_history & history_a, nano::ledger & ledger_a, nano::wallets & wallets_a, nano::vote_router & vote_router_a) :
	config{ config_a },
	node_config{ node_a.config },
	network_constants{ node_a.network_params.network },
	local_votes (history_a),
	ledger (ledger_a),
	wallets (wallets_a),
	vote_router{ vote_router_a },
	replier (replier_a),
	stats (node_a.stats),
	logger (node_a.logger)
{
	queue.max_size_query = [this] (auto const & origin) {
		return config.max_queue;
	};
	queue.priority_query = [] (auto const & origin) {
		return 1;
	};

	// Disable if voting is disabled
	if (!node_config.enable_voting)
	{
		enabled = false;
	}
}

nano::request_aggregator::~request_aggregator ()
{
	debug_assert (threads.empty ());
}

void nano::request_aggregator::start ()
{
	debug_assert (threads.empty ());

	if (!enabled)
	{
		return;
	}

	for (auto i = 0; i < config.threads; ++i)
	{
		threads.emplace_back ([this] () {
			nano::thread_role::set (nano::thread_role::name::request_aggregator);
			run ();
		});
	}
}

void nano::request_aggregator::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	for (auto & thread : threads)
	{
		if (thread.joinable ())
		{
			thread.join ();
		}
	}
	threads.clear ();
}

std::size_t nano::request_aggregator::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return queue.size ();
}

bool nano::request_aggregator::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return queue.empty ();
}

bool nano::request_aggregator::request (request_type const & request, std::shared_ptr<nano::transport::channel> const & channel)
{
	release_assert (channel != nullptr);
	debug_assert (!request.empty ());

	// Do not accept requests if disabled
	if (!enabled.load (std::memory_order_relaxed))
	{
		return false;
	}

	bool added = false;
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		added = queue.push ({ request, channel }, { nano::no_value{}, channel });
	}
	if (added)
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::request);
		stats.add (nano::stat::type::request_aggregator, nano::stat::detail::request_hashes, request.size ());

		condition.notify_one ();
	}
	else
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::overfill);
		stats.add (nano::stat::type::request_aggregator, nano::stat::detail::overfill_hashes, request.size ());
	}

	// TODO: This stat is for compatibility with existing tests and is in principle unnecessary
	stats.inc (nano::stat::type::aggregator, added ? nano::stat::detail::aggregator_accepted : nano::stat::detail::aggregator_dropped);

	return added;
}

void nano::request_aggregator::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::loop);

		if (!queue.empty ())
		{
			// Only log if component is under pressure
			if (queue.size () > nano::queue_warning_threshold () && log_interval.elapse (15s))
			{
				logger.info (nano::log::type::request_aggregator, "{} requests in processing queue", queue.size ());
			}

			run_batch (lock);
			debug_assert (!lock.owns_lock ());
			lock.lock ();
		}
		else
		{
			condition.wait (lock, [&] { return stopped || !queue.empty (); });
		}
	}
}

void nano::request_aggregator::run_batch (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (lock.owns_lock ());
	debug_assert (!mutex.try_lock ());
	debug_assert (!queue.empty ());

	debug_assert (config.batch_size > 0);
	auto batch = queue.next_batch (config.batch_size);

	lock.unlock ();

	auto transaction = ledger.tx_begin_read ();

	for (auto const & [value, origin] : batch)
	{
		auto const & [request, channel] = value;

		transaction.refresh_if_needed ();

		if (!channel->max (nano::transport::traffic_type::vote_reply))
		{
			process (transaction, request, channel);
		}
		else
		{
			stats.inc (nano::stat::type::request_aggregator, nano::stat::detail::channel_full, stat::dir::out);
		}
	}
}

void nano::request_aggregator::process (nano::secure::transaction const & transaction, request_type const & request, std::shared_ptr<nano::transport::channel> const & channel)
{
	replier.reply (transaction, request, channel);
}

void nano::request_aggregator::erase_duplicates (std::vector<std::pair<nano::block_hash, nano::root>> & requests_a) const
{
	std::sort (requests_a.begin (), requests_a.end (), [] (auto const & pair1, auto const & pair2) {
		return pair1.first < pair2.first;
	});
	requests_a.erase (std::unique (requests_a.begin (), requests_a.end (), [] (auto const & pair1, auto const & pair2) {
		return pair1.first == pair2.first;
	}),
	requests_a.end ());
}

nano::container_info nano::request_aggregator::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.add ("queue", queue.container_info ());
	return info;
}

/*
 * request_aggregator_config
 */

nano::error nano::request_aggregator_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("max_queue", max_queue, "Maximum number of queued requests per peer. \ntype:uint64");
	toml.put ("threads", threads, "Number of threads for request processing. \ntype:uint64");
	toml.put ("batch_size", batch_size, "Number of requests to process in a single batch. \ntype:uint64");

	return toml.get_error ();
}

nano::error nano::request_aggregator_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("max_queue", max_queue);
	toml.get ("threads", threads);
	toml.get ("batch_size", batch_size);

	return toml.get_error ();
}
