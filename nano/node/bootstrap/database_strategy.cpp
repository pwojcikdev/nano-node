#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/node/bootstrap/database_strategy.hpp>
#include <nano/node/nodeconfig.hpp>

namespace nano::bootstrap
{
database_strategy::database_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a }
{
}

void database_strategy::start ()
{
	debug_assert (!thread.joinable ());
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_database_scan);
		run ();
	});
}

void database_strategy::stop ()
{
	nano::join_or_pass (thread);
}

void database_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		// Avoid high churn rate of database requests
		bool should_throttle = !ctx.database_scan.warmed_up () && ctx.throttle.throttled ();
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_database);
		run_one (should_throttle);
		lock.lock ();
	}
}

void database_strategy::run_one (bool should_throttle)
{
	ctx.wait_block_processor ();
	auto channel = ctx.wait_channel ();
	if (!channel)
	{
		return;
	}
	auto account = wait_database (should_throttle);
	if (account.is_zero ())
	{
		return;
	}
	ctx.request (account, 2, channel, query_source::database);
}

nano::account database_strategy::next_database (bool should_throttle)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (ctx.config.database_warmup_ratio > 0);

	// Throttling increases the weight of database requests
	if (!ctx.database_limiter.should_pass (should_throttle ? ctx.config.database_warmup_ratio : 1))
	{
		return { 0 };
	}
	auto account = ctx.database_scan.next ([this] (nano::account const & account) {
		return ctx.count_tags (account, query_source::database) == 0;
	});
	if (account.is_zero ())
	{
		return { 0 };
	}
	ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_database);
	return account;
}

nano::account database_strategy::wait_database (bool should_throttle)
{
	nano::account result{ 0 };
	ctx.wait ([this, &result, should_throttle] () {
		debug_assert (!ctx.mutex.try_lock ());
		result = next_database (should_throttle);
		if (!result.is_zero ())
		{
			return true;
		}
		return false;
	});
	return result;
}
}
