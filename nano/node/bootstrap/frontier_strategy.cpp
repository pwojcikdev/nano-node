#include <nano/lib/logging.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp>
#include <nano/node/bootstrap/frontier_strategy.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/account.hpp>
#include <nano/store/ledger/pending.hpp>

#include <algorithm>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
frontier_strategy::frontier_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a },
	workers{ 1, nano::thread_role::name::bootstrap_frontier_processing }
{
}

void frontier_strategy::start ()
{
	debug_assert (!thread.joinable ());
	workers.start ();
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_frontier_scan);
		run ();
	});
}

void frontier_strategy::stop ()
{
	join_or_pass (thread);
	workers.stop ();
}

void frontier_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_frontiers);
		run_one ();
		lock.lock ();
	}
}

void frontier_strategy::run_one ()
{
	ctx.frontiers.advance ();

	auto launch = ctx.wait_result ([this] () {
		return next_frontier_launch ();
	});

	if (!launch)
	{
		return;
	}

	frontiers_query query{};
	query.start = launch->position;
	query.count = nano::messages::asc_pull_ack::frontiers_payload::max_frontiers;

	ctx.send (launch->channel, query, strategy::frontier, launch->id, launch->round);
}

std::optional<frontier_strategy::launch_result> frontier_strategy::next_frontier_launch ()
{
	debug_assert (!ctx.mutex.try_lock ());

	auto const now = std::chrono::steady_clock::now ();
	auto const probe = ctx.probes ();

	ctx.frontiers.settle (probe, now);

	if (ctx.accounts.priority_half_full ())
	{
		ctx.stats.inc (nano::stat::type::bootstrap_frontier_wait, nano::stat::detail::wait_priority);
		return std::nullopt;
	}
	if (workers.queued_tasks () >= ctx.config.frontier_scan.max_pending)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_frontier_wait, nano::stat::detail::wait_workers);
		return std::nullopt;
	}

	auto round = ctx.frontiers.next_round (probe, now);
	if (!round)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_frontier_wait, nano::stat::detail::wait_slot);
		return std::nullopt;
	}

	auto grant = ctx.acquire (strategy::frontier, {}, *round, now);
	if (!grant)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_frontier_wait, to_stat_detail (grant.peer_status));
		return std::nullopt;
	}

	ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::sample);
	ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::next_frontier);

	return launch_result{ grant.channel, round, round->position (), grant.id };
}

bool frontier_strategy::process (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::frontiers);

	release_assert (std::holds_alternative<frontiers_query> (tag.query));
	auto const & query = std::get<frontiers_query> (tag.query);
	debug_assert (!query.start.is_zero ());

	ctx.stats.inc (nano::stat::type::bootstrap_process, response.frontiers.empty () ? nano::stat::detail::frontiers_empty : nano::stat::detail::frontiers);

	auto result = verify (response, query);
	switch (result)
	{
		case verify_result::ok:
		case verify_result::nothing_new:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_frontiers, response.frontiers.empty () ? nano::stat::detail::nothing_new : nano::stat::detail::ok);

			auto const fed = ctx.frontiers.process (tag.id, query.start, response.frontiers);
			if (!fed)
			{
				ctx.stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::stale);
			}

			if (!response.frontiers.empty ())
			{
				ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::frontiers, nano::stat::dir::in, response.frontiers.size ());

				// Allow some overfill to avoid unnecessarily dropping responses
				if (workers.queued_tasks () < ctx.config.frontier_scan.max_pending * 4)
				{
					workers.post ([this, frontiers_l = response.frontiers] {
						process_frontiers (frontiers_l);
					});
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
		}
		break;
	}

	return result != verify_result::invalid;
}

void frontier_strategy::process_frontiers (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	release_assert (!frontiers.empty ());

	// Accounts must be passed in ascending order
	debug_assert (std::adjacent_find (frontiers.begin (), frontiers.end (), [] (auto const & lhs, auto const & rhs) {
		return lhs.first.number () >= rhs.first.number ();
	})
	== frontiers.end ());

	ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::processing_frontiers);

	frontier_classification result;
	{
		auto transaction = ctx.ledger.tx_begin_read ();
		result = classify_frontiers (transaction, ctx.ledger, frontiers);
	}

	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::processed, frontiers.size ());
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::prioritized, result.prioritize.size ());
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::outdated, result.outdated);
	ctx.stats.add (nano::stat::type::bootstrap_frontiers, nano::stat::detail::pending, result.pending);

	ctx.logger.debug (nano::log::type::bootstrap, "Processed {} frontiers of which outdated: {}, pending: {}", frontiers.size (), result.outdated, result.pending);

	nano::unique_lock<nano::mutex> lock{ ctx.mutex };

	for (auto const & account : result.prioritize)
	{
		// Use the lowest possible priority here
		ctx.accounts.priority_set (account, account_sets_index::priority_cutoff);
	}

	lock.unlock ();

	ctx.condition.notify_all ();
}

/*
 *
 */

frontier_classification classify_frontiers (nano::secure::transaction const & transaction, nano::ledger & ledger, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	// Accounts must be in ascending order
	debug_assert (std::adjacent_find (frontiers.begin (), frontiers.end (), [] (auto const & lhs, auto const & rhs) {
		return lhs.first.number () >= rhs.first.number ();
	})
	== frontiers.end ());

	frontier_classification result;

	if (frontiers.empty ())
	{
		return result;
	}

	auto const start = frontiers.front ().first;
	auto account_crawler = ledger.store.account.crawl (transaction, start);
	auto pending_crawler = ledger.store.pending.crawl (transaction, start);

	auto block_exists = [&ledger, &transaction] (nano::block_hash const & hash) {
		return ledger.any.block_exists_or_pruned (transaction, hash);
	};

	auto should_prioritize = [&] (nano::account const & account, nano::block_hash const & frontier) {
		account_crawler.skip_to (account);
		pending_crawler.skip_to (account);

		if (account_crawler && account_crawler->first == account)
		{
			if (account_crawler->second.head != frontier && !block_exists (frontier))
			{
				++result.outdated;
				return true;
			}
			return false;
		}

		if (pending_crawler && pending_crawler->first.account == account)
		{
			++result.pending;
			return true;
		}

		return false;
	};

	for (auto const & [account, frontier] : frontiers)
	{
		if (should_prioritize (account, frontier))
		{
			result.prioritize.push_back (account);
		}
	}

	return result;
}
}
