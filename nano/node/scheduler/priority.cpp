#include <nano/lib/blocks.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/bucketing.hpp>
#include <nano/node/election.hpp>
#include <nano/node/ledger_notifications.hpp>
#include <nano/node/node.hpp>
#include <nano/node/scheduler/priority.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/secure/ledger_set_confirmed.hpp>

nano::scheduler::priority::priority (nano::node_config & node_config, nano::node & node_a, nano::ledger & ledger_a, nano::ledger_notifications & ledger_notifications_a, nano::bucketing & bucketing_a, nano::active_elections & active_a, nano::cementing_set & cementing_set_a, nano::stats & stats_a, nano::logger & logger_a) :
	config{ node_config.priority_scheduler },
	node{ node_a },
	ledger{ ledger_a },
	ledger_notifications{ ledger_notifications_a },
	bucketing{ bucketing_a },
	active{ active_a },
	cementing_set{ cementing_set_a },
	stats{ stats_a },
	logger{ logger_a },
	pool{ config.max_blocks, config.reserved_blocks },
	workers{ 2, nano::thread_role::name::scheduler_priority_activations }
{
	if (!config.enable)
	{
		return;
	}

	// React to AEC events
	active.election_started.add ([this] (auto const & election, nano::bucket_index bucket, nano::priority_timestamp priority) {
		nano::lock_guard<nano::mutex> lock{ mutex };

		if (election->behavior () == nano::election_behavior::priority)
		{
			elections.insert (election, bucket, priority);
		}
	});

	// React to AEC events
	active.election_erased.add ([this] (auto const & election) {
		nano::lock_guard<nano::mutex> lock{ mutex };
		elections.erase (election);
	});

	// Activate accounts with fresh blocks
	ledger_notifications.blocks_processed.add ([this] (auto const & batch) {
		std::deque<nano::account> accounts;
		for (auto const & [result, context] : batch)
		{
			if (result == nano::block_status::progress)
			{
				accounts.push_back (context.block->account ());
			}
		}
		if (!accounts.empty ())
		{
			if (workers.queued_tasks () >= nano::queue_warning_threshold () && warning_interval.elapse (15s))
			{
				node.logger.warn (nano::log::type::election_scheduler, "Notification queue has {} tasks", workers.queued_tasks ());
			}

			// Schedule activation of accounts on the background thread
			workers.post ([this, accounts = std::move (accounts)] () {
				auto transaction = ledger.tx_begin_read ();
				for (auto const & account : accounts)
				{
					activate (transaction, account);
				}
			});
		}
	});

	if (node.flags.disable_activate_successors)
	{
		return;
	}

	// Activate successors of cemented blocks
	cementing_set.batch_cemented.add ([this] (auto const & batch) {
		std::deque<nano::account> accounts;
		for (auto const & ctx : batch)
		{
			auto const & block = ctx.block;

			accounts.push_back (block->account ());

			// Activate the next unconfirmed block in the destination account
			if (block->is_send () && !block->destination ().is_zero () && block->destination () != block->account ())
			{
				accounts.push_back (block->destination ());
			}
		}
		if (!accounts.empty ())
		{
			if (workers.queued_tasks () >= nano::queue_warning_threshold () && warning_interval.elapse (15s))
			{
				node.logger.warn (nano::log::type::election_scheduler, "Notification queue has {} tasks", workers.queued_tasks ());
			}

			// Schedule activation of accounts on the background thread
			workers.post ([this, accounts = std::move (accounts)] () {
				auto transaction = ledger.tx_begin_read ();
				for (auto const & account : accounts)
				{
					activate (transaction, account);
				}
			});
		}
	});
}

nano::scheduler::priority::~priority ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::scheduler::priority::start ()
{
	debug_assert (!thread.joinable ());

	if (!config.enable)
	{
		return;
	}

	workers.start ();

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::scheduler_priority);
		run ();
	} };
}

void nano::scheduler::priority::stop ()
{
	{
		nano::lock_guard<nano::mutex> lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();
	join_or_pass (thread);

	workers.stop ();
}

void nano::scheduler::priority::notify (int64_t vacancy)
{
	condition.notify_all ();
}

bool nano::scheduler::priority::activate (secure::transaction const & transaction, nano::account const & account)
{
	debug_assert (!account.is_zero ());
	if (auto info = ledger.any.account_get (transaction, account))
	{
		nano::confirmation_height_info conf_info;
		ledger.store.confirmation_height.get (transaction, account, conf_info);
		if (conf_info.height < info->block_count)
		{
			return activate (transaction, account, *info, conf_info);
		}
	}
	stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::activate_skip);
	return false; // Not activated
}

bool nano::scheduler::priority::activate (secure::transaction const & transaction, nano::account const & account, nano::account_info const & account_info, nano::confirmation_height_info const & conf_info)
{
	debug_assert (conf_info.frontier != account_info.head);

	auto const hash = conf_info.height == 0 ? account_info.open_block : ledger.any.block_successor (transaction, conf_info.frontier).value_or (0);
	auto const block = ledger.any.block_get (transaction, hash);
	if (!block)
	{
		return false; // Not activated
	}

	if (ledger.dependents_confirmed (transaction, *block))
	{
		auto const [priority_balance, priority_timestamp] = ledger.block_priority (transaction, *block);
		auto const bucket_index = bucketing.bucket_index (priority_balance);

		bool added = false;
		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			added = pool.push (block, bucket_index, priority_timestamp);
		}
		if (added)
		{
			stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::activated);
			logger.trace (nano::log::type::election_scheduler, nano::log::detail::block_activated,
			nano::log::arg{ "account", account },
			nano::log::arg{ "block", block },
			nano::log::arg{ "time", account_info.modified },
			nano::log::arg{ "priority_balance", priority_balance },
			nano::log::arg{ "priority_timestamp", priority_timestamp });

			condition.notify_all ();
		}
		else
		{
			stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::activate_full);
		}
		return added;
	}

	stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::activate_failed);
	return false; // Not activated
}

bool nano::scheduler::priority::contains (nano::block_hash const & hash) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return pool.contains (hash);
}

bool nano::scheduler::priority::contains (std::shared_ptr<nano::election> const & election) const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return elections.contains (election);
}

std::size_t nano::scheduler::priority::size () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return pool.size ();
}

bool nano::scheduler::priority::empty () const
{
	nano::lock_guard<nano::mutex> lock{ mutex };
	return pool.empty ();
}

bool nano::scheduler::priority::predicate () const
{
	debug_assert (!mutex.try_lock ());

	// Check if any bucket has blocks and available election slots
	auto tops = pool.top_all ();
	for (auto const & [bucket_index, entry] : tops)
	{
		if (bucket_activate_predicate (bucket_index, entry.priority))
		{
			return true;
		}
	}

	// Check if any bucket is overfilled and needs to cancel elections
	auto sizes = elections.sizes ();
	for (auto const & [bucket_index, size] : sizes)
	{
		if (size > 0 && bucket_overfill_predicate (bucket_index))
		{
			return true;
		}
	}

	return false;
}

bool nano::scheduler::priority::bucket_activate_predicate (nano::bucket_index bucket, nano::priority_timestamp candidate_timestamp) const
{
	debug_assert (!mutex.try_lock ());

	auto count = elections.size (bucket);

	// Always have space for reserved elections
	if (count < config.reserved_elections)
	{
		return true;
	}

	// Allow up to max_elections if global vacancy allows
	if (count < config.max_elections)
	{
		return active.vacancy (nano::election_behavior::priority) > 0;
	}

	// Check if new priority is better than the lowest priority (highest value) in bucket
	if (auto lowest = elections.worst (bucket))
	{
		// Compare to equal to drain duplicates
		if (candidate_timestamp <= lowest->priority)
		{
			// Bound number of reprioritizations
			return count < config.max_elections * 2;
		}
	}

	return false;
}

bool nano::scheduler::priority::bucket_overfill_predicate (nano::bucket_index bucket) const
{
	debug_assert (!mutex.try_lock ());

	auto count = elections.size (bucket);

	// Always have space for reserved elections
	if (count <= config.reserved_elections)
	{
		return false;
	}

	// Allow up to 2x max_elections if there is space in AEC
	if (count <= config.max_elections)
	{
		return active.vacancy (nano::election_behavior::priority) < 0;
	}

	return true; // Otherwise start cancelling elections
}

bool nano::scheduler::priority::activate_bucket (nano::unique_lock<nano::mutex> & lock, nano::bucket_index bucket)
{
	debug_assert (!mutex.try_lock ());
	debug_assert (lock.owns_lock ());

	auto block_opt = pool.pop (bucket);
	release_assert (block_opt);
	auto const & [block, bucket_index, priority] = *block_opt;

	lock.unlock ();

	auto result = active.insert (block, nano::election_behavior::priority, bucket_index, priority);

	if (result.inserted)
	{
		stats.inc (nano::stat::type::election_bucket, nano::stat::detail::activate_success);
		return true;
	}
	else
	{
		stats.inc (nano::stat::type::election_bucket, nano::stat::detail::activate_failed);
		return false;
	}
}

void nano::scheduler::priority::run_one (nano::unique_lock<nano::mutex> & lock)
{
	debug_assert (!mutex.try_lock ());
	debug_assert (lock.owns_lock ());

	// Activate buckets with available candidates
	auto tops = pool.top_all ();
	for (auto const & [bucket_index, entry] : tops)
	{
		if (bucket_activate_predicate (bucket_index, entry.priority))
		{
			activate_bucket (lock, bucket_index);
			debug_assert (!lock.owns_lock ());
			lock.lock ();
		}
	}

	// Cancel elections in overfilled buckets
	std::deque<std::shared_ptr<nano::election>> to_cancel;

	auto sizes = elections.sizes ();
	for (auto const & [bucket_index, size] : sizes)
	{
		if (size > 0 && bucket_overfill_predicate (bucket_index))
		{
			// Get the worst election (largest priority value)
			auto worst = elections.worst (bucket_index);
			debug_assert (worst.has_value ());
			if (worst)
			{
				to_cancel.push_back (worst->election);
			}
		}
	}

	if (!to_cancel.empty ())
	{
		lock.unlock ();

		for (auto const & election : to_cancel)
		{
			logger.debug (nano::log::type::election_scheduler, "Cancelling overfill election for root: {} with blocks: {} (behavior: {}, state: {}, voters: {}, blocks: {}, duration: {}ms)",
			election->qualified_root,
			fmt::join (election->blocks_hashes (), ", "), // TODO: Lazy eval
			to_string (election->behavior ()),
			to_string (election->state ()),
			election->voter_count (),
			election->block_count (),
			election->duration ().count ());

			bool cancelled = election->cancel ();

			stats.inc (nano::stat::type::election_scheduler, cancelled ? nano::stat::detail::cancel_lowest : nano::stat::detail::cancel_failed);
		}

		lock.lock ();
	}
}

void nano::scheduler::priority::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] () {
			return stopped || predicate ();
		});

		if (stopped)
		{
			return;
		}

		debug_assert ((std::this_thread::yield (), true)); // Introduce some random delay in debug builds

		stats.inc (nano::stat::type::election_scheduler, nano::stat::detail::loop);

		run_one (lock);
		debug_assert (lock.owns_lock ());
	}
}

nano::container_info nano::scheduler::priority::container_info () const
{
	nano::container_info info;
	info.add ("pool", pool.container_info ());
	info.add ("elections", elections.container_info ());
	return info;
}

/*
 * priority_config
 */

nano::error nano::scheduler::priority_config::serialize (nano::tomlconfig & toml) const
{
	toml.put ("enable", enable, "Enable priority scheduler. \nType: bool");
	toml.put ("max_blocks", max_blocks, "Total shared pool size across all buckets. \nType: uint64");
	toml.put ("reserved_blocks", reserved_blocks, "Reserved blocks per bucket. \nType: uint64");
	toml.put ("reserved_elections", reserved_elections, "Guaranteed election slots per bucket. \nType: uint64");
	toml.put ("max_elections", max_elections, "Maximum election slots per bucket when AEC has space. \nType: uint64");

	return toml.get_error ();
}

nano::error nano::scheduler::priority_config::deserialize (nano::tomlconfig & toml)
{
	toml.get ("enable", enable);
	toml.get ("max_blocks", max_blocks);
	toml.get ("reserved_blocks", reserved_blocks);
	toml.get ("reserved_elections", reserved_elections);
	toml.get ("max_elections", max_elections);

	return toml.get_error ();
}