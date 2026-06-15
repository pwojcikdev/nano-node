#include <nano/lib/blocks.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_context.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/bootstrap/topo_strategy.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/node/network.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/topology.hpp>

#include <algorithm>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
namespace
{
	nano::node_capabilities_flags topo_capability ()
	{
		return nano::node_capabilities_flags{ nano::node_capabilities::topo_index };
	}
}

topo_strategy::topo_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a },
	probes{
		.peer_status = [this] (std::span<nano::account const> exclude) { return ctx.peers.probe (topo_capability (), exclude); },
		.count_inflight = [this] (std::span<id_t const> ids) {
			auto const & by_id = ctx.tags.get<bootstrap_context::tag_id> ();
			return static_cast<size_t> (std::count_if (ids.begin (), ids.end (), [&] (id_t id) {
				return by_id.find (id) != by_id.end ();
			})); },
	}
{
}

void topo_strategy::start ()
{
	debug_assert (!thread.joinable ());
	orient ();
	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_scan);
		run ();
	});
}

void topo_strategy::stop ()
{
	nano::join_or_pass (thread);
}

void topo_strategy::run ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo);
		run_one ();
		lock.lock ();
	}
}

void topo_strategy::run_one ()
{
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		ctx.topologies.settle (std::chrono::steady_clock::now (), probes);
	}

	if (try_submit ())
	{
		return;
	}
	if (try_fetch ())
	{
		return;
	}
	try_page_or_wait ();
}

void topo_strategy::orient ()
{
	nano::topo_key start{};
	if (ctx.config.topo_scan.enable_orient)
	{
		auto transaction = ctx.ledger.tx_begin_read ();
		if (auto latest = ctx.ledger.store.topology.latest (transaction))
		{
			start = *latest;
		}
	}

	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		ctx.topologies.orient (start);
	}
}

bool topo_strategy::try_submit ()
{
	auto const now = std::chrono::steady_clock::now ();
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		if (!ctx.topologies.submit_ready (now))
		{
			return false;
		}
	}

	ctx.wait_block_processor (strategy::topo, ctx.config.block_processor_threshold);

	std::deque<std::shared_ptr<nano::block>> blocks;
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		blocks = ctx.topologies.next_submit_batch (std::chrono::steady_clock::now (), ctx.config.topo_scan.block_batch_size);
	}
	if (blocks.empty ())
	{
		return false;
	}

	std::deque<nano::block_hash> rejected;
	auto const & channel = ctx.block_processor_channel (strategy::topo);
	for (auto const & block : blocks)
	{
		auto const added = ctx.block_processor.add (block, nano::block_source::bootstrap, channel, nullptr, strategy::topo);
		if (!added)
		{
			rejected.push_back (block->hash ());
		}
	}

	if (!rejected.empty ())
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		for (auto const & hash : rejected)
		{
			ctx.topologies.inspect (hash, nano::block_status::insufficient_work, std::chrono::steady_clock::now ());
		}
		ctx.condition.notify_all ();
	}

	return true;
}

bool topo_strategy::try_fetch ()
{
	auto const now = std::chrono::steady_clock::now ();
	std::deque<nano::block_hash> candidates;
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		if (!ctx.topologies.fetch_ready (now))
		{
			return false;
		}
		auto const max_hashes = std::min (ctx.config.topo_scan.block_batch_size, nano::messages::asc_pull_req::blocks_random_payload::max_hashes);
		candidates = ctx.topologies.next_fetch_candidates (now, max_hashes);
	}
	if (candidates.empty ())
	{
		return false;
	}

	std::deque<nano::block_hash> already_local;
	std::deque<nano::block_hash> missing;
	{
		auto transaction = ctx.ledger.tx_begin_read ();
		for (auto const & hash : candidates)
		{
			if (ctx.ledger.any.block_exists_or_pruned (transaction, hash))
			{
				already_local.push_back (hash);
			}
			else
			{
				missing.push_back (hash);
			}
		}
	}

	if (!already_local.empty ())
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		ctx.topologies.commit_fetch ({}, already_local, 0, std::chrono::steady_clock::now ());
	}

	if (missing.empty ())
	{
		ctx.condition.notify_all ();
		return true;
	}

	auto launch = ctx.wait_result ([this, &missing] () {
		return next_fetch_launch (missing);
	});

	if (!launch)
	{
		return false;
	}

	blocks_random_query query{ .hashes = missing };
	ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::blocks_random);
	return ctx.send (launch->channel, query, strategy::topo, launch->id);
}

std::optional<topo_strategy::fetch_launch> topo_strategy::next_fetch_launch (std::deque<nano::block_hash> const & missing)
{
	debug_assert (!ctx.mutex.try_lock ());

	auto grant = ctx.acquire (strategy::topo, topo_capability ());
	if (!grant)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo_scan, to_stat_detail (grant.peer_status));
		return std::nullopt;
	}

	ctx.topologies.commit_fetch (missing, {}, grant.id, std::chrono::steady_clock::now ());
	return fetch_launch{ grant.channel, grant.id };
}

bool topo_strategy::try_page_or_wait ()
{
	auto result = ctx.wait_result ([this] () {
		return next_page_or_ready ();
	});

	if (!result || result->type == page_wait_result::kind::ready)
	{
		return false;
	}

	auto const & launch = result->launch;

	topo_index_query query{};
	query.start = launch.position;
	query.count = nano::messages::asc_pull_ack::topo_index_payload::max_entries;

	return ctx.send (launch.channel, query, strategy::topo, launch.id);
}

std::optional<topo_strategy::page_wait_result> topo_strategy::next_page_or_ready ()
{
	debug_assert (!ctx.mutex.try_lock ());

	auto const now = std::chrono::steady_clock::now ();
	ctx.topologies.settle (now, probes);

	if (ctx.topologies.submit_ready (now) || ctx.topologies.fetch_ready (now))
	{
		return page_wait_result{ page_wait_result::kind::ready, {} };
	}
	auto round = ctx.topologies.next_round (now, probes);
	if (!round)
	{
		return std::nullopt;
	}

	auto grant = ctx.acquire (strategy::topo, topo_capability (), round->exclude ());
	if (!grant)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo_scan, to_stat_detail (grant.peer_status));
		return std::nullopt;
	}

	round->reserve (grant.node_id, grant.id, now);
	ctx.stats.inc (nano::stat::type::bootstrap_next, nano::stat::detail::topo_index);
	return page_wait_result{ page_wait_result::kind::launch, page_launch{ grant.channel, round->position (), grant.id } };
}

bool topo_strategy::process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::blocks_random);
	release_assert (std::holds_alternative<blocks_random_query> (tag.query));

	ctx.stats.inc (nano::stat::type::bootstrap_process, response.blocks.empty () ? nano::stat::detail::topo_blocks_empty : nano::stat::detail::topo_blocks);

	auto result = verify (response, std::get<blocks_random_query> (tag.query));
	switch (result)
	{
		case verify_result::ok:
		case verify_result::nothing_new:
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, response.blocks.empty () ? nano::stat::detail::nothing_new : nano::stat::detail::ok);
			if (!response.blocks.empty ())
			{
				ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());
			}
			ctx.topologies.process_blocks (tag.id, response.blocks, std::chrono::steady_clock::now ());
			break;
		case verify_result::invalid:
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::invalid);
			ctx.topologies.erase_fetch (tag.id, std::chrono::steady_clock::now ());
			break;
	}

	return result != verify_result::invalid;
}

bool topo_strategy::process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::topo_index);
	release_assert (std::holds_alternative<topo_index_query> (tag.query));

	ctx.stats.inc (nano::stat::type::bootstrap_process, response.entries.empty () ? nano::stat::detail::topo_page_empty : nano::stat::detail::topo_page);

	auto result = verify (response, std::get<topo_index_query> (tag.query));
	switch (result)
	{
		case verify_result::ok:
		case verify_result::nothing_new:
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, response.entries.empty () ? nano::stat::detail::nothing_new : nano::stat::detail::ok);
			if (!response.entries.empty ())
			{
				ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::topo_indexes, nano::stat::dir::in, response.entries.size ());
			}
			if (!ctx.topologies.process_page (tag.id, std::get<topo_index_query> (tag.query).start, response.entries))
			{
				ctx.stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_page_stale);
			}
			break;
		case verify_result::invalid:
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::invalid);
			ctx.topologies.erase_page (tag.id, std::get<topo_index_query> (tag.query).start);
			break;
	}

	return result != verify_result::invalid;
}

void topo_strategy::timeout (async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	if (std::holds_alternative<topo_index_query> (tag.query))
	{
		ctx.topologies.erase_page (tag.id, std::get<topo_index_query> (tag.query).start);
	}
	else if (std::holds_alternative<blocks_random_query> (tag.query))
	{
		ctx.topologies.erase_fetch (tag.id, std::chrono::steady_clock::now ());
	}
}

void topo_strategy::failure (async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	if (std::holds_alternative<topo_index_query> (tag.query))
	{
		ctx.topologies.erase_page (tag.id, std::get<topo_index_query> (tag.query).start);
	}
	else if (std::holds_alternative<blocks_random_query> (tag.query))
	{
		ctx.topologies.erase_fetch (tag.id, std::chrono::steady_clock::now ());
	}
}

void topo_strategy::inspect (nano::block_status const & result, nano::block_context const & context)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (context.block != nullptr);
	ctx.topologies.inspect (context.block->hash (), result, std::chrono::steady_clock::now ());
}
}
