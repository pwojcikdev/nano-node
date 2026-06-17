#include <nano/lib/blocks.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/node_capabilities.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/bootstrap/topo_strategy.hpp>
#include <nano/node/bootstrap/verify.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger_store.hpp>

namespace
{
// Cap on retire pages queued for pre-check; excess pages are dropped and re-discovered by repair heads
constexpr std::size_t max_precheck_tasks = 64;
// Maximum blocks released to the block processor per submit iteration
constexpr std::size_t max_submit_batch = 256;
}

namespace nano::bootstrap
{
topo_strategy::topo_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a },
	scan{ ctx.config.topo_scan, ctx.stats },
	blocks{ ctx.config.topo_scan, ctx.stats },
	workers{ 1, nano::thread_role::name::bootstrap_topo_processing }
{
}

void topo_strategy::start ()
{
	debug_assert (!scan_thread.joinable ());

	workers.start ();

	scan_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_scan);
		run_scan ();
	});
	fetch_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_fetch);
		run_fetch ();
	});
	submit_thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_submit);
		run_submit ();
	});
}

void topo_strategy::stop ()
{
	join_or_pass (scan_thread);
	join_or_pass (fetch_thread);
	join_or_pass (submit_thread);
	workers.stop ();
}

void topo_strategy::orient ()
{
	std::optional<nano::topo_key> latest;
	{
		auto transaction = ctx.ledger.tx_begin_read ();
		latest = ctx.ledger.store.topology.latest (transaction);
	}

	// Genesis sits at topo_height 1, so only anchor the spearhead to our tip when the ledger holds more than genesis.
	// On a fresh (genesis-only) ledger there is nothing to skip, but epoch open blocks can also sit at
	// topo_height 1 and sort before genesis by hash, so we leave the spearhead at true zero to discover them.
	if (latest && latest->topo_height > 1)
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		scan.orient (latest.value_or (nano::topo_key{}));
	}
}

void topo_strategy::reset ()
{
	scan.reset ();
	blocks.reset ();
}

nano::container_info topo_strategy::container_info () const
{
	nano::container_info info;
	info.add ("scan", scan.container_info ());
	info.add ("blocks", blocks.container_info ());
	return info;
}

/*
 * Scan
 */

void topo_strategy::run_scan ()
{
	orient ();

	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo_scan);
		scan_one ();
		lock.lock ();
	}
}

void topo_strategy::scan_one ()
{
	// Only peers that advertise the topo index capability can answer topo_index requests
	auto channel = ctx.wait_channel (strategy::topology, nano::node_capabilities::topo_index);
	if (!channel)
	{
		return;
	}

	// Block until a head is due, reserving it for `id` (mirrors frontier's wait_frontier). The spearhead is
	// gated by back-pressure (too many blocks awaiting fetch); repair heads stay eligible.
	auto const id = nano::bootstrap::generate_id ();
	std::optional<topo_scan::request> req;
	ctx.wait ([this, &req, id] () {
		bool const include_spearhead = blocks.pending_count () < ctx.config.topo_scan.max_pending;
		req = scan.next (include_spearhead, id);
		return req.has_value ();
	});
	if (!req)
	{
		return;
	}

	topo_index_query query{};
	query.start = req->start;
	query.count = req->count;

	ctx.send (channel, query, strategy::topology, id);
}

/*
 * Fetch
 */

void topo_strategy::run_fetch ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo_fetch);
		fetch_one ();
		lock.lock ();
	}
}

void topo_strategy::fetch_one ()
{
	// Claim a batch before taking a channel, so we never reserve a peer for a batch that raced empty
	std::deque<nano::block_hash> batch;
	ctx.wait ([this, &batch] () {
		batch = blocks.next_fetch_batch (ctx.config.topo_scan.fetch_batch);
		return !batch.empty ();
	});
	if (batch.empty ())
	{
		return;
	}

	// TODO: Temporarily filter by topo index, replace with filtering by protocol version
	auto channel = ctx.wait_channel (strategy::topology, nano::node_capabilities::topo_index);
	if (!channel)
	{
		return;
	}

	auto const id = nano::bootstrap::generate_id ();
	blocks_random_query query{};
	query.hashes = std::move (batch);

	ctx.send (channel, query, strategy::topology, id);
}

/*
 * Submit
 */

void topo_strategy::run_submit ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		lock.unlock ();
		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo_submit);
		submit_one ();
		lock.lock ();
	}
}

void topo_strategy::submit_one ()
{
	std::deque<std::shared_ptr<nano::block>> batch;
	ctx.wait ([this, &batch] () {
		batch = blocks.next_submit_batch (max_submit_batch);
		return !batch.empty ();
	});
	if (batch.empty ())
	{
		return;
	}

	// Respect the topology strategy's own block processor fair-queue bucket before releasing
	ctx.wait_block_processor (strategy::topology, ctx.config.block_processor_threshold);

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::submitted, batch.size ());

	ctx.block_processor.add_many (batch, nano::block_source::bootstrap, ctx.block_processor_channel (strategy::topology), {}, strategy::topology);
}

/*
 * Response handling
 */

bool topo_strategy::process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::topo_index);

	release_assert (std::holds_alternative<topo_index_query> (tag.query));
	auto const & query = std::get<topo_index_query> (tag.query);

	auto const result = verify (response, query);

	if (result == verify_result::invalid)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::invalid);
		scan.cancel (tag.id);
		return false;
	}

	ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, result == verify_result::ok ? nano::stat::detail::ok : nano::stat::detail::nothing_new);
	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::topo_indexes, response.entries.size ());

	auto page = scan.process (tag.id, response.entries);
	if (!page.entries.empty ())
	{
		// Drop the page under overload; repair heads will rediscover it
		if (workers.queued_tasks () < max_precheck_tasks)
		{
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::retire);

			workers.post ([this, head = page.head, entries = std::move (page.entries)] () mutable {
				precheck (head, std::move (entries));
			});
		}
		else
		{
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::dropped);
		}
	}

	return true;
}

bool topo_strategy::process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type () == query_type::blocks_random);

	release_assert (std::holds_alternative<blocks_random_query> (tag.query));
	auto const & query = std::get<blocks_random_query> (tag.query);

	auto result = verify (response, query);
	switch (result)
	{
		case verify_result::ok:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::ok);
			ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::fetched, response.blocks.size ());
			blocks.process_fetched (response.blocks);
		}
		break;
		case verify_result::nothing_new:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::nothing_new);
			// No blocks returned; the requested entries stay pending and will be retried after cooldown
		}
		break;
		case verify_result::invalid:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::invalid);
		}
		break;
	}

	return result != verify_result::invalid;
}

void topo_strategy::timeout (async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	switch (tag.type ())
	{
		case query_type::topo_index:
		{
			scan.cancel (tag.id);
		}
		break;
		case query_type::blocks_random:
		{
			release_assert (std::holds_alternative<blocks_random_query> (tag.query));
			auto const & query = std::get<blocks_random_query> (tag.query);
			blocks.rearm (query.hashes);
		}
		break;
		default:
			break;
	}
}

void topo_strategy::failure (async_tag const & tag)
{
	timeout (tag);
}

/*
 * Pre-check (runs on the worker pool, off the message thread)
 */

void topo_strategy::precheck (unsigned head, std::deque<nano::topo_key> entries)
{
	std::deque<nano::topo_key> missing;
	{
		auto transaction = ctx.ledger.tx_begin_read ();
		for (auto const & key : entries)
		{
			if (!ctx.ledger.any.block_exists_or_pruned (transaction, key.hash))
			{
				missing.push_back (key);
			}
		}
	}

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::prechecked, entries.size ());

	// The spearhead (head 0) turns up new tip blocks; repair heads turn up gaps in the already-swept range
	ctx.stats.add (nano::stat::type::bootstrap_topo, (head == 0) ? nano::stat::detail::redundant : nano::stat::detail::rescanned, entries.size () - missing.size ());
	ctx.stats.add (nano::stat::type::bootstrap_topo, (head == 0) ? nano::stat::detail::missing : nano::stat::detail::gap, missing.size ());

	if (!missing.empty ())
	{
		{
			nano::lock_guard<nano::mutex> lock{ ctx.mutex };
			blocks.add (missing);
		}
		ctx.condition.notify_all (); // Wake the fetch loop
	}
}
}
