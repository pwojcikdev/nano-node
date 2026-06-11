#include <nano/lib/config.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/dependency_strategy.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/formatting.hpp>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
dependency_strategy::dependency_strategy (service & ctx_a) :
	ctx{ ctx_a },
	children{ ctx_a.strand }
{
}

asio::awaitable<void> dependency_strategy::run ()
{
	debug_assert (ctx.strand.running_in_this_thread ());

	children.spawn (run_requests ());
	children.spawn (run_sync ());

	try
	{
		co_await children.join (); // Runs until cancelled
	}
	catch (boost::system::system_error const & ex)
	{
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	co_await asio::this_coro::reset_cancellation_state ();
	children.cancel ();
	co_await children.join ();

	inflight.clear ();
}

asio::awaitable<void> dependency_strategy::run_requests ()
{
	while (true)
	{
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_dependencies);

		// No need to wait for block_processor, as we are not processing blocks

		auto budget = co_await ctx.request_budget.acquire ();
		co_await ctx.wait_limiter (ctx.limiter, 1);
		auto channel = co_await ctx.wait_channel ();

		// Wait until a blocked dependency without an in-flight request is available
		nano::block_hash blocking{ 0 };
		co_await nano::async::until (ctx.accounts_changed, ctx.clock, [this, &blocking] () {
			blocking = ctx.accounts.next_blocking ([this] (nano::block_hash const & hash) {
				return !inflight.contains (hash);
			});
			return !blocking.is_zero ();
		});

		ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_blocking);
		ctx.logger.debug (nano::log::type::bootstrap, "Requesting account info for: {} from: {}", blocking, channel);

		inflight.insert (blocking);
		children.spawn (run_walk (blocking, channel, std::move (budget)));
	}
}

asio::awaitable<void> dependency_strategy::run_walk (nano::block_hash hash, std::shared_ptr<nano::transport::channel> channel, nano::async::semaphore::token budget)
{
	nano::scope_guard guard{ [this, hash] () {
		inflight.erase (hash);
	} };

	account_info_query query{ .target = hash };

	auto outcome = co_await ctx.request (channel, query, query_source::dependencies, std::move (budget));
	if (!outcome)
	{
		co_return; // Timeout or send failure: the reserved peer slot stays as an implicit penalty
	}

	auto const & response = std::get<nano::messages::asc_pull_ack::account_info_payload> (outcome.message->payload);

	// There is no way to verify the response, so it is processed (and the peer released) regardless
	ctx.peers.release (outcome.channel);
	ctx.peers_changed.notify_all ();

	if (response.account.is_zero ())
	{
		ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info_empty);
		co_return; // OK, but nothing to do
	}

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::account_info);

	// Prioritize account containing the dependency
	ctx.accounts.dependency_update (hash, response.account);
	ctx.accounts.priority_set (response.account, account_sets_index::priority_cutoff); // Use the lowest possible priority here
	ctx.accounts_changed.notify_all ();
}

asio::awaitable<void> dependency_strategy::run_sync ()
{
	while (true)
	{
		// Reinsert known dependencies into the priority set
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::sync_dependencies);
		auto synced = ctx.accounts.sync_dependencies ();
		ctx.logger.debug (nano::log::type::bootstrap, "Synced {} dependencies", synced);
		if (synced > 0)
		{
			ctx.accounts_changed.notify_all ();
		}
		co_await ctx.clock.sleep_for (nano::is_dev_run () ? 500ms : 60s);
	}
}
}
