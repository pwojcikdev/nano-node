#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bootstrap/database_strategy.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/transport/channel.hpp>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
database_strategy::database_strategy (service & ctx_a) :
	ctx{ ctx_a },
	pulls{ ctx_a.strand }
{
}

asio::awaitable<void> database_strategy::run ()
{
	debug_assert (ctx.strand.running_in_this_thread ());

	try
	{
		co_await run_requests ();
	}
	catch (boost::system::system_error const & ex)
	{
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	co_await asio::this_coro::reset_cancellation_state ();
	pulls.cancel ();
	co_await pulls.join ();

	inflight.clear ();
}

asio::awaitable<void> database_strategy::run_requests ()
{
	while (true)
	{
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_database);

		co_await ctx.wait_block_processor ();

		auto budget = co_await ctx.request_budget.acquire ();
		co_await ctx.wait_limiter (ctx.limiter, 1);
		auto channel = co_await ctx.wait_channel ();

		// Avoid high churn rate of database requests: throttling increases the request cost while
		// the ledger scan is unproductive and the database has not yet been fully crawled
		bool const should_throttle = !ctx.database_scan.warmed_up () && ctx.throttle.throttled ();
		debug_assert (ctx.config.database_warmup_ratio > 0);
		co_await ctx.wait_limiter (ctx.database_limiter, should_throttle ? ctx.config.database_warmup_ratio : 1);

		// The scan refills its queue from the ledger internally; this is blocking store IO, but it was
		// equally blocking under the old shared mutex, so strand residence is parity, not a regression
		std::optional<blocks_query> query;
		while (!query)
		{
			query = ctx.database_scan.next ([this] (nano::account const & account) {
				return !inflight.contains (account);
			});
			if (!query)
			{
				co_await ctx.clock.sleep_for (100ms);
			}
		}

		ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_database);

		// The database scan always issues safe requests; record the pull start point
		ctx.stats.inc (nano::stat::type::bootstrap_database, query->type == query_type::blocks_by_hash ? nano::stat::detail::from_confirmed : nano::stat::detail::from_open);

		inflight.insert (query->account);
		pulls.spawn (run_pull (*query, channel, std::move (budget)));
	}
}

asio::awaitable<void> database_strategy::run_pull (blocks_query query, std::shared_ptr<nano::transport::channel> channel, nano::async::semaphore::token budget)
{
	nano::scope_guard guard{ [this, account = query.account] () {
		inflight.erase (account);
	} };

	co_await ctx.run_pull (channel, query, query_source::database, std::move (budget));
}
}
