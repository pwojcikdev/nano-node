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
#include <nano/secure/common.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>
#include <nano/store/ledger/topology.hpp>
#include <nano/store/ledger_store.hpp>

#include <utility>
#include <vector>

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
	gaps{ ctx.config.topo_scan, ctx.stats },
	workers{ 1, nano::thread_role::name::bootstrap_topo_processing }
{
	// Retire completed pages straight into the pre-check pipeline
	scan.sink = [this] (topo_scan::page page) {
		post_precheck (std::move (page));
	};
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
	gaps.reset ();
}

void topo_strategy::maintenance ()
{
	debug_assert (!ctx.mutex.try_lock ());
	gaps.evict (); // Drop stale gaps so a permanently-missing dependency can't wedge the spearhead
}

nano::container_info topo_strategy::container_info () const
{
	nano::container_info info;
	info.add ("scan", scan.container_info ());
	info.add ("blocks", blocks.container_info ());
	info.add ("gaps", gaps.container_info ());
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
	// Back-pressure: pause discovery while the fetch buffer is full
	ctx.wait ([this] () {
		return blocks.total_count () < ctx.config.topo_scan.max_buffered;
	});

	// Reserve the oldest due head and learn how many distinct peers its round wants
	std::optional<topo_scan::request> req;
	ctx.wait ([this, &req] () {
		bool const include_spearhead = gaps.count () < ctx.config.topo_scan.max_gaps;
		req = scan.next (include_spearhead);
		return req.has_value ();
	});
	if (!req)
	{
		return;
	}

	// Acquire up to `fanout` distinct peers (topo index requests need the capability); the cross-round
	// `exclude` keeps a top-up from re-sampling peers already used for this cursor
	auto acquired = ctx.wait_channels (strategy::topology, nano::node_capabilities::topo_index, req->exclude, req->fanout);

	std::vector<std::pair<std::shared_ptr<nano::transport::channel>, id_t>> sends;
	{
		nano::lock_guard<nano::mutex> lock{ ctx.mutex };
		bool stale = false;
		for (auto const & lease : acquired.leases)
		{
			auto const id = nano::bootstrap::generate_id ();
			if (!scan.dispatch (req->head, req->start, id, lease.node_id))
			{
				stale = true; // The head advanced under us; drop the whole round (nothing dispatched yet)
				break;
			}
			sends.emplace_back (lease.channel, id);
		}
		if (stale)
		{
			for (auto const & lease : acquired.leases)
			{
				ctx.peers.release (lease.channel);
			}
			return;
		}
		// An exhausted round may complete the spearhead immediately; the sink retires whatever it produces
		if (acquired.exhausted)
		{
			scan.exhausted (req->head, req->start);
		}
	}

	for (auto const & [channel, id] : sends)
	{
		topo_index_query query{};
		query.start = req->start;
		query.count = req->count;
		ctx.send (channel, query, strategy::topology, id);
	}
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
		scan.cancel (tag.id); // the sink retires any page this completes
		return false;
	}

	ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, result == verify_result::ok ? nano::stat::detail::ok : nano::stat::detail::nothing_new);
	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::topo_indexes, response.entries.size ());

	scan.process (tag.id, response.entries); // the sink retires any page this completes

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
			scan.cancel (tag.id); // the sink retires any page this completes
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

void topo_strategy::post_precheck (topo_scan::page page)
{
	if (page.entries.empty ())
	{
		return;
	}

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

void topo_strategy::precheck (head_index head, std::deque<nano::topo_key> entries)
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

/*
 * Block-processor feedback
 */

void topo_strategy::inspect (nano::block_status result, nano::block const & block, strategy tag)
{
	debug_assert (!ctx.mutex.try_lock ());

	// Any source: the block is in the ledger now, so the gap we tracked for it (if any) is resolved
	if (result == nano::block_status::progress)
	{
		gaps.resolve (block.hash ());
		return;
	}

	// Only our own submissions count toward the spearhead back-pressure
	if (tag != strategy::topology)
	{
		return;
	}

	auto const account = block.account_field ().value_or (0);
	switch (result)
	{
		case nano::block_status::gap_previous:
			ctx.logger.warn (nano::log::type::bootstrap, "Topo gap (previous): block {} account {} missing previous {}", block.hash (), account, block.previous ());
			break;
		case nano::block_status::gap_source:
			ctx.logger.warn (nano::log::type::bootstrap, "Topo gap (source): block {} account {} missing source {}", block.hash (), account, block.source_field ().value_or (block.link_field ().value_or (0).as_block_hash ()));
			break;
		case nano::block_status::gap_epoch_open_pending:
			ctx.logger.warn (nano::log::type::bootstrap, "Topo gap (epoch open pending): block {} account {}", block.hash (), account);
			break;
		default:
			return; // Not a gap we track
	}

	gaps.track (block.hash (), account);
}

void topo_strategy::rollback (nano::account const & account)
{
	debug_assert (!ctx.mutex.try_lock ());
	gaps.rollback (account);
}
}
