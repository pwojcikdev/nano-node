#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/bootstrap/bootstrap_server.hpp>
#include <nano/node/bootstrap/priority_strategy.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/confirmation_height.hpp>

#include <algorithm>

namespace nano::bootstrap
{
priority_strategy::priority_strategy (service & ctx_a) :
	ctx{ ctx_a },
	pulls{ ctx_a.strand }
{
}

asio::awaitable<void> priority_strategy::run ()
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

	// Reset transient state so a respawn after a service reset starts cleanly
	buffer.clear ();
	inflight.clear ();
}

asio::awaitable<void> priority_strategy::run_requests ()
{
	while (true)
	{
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_priority);

		co_await ctx.wait_block_processor ();

		// Refill the buffer with a fresh batch of pull queries once it runs dry
		if (buffer.empty ())
		{
			ctx.stats.inc (nano::stat::type::bootstrap_priority, nano::stat::detail::refill);
			co_await refill ();
		}
		release_assert (!buffer.empty ());

		auto budget = co_await ctx.request_budget.acquire ();
		co_await ctx.wait_limiter (ctx.limiter, 1);
		auto channel = co_await ctx.wait_channel ();

		auto entry = buffer.front ();
		buffer.pop_front ();

		ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_priority);

		// Only cooldown accounts that are likely to have more blocks
		// This is to avoid requesting blocks from the same frontier multiple times, before the block processor had a chance to process them
		// Not throttling accounts that are probably up-to-date allows us to evict them from the priority set faster
		// The cooldown is applied before the send; a failed send resets it promptly inside run_pull
		if (entry.fails == 0)
		{
			ctx.accounts.timestamp_set (entry.query.account);
		}

		inflight[entry.query.account] += 1;
		pulls.spawn (run_pull (std::move (entry), channel, std::move (budget)));
	}
}

asio::awaitable<void> priority_strategy::refill ()
{
	// Wait until at least one priority account is available, then grab a batch
	std::deque<priority_result> batch;
	co_await nano::async::until (ctx.accounts_changed, ctx.clock, [this, &batch] () {
		batch = ctx.accounts.next_priority_batch ([this] (nano::account const & account) {
			auto it = inflight.find (account);
			return it == inflight.end () || it->second < max_inflight_per_account;
		},
		batch_size);
		return !batch.empty ();
	});

	// Decide optimistic vs safe per entry on the strand, where the seeded rng lives
	std::vector<bool> optimistic;
	optimistic.reserve (batch.size ());
	for (auto const & entry : batch)
	{
		// Probabilistically choose between requesting blocks from account frontier or confirmed frontier
		// Optimistic requests start from the (possibly unconfirmed) account frontier and are vulnerable to bootstrap poisoning
		// Safe requests start from the confirmed frontier and given enough time will eventually resolve forks
		optimistic.push_back (ctx.random (100) < ctx.config.optimistic_request_percentage);
	}

	// Build all pull queries for the batch using a single read transaction, off the strand
	buffer = co_await nano::async::offload (ctx.workers, [this, batch = std::move (batch), optimistic = std::move (optimistic)] () {
		std::deque<buffered_request> result;
		auto transaction = ctx.ledger.store.tx_begin_read ();
		for (std::size_t n = 0; n < batch.size (); ++n)
		{
			result.push_back ({ prepare_query (transaction, batch[n], optimistic[n]), batch[n].fails });
		}
		return result;
	});
}

asio::awaitable<void> priority_strategy::run_pull (buffered_request entry, std::shared_ptr<nano::transport::channel> channel, nano::async::semaphore::token budget)
{
	auto const account = entry.query.account;

	nano::scope_guard guard{ [this, account] () {
		if (auto it = inflight.find (account); it != inflight.end ())
		{
			if (--it->second == 0)
			{
				inflight.erase (it);
			}
		}
	} };

	auto status = co_await ctx.run_pull (channel, entry.query, query_source::priority, std::move (budget));

	// A request that never reached the peer must clear the optimistic cooldown so the account can be retried promptly
	if (status == request_outcome::status_t::send_failed && entry.fails == 0)
	{
		ctx.accounts.timestamp_reset (account);
		ctx.accounts_changed.notify_all ();
	}
}

blocks_query priority_strategy::prepare_query (nano::store::transaction const & transaction, priority_result const & entry, bool optimistic) const
{
	// Decide how many blocks to request based on the account priority
	std::size_t const min_pull_count = 2;
	auto count = std::clamp (static_cast<std::size_t> (entry.priority), min_pull_count, nano::bootstrap_server::max_blocks);
	count = std::min (count, ctx.config.max_pull_count);

	blocks_query query{};
	query.account = entry.account;
	query.count = count;

	// Check if the account picked has blocks, if it does, start the pull from the highest block
	if (auto info = ctx.ledger.store.account.get (transaction, entry.account))
	{
		if (optimistic) // Optimistic request case
		{
			ctx.stats.inc (nano::stat::type::bootstrap_priority, nano::stat::detail::from_frontier);

			query.type = query_type::blocks_by_hash;
			query.start = info->head;
		}
		else // Pessimistic (safe) request case
		{
			if (auto conf_info = ctx.ledger.store.confirmation_height.get (transaction, entry.account))
			{
				ctx.stats.inc (nano::stat::type::bootstrap_priority, nano::stat::detail::from_confirmed);

				query.type = query_type::blocks_by_hash;
				query.start = conf_info->frontier;
			}
			else
			{
				ctx.stats.inc (nano::stat::type::bootstrap_priority, nano::stat::detail::from_open);

				query.type = query_type::blocks_by_account;
				query.start = entry.account;
			}
		}
	}
	else
	{
		ctx.stats.inc (nano::stat::type::bootstrap_priority, nano::stat::detail::from_open);

		query.type = query_type::blocks_by_account;
		query.start = entry.account;
	}

	return query;
}
}
