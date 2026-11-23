#include <nano/lib/container_info.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/dependency_strategy.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

#include <optional>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
dependency_strategy::dependency_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a },
	lookups{ ctx_a.config.lookup }
{
}

void dependency_strategy::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_dependency_walker);
		run ();
	});

	debug_assert (!sync_thread.joinable ());
	sync_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_dependency_sync);
		run_sync ();
	});

	debug_assert (!lookup_thread.joinable ());
	lookup_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_lookup);
		run_lookups ();
	});
}

void dependency_strategy::stop ()
{
	nano::join_or_pass (thread);
	nano::join_or_pass (sync_thread);
	nano::join_or_pass (lookup_thread);
}

void dependency_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_dependencies);
		run_one ();
		lock.lock ();
	}
}

void dependency_strategy::run_one ()
{
	// No need to wait for block_processor, as we are not processing blocks
	auto channel = ctx.wait_channel (strategy::dependency);
	if (!channel)
	{
		return;
	}
	auto blocking = wait_blocking ();
	if (blocking.is_zero ())
	{
		return;
	}
	request_info (blocking, channel);
}

nano::block_hash dependency_strategy::next_blocking ()
{
	debug_assert (!ctx.mutex.try_lock ());

	auto blocking = ctx.accounts.next_blocking ([this] (nano::block_hash const & hash) {
		return ctx.count_tags (hash, strategy::dependency) == 0;
	});
	if (blocking.is_zero ())
	{
		return { 0 };
	}
	ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_blocking);
	return blocking;
}

nano::block_hash dependency_strategy::wait_blocking ()
{
	auto result = ctx.wait_result ([this] () -> std::optional<nano::block_hash> {
		auto blocking = next_blocking ();
		if (blocking.is_zero ())
		{
			return std::nullopt;
		}
		return blocking;
	});
	return result.value_or (nano::block_hash{ 0 });
}

bool dependency_strategy::request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel)
{
	account_info_query query{};
	query.target = hash;

	return ctx.send (channel, query, strategy::dependency);
}

bool dependency_strategy::process (nano::messages::asc_pull_ack::account_info_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::account_info_by_hash);
	debug_assert (!tag.hash.is_zero ());

	if (response.account.is_zero ())
	{
		ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info_empty);

		// The peer does not know the block, so a looked up block is asked for elsewhere
		lookup_failed (tag, lookup_failure::empty);

		return true; // OK, but nothing to do
	}

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info);

	// Record the account that holds this block for every blocked account waiting on it
	ctx.accounts.dependency_update (tag.hash, response.account);

	if (lookups.erase (tag.hash))
	{
		// The block is known to exist, so its account is pulled ahead of anything else
		// Erased first, as setting a priority leaves an account that is prioritized already as it is
		ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::resolved);
		ctx.logger.debug (nano::log::type::bootstrap, "Looked up block: {} (account: {})", tag.hash, response.account);

		ctx.accounts.priority_erase (response.account);
		ctx.accounts.priority_set (response.account, account_sets_index::priority_max);
	}
	else
	{
		// Prioritize account containing the dependency, at the lowest possible priority
		ctx.accounts.priority_set (response.account, account_sets_index::priority_cutoff);
	}

	return true; // OK, no way to verify the response
}

void dependency_strategy::timeout (async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());

	lookup_failed (tag, lookup_failure::timeout);
}

void dependency_strategy::failure (async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());

	lookup_failed (tag, lookup_failure::invalid);
}

void dependency_strategy::run_sync ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		// Reinsert known dependencies into the priority set
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::sync_dependencies);
		auto synced = ctx.accounts.sync_dependencies ();
		ctx.logger.debug (nano::log::type::bootstrap, "Synced {} dependencies", synced);
		ctx.condition.wait_for (lock, nano::is_dev_run () ? 500ms : 60s, [this] () { return ctx.stopped; });
	}
}

/*
 * Lookups
 */

void dependency_strategy::lookup (nano::block_hash const & hash)
{
	debug_assert (!ctx.mutex.try_lock ());

	// Nothing would look the block up or forget it
	if (!enabled ())
	{
		return;
	}

	auto const result = lookups.insert (hash, std::chrono::steady_clock::now ());
	ctx.stats.inc (nano::stat::type::bootstrap_lookup_insert, to_stat_detail (result));

	ctx.logger.debug (nano::log::type::bootstrap, "Missing block to look up: {} (result: {})", hash, nano::enum_to_string (result));
}

void dependency_strategy::run_lookups ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::loop);
		run_lookup_one ();
		lock.lock ();
	}
}

void dependency_strategy::run_lookup_one ()
{
	// Wait for a block that is due for a lookup
	auto const request = ctx.wait_result ([this] () {
		return lookups.next (std::chrono::steady_clock::now ());
	});
	if (!request)
	{
		return; // Stopped
	}

	// A block that reached the ledger in the meantime needs no lookup
	if (in_ledger (request->hash))
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		lookups.erase (request->hash);
		ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::old);
		return;
	}

	// Any peer will do, as long as it has not failed to find this block already
	auto const acquired = ctx.wait_channels (strategy::dependency, {}, request->exclude, /* max */ 1);

	std::shared_ptr<nano::transport::channel> channel;
	auto const id = nano::bootstrap::generate_id ();
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };

		if (acquired.exhausted)
		{
			// Every peer has been asked, one of them may have received the block since
			lookups.reset_peers (request->hash);
			ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::exhausted);
		}

		for (auto const & lease : acquired.leases)
		{
			if (lookups.dispatch (request->hash, id, lease.node_id))
			{
				channel = lease.channel;
			}
			else
			{
				// The block stopped waiting for a lookup while a peer was being found
				ctx.peers.release (lease.channel);
				ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::stale);
			}
		}
	}
	if (!channel)
	{
		return;
	}

	ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::stat::detail::request);

	account_info_query query{};
	query.target = request->hash;
	ctx.send (channel, query, strategy::dependency, id);
}

void dependency_strategy::lookup_failed (async_tag const & tag, lookup_failure reason)
{
	debug_assert (!ctx.mutex.try_lock ());

	// Requests for the dependencies of blocked accounts are retried by the walker instead
	if (!lookups.contains (tag.hash))
	{
		return;
	}

	ctx.stats.inc (nano::stat::type::bootstrap_lookup, nano::enum_convert<nano::stat::detail> (reason));

	auto const result = lookups.fail (tag.hash, tag.id, std::chrono::steady_clock::now ());
	ctx.stats.inc (nano::stat::type::bootstrap_lookup_fail, to_stat_detail (result));

	if (result == lookup_index::fail_result::exhausted)
	{
		ctx.logger.debug (nano::log::type::bootstrap, "Giving up on looking up block: {}", tag.hash);
	}
}

bool dependency_strategy::enabled () const
{
	// Without bootstrap as a whole no strategy runs
	if (!ctx.config.enable)
	{
		return false;
	}

	return ctx.config.enable_dependency_walker;
}

bool dependency_strategy::in_ledger (nano::block_hash const & hash) const
{
	auto transaction = ctx.ledger.tx_begin_read ();
	return ctx.ledger.any.block_exists_or_pruned (transaction, hash);
}

void dependency_strategy::reset ()
{
	debug_assert (!ctx.mutex.try_lock ());

	lookups.clear ();
}

nano::container_info dependency_strategy::container_info () const
{
	debug_assert (!ctx.mutex.try_lock ());

	nano::container_info info;
	info.add ("lookups", lookups.container_info ());
	return info;
}
}
