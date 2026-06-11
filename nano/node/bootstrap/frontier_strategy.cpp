#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/frontier_strategy.hpp>
#include <nano/node/bootstrap/service.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/formatting.hpp>
#include <nano/secure/ledger.hpp>

#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <numeric>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
frontier_strategy::frontier_strategy (service & ctx_a) :
	ctx{ ctx_a },
	head_scope{ ctx_a.strand },
	classify_changed{ ctx_a.strand }
{
}

frontier_strategy::head_state::head_state (head_index_t index_a, nano::account start_a, nano::account end_a, nano::async::strand & strand) :
	index{ index_a },
	start{ start_a },
	end{ end_a },
	cursor{ start_a },
	wake{ strand },
	children{ strand }
{
}

asio::awaitable<void> frontier_strategy::run ()
{
	debug_assert (ctx.strand.running_in_this_thread ());

	// (Re)divide the account space into consecutive equal ranges, one long-lived coroutine each.
	// Head parallelism is coroutine parallelism: each head schedules itself, there is no scan index.
	// Rebuilt on every invocation so that a service reset restarts the scan from the range starts.
	heads.clear ();
	nano::uint256_t const max_account = std::numeric_limits<nano::uint256_t>::max ();
	nano::uint256_t const range_size = max_account / ctx.config.frontier_scan.head_parallelism;
	for (unsigned i = 0; i < ctx.config.frontier_scan.head_parallelism; ++i)
	{
		// Start at 1 to avoid the burn account
		nano::uint256_t start = (i == 0) ? 1 : i * range_size;
		nano::uint256_t end = (i == ctx.config.frontier_scan.head_parallelism - 1) ? max_account : start + range_size;
		heads.push_back (std::make_unique<head_state> (i, nano::account{ start }, nano::account{ end }, ctx.strand));
	}
	release_assert (!heads.empty ());

	for (auto & head : heads)
	{
		head_scope.spawn (run_head (*head));
	}

	try
	{
		co_await head_scope.join (); // Heads run until cancelled
	}
	catch (boost::system::system_error const & ex)
	{
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	co_await asio::this_coro::reset_cancellation_state ();
	head_scope.cancel ();
	co_await head_scope.join ();
}

asio::awaitable<void> frontier_strategy::run_head (head_state & head)
{
	try
	{
		while (true)
		{
			ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_frontiers);
			co_await run_round (head);
		}
	}
	catch (boost::system::system_error const & ex)
	{
		debug_assert (ex.code () == asio::error::operation_aborted);
	}
	// On shutdown the in-flight samples of this head are cancelled rather than ridden out
	co_await asio::this_coro::reset_cancellation_state ();
	head.children.cancel ();
	co_await head.children.join ();
}

// One full fanout round at the head cursor. This lifecycle used to be smeared across run_one, the
// scan index scheduling, the response dispatch and the maintenance timeout sweep; here the round is
// a single block whose locals are the round state, concluding only at the single settlement point.
asio::awaitable<void> frontier_strategy::run_round (head_state & head)
{
	auto const start = head.cursor;
	auto state = std::make_shared<round_state> (round_state{
	.round = frontier_round{ ctx.config.frontier_scan, start, head.end },
	.used = {},
	.inflight = 0,
	.open = true,
	});

	size_t launched = 0;
	bool exhausted = false;
	auto last_launch = ctx.clock.now ();

	while (!state->round.settled () && !exhausted)
	{
		// The first consideration_count samples go out eagerly, further re-samples of a stuck round
		// wait out the cooldown first (this used to be frontier_scan_index::next ()'s scheduling).
		// Wakes from non-settling samples do not bypass the pacing.
		if (launched >= ctx.config.frontier_scan.consideration_count)
		{
			while (!state->round.settled () && ctx.clock.now () < last_launch + ctx.config.frontier_scan.cooldown)
			{
				auto const remaining = std::chrono::duration_cast<std::chrono::milliseconds> (last_launch + ctx.config.frontier_scan.cooldown - ctx.clock.now ());
				co_await wait_progress (head, remaining);
			}
			if (state->round.settled ())
			{
				break;
			}
		}

		// Backpressure gates, in the same order as before
		co_await nano::async::until (ctx.accounts_changed, ctx.clock, [this] () {
			return !ctx.accounts.priority_half_full ();
		});
		co_await ctx.wait_limiter (ctx.frontiers_limiter, 1);
		co_await nano::async::until (classify_changed, ctx.clock, [this] () {
			return classify_inflight < ctx.config.frontier_scan.max_pending;
		});
		auto budget = co_await ctx.request_budget.acquire ();
		co_await ctx.wait_limiter (ctx.limiter, 1);

		if (state->round.settled ()) // Re-check after the potentially long waits above
		{
			break;
		}

		// Route each sample of the round to a peer that was not yet queried for this position
		auto result = ctx.peers.acquire ({}, state->used);
		switch (result.status)
		{
			case acquire_status::acquired:
			{
				ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_frontier);
				ctx.logger.debug (nano::log::type::bootstrap, "Requesting frontiers starting from: {} from: {} (head: {})", start, result.channel, head.index);

				state->used.push_back (result.node_id);
				state->inflight += 1;
				launched += 1;
				last_launch = ctx.clock.now ();

				head.children.spawn (run_sample (head, state, start, result.channel, std::move (budget)));
			}
			break;
			case acquire_status::exhausted:
			{
				// Every distinct capable peer was already sampled for this position;
				// ride out the in-flight requests, then conclude with what was gathered
				if (state->inflight == 0)
				{
					exhausted = true;
				}
				else
				{
					co_await wait_progress (head, ctx.config.request_timeout);
				}
			}
			break;
			case acquire_status::busy:
			case acquire_status::no_peers:
			{
				// Wait for peer capacity, or for a settlement that may conclude the round meanwhile
				co_await nano::async::wait_any (ctx.peers_changed.wait (), wait_progress (head, 1s));
			}
			break;
		}
	}

	// === The single settlement point: the only place the head can advance, wrap or give up ===
	state->open = false;
	head.rounds += 1;

	if (auto target = state->round.conclude ())
	{
		if (state->round.done ())
		{
			ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done);
		}
		else if (state->round.empty_range ())
		{
			ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_empty);
		}
		else
		{
			ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_partial);
		}

		debug_assert (target->number () > head.cursor.number ());
		head.processed += state->round.candidate_count ();
		head.cursor = *target;

		// Bound the search range, wrap around once the range end is reached
		if (head.cursor.number () >= head.end.number ())
		{
			ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_range);
			head.cursor = head.start;
		}
	}
	else
	{
		// Nothing was learned (all samples lost); pace the retry of the same position
		co_await wait_progress (head, ctx.config.frontier_scan.cooldown);
	}
}

// The complete lifecycle of one sample request in a straight line: send, await the response or its
// timeout, verify, feed the round and hand the frontiers over for ledger reconciliation
asio::awaitable<void> frontier_strategy::run_sample (head_state & head, std::shared_ptr<round_state> state, nano::account start, std::shared_ptr<nano::transport::channel> channel, nano::async::semaphore::token budget)
{
	frontiers_query query{ .start = start, .count = nano::messages::asc_pull_ack::frontiers_payload::max_frontiers };

	auto outcome = co_await ctx.request (channel, query, query_source::frontiers, std::move (budget));

	// Stragglers from an already settled round still do their peer accounting but feed nothing
	bool const current = state->open;
	if (current)
	{
		release_assert (state->inflight > 0);
		state->inflight -= 1;
	}

	std::optional<std::deque<std::pair<nano::account, nano::block_hash>>> to_classify;

	if (outcome)
	{
		auto const & response = std::get<nano::messages::asc_pull_ack::frontiers_payload> (outcome.message->payload);

		ctx.stats.inc (nano::stat::type::bootstrap_process, response.frontiers.empty () ? nano::stat::detail::frontiers_empty : nano::stat::detail::frontiers);

		switch (verify (response, query))
		{
			case verify_result::ok:
			case verify_result::nothing_new: // An empty response is a valid "nothing past this position" sample
			{
				ctx.stats.inc (nano::stat::type::bootstrap_verify_frontiers, response.frontiers.empty () ? nano::stat::detail::nothing_new : nano::stat::detail::ok);

				// A processed response returns the reserved peer capacity
				ctx.peers.release (outcome.channel);
				ctx.peers_changed.notify_all ();

				if (current)
				{
					state->round.feed (response.frontiers);
				}

				if (!response.frontiers.empty ())
				{
					ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::frontiers, nano::stat::dir::in, response.frontiers.size ());

					// Reconcile against the ledger, bounded by the number of in-flight classifications
					if (classify_inflight < ctx.config.frontier_scan.max_pending * 4) // Allow some overfill to avoid unnecessarily dropping responses
					{
						to_classify = response.frontiers;
					}
					else
					{
						ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::frontiers_dropped, response.frontiers.size ());
					}
				}
			}
			break;
			case verify_result::invalid:
			{
				ctx.stats.inc (nano::stat::type::bootstrap_verify_frontiers, nano::stat::detail::invalid);
				ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::invalid_response);
				// No release: the reserved slot penalizes the peer until pool decay
			}
			break;
		}
	}

	if (current)
	{
		head.wake.notify_all (); // Wake the round conductor: the round may now be concluded
	}

	// The ledger reconciliation continues in this frame after the round was woken
	if (to_classify)
	{
		classify_inflight += 1;
		nano::scope_guard guard{ [this] () {
			classify_inflight -= 1;
			classify_changed.notify_all ();
		} };
		co_await classify (std::move (*to_classify));
	}
}

asio::awaitable<void> frontier_strategy::classify (std::deque<std::pair<nano::account, nano::block_hash>> frontiers)
{
	ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::processing_frontiers);

	auto const count = frontiers.size ();

	// The crawl is blocking store IO and runs on the worker pool, never on the strand
	auto classified = co_await nano::async::offload (ctx.workers, [&ledger = ctx.ledger, frontiers = std::move (frontiers)] () {
		auto transaction = ledger.tx_begin_read ();
		return classify_frontiers (ledger, transaction, frontiers);
	});

	// Resumed back on the strand: apply the decisions
	for (auto const & account : classified.prioritize)
	{
		// Use the lowest possible priority here
		ctx.accounts.priority_set (account, account_sets_index::priority_cutoff);
	}

	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::processed, count);
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::prioritized, classified.prioritize.size ());
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::outdated, classified.outdated);
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::pending, classified.pending);

	ctx.logger.debug (nano::log::type::bootstrap, "Processed {} frontiers of which outdated: {}, pending: {}", count, classified.outdated, classified.pending);

	if (!classified.prioritize.empty ())
	{
		ctx.accounts_changed.notify_all ();
	}
}

asio::awaitable<void> frontier_strategy::wait_progress (head_state & head, std::chrono::milliseconds timeout)
{
	co_await nano::async::wait_any (head.wake.wait (), ctx.clock.sleep_for (timeout));
}

nano::container_info frontier_strategy::container_info () const
{
	auto collect_progress = [&] () {
		nano::container_info info;
		for (auto const & head : heads)
		{
			boost::multiprecision::cpp_dec_float_50 start{ head->start.number ().str () };
			boost::multiprecision::cpp_dec_float_50 cursor{ head->cursor.number ().str () };
			boost::multiprecision::cpp_dec_float_50 end{ head->end.number ().str () };

			// Progress in the range [0, 1000000] since we can only represent `size_t` integers in the container_info data
			boost::multiprecision::cpp_dec_float_50 progress = (cursor - start) * boost::multiprecision::cpp_dec_float_50 (1000000) / (end - start);

			info.put (std::to_string (head->index), progress.convert_to<std::uint64_t> ());
		}
		return info;
	};

	auto collect_counter = [&] (auto getter) {
		nano::container_info info;
		for (auto const & head : heads)
		{
			info.put (std::to_string (head->index), getter (*head));
		}
		return info;
	};

	auto total_processed = std::accumulate (heads.begin (), heads.end (), std::size_t{ 0 }, [] (auto total, auto const & head) {
		return total + head->processed;
	});

	nano::container_info info;
	info.put ("total_processed", total_processed);
	info.add ("progress", collect_progress ());
	info.add ("rounds", collect_counter ([] (head_state const & head) { return head.rounds; }));
	info.add ("processed", collect_counter ([] (head_state const & head) { return head.processed; }));
	return info;
}
}
