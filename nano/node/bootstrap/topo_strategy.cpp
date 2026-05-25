#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/topo_strategy.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/channel.hpp>
#include <nano/node/transport/formatting.hpp>
#include <nano/secure/ledger.hpp>
#include <nano/secure/ledger_set_any.hpp>

#include <utility>

using namespace std::chrono_literals;

namespace nano::bootstrap
{
topo_strategy::topo_strategy (bootstrap_context & ctx_a) :
	ctx{ ctx_a }
{
}

void topo_strategy::start ()
{
	debug_assert (!thread_scan.joinable ());
	debug_assert (!thread_blocks.joinable ());
	debug_assert (!thread_processing.joinable ());

	// A single thread drives all scanning heads, weighting the spear (spear_weight:1).
	thread_scan = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_index);
		run_scan ();
	});

	thread_blocks = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_blocks);
		run_blocks ();
	});

	thread_processing = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_processing);
		run_processing ();
	});
}

void topo_strategy::stop ()
{
	nano::join_or_pass (thread_scan);
	nano::join_or_pass (thread_blocks);
	nano::join_or_pass (thread_processing);
}

void topo_strategy::inspect (nano::secure::transaction const & txn, nano::block_status const & status, nano::block_context const & context)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (std::any_cast<query_source> (context.tag) == query_source::topology_index);

	auto const & hash = context.block->hash ();

	switch (status)
	{
		case nano::block_status::progress:
		case nano::block_status::old:
		{
			// The block reached the ledger. The member already carries its
			// topo_key from discovery, so we just mark it confirmed.
			ctx.topology.block_indexed (hash);
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::indexed);
		}
		break;
		case nano::block_status::gap_previous:
		case nano::block_status::gap_source:
		case nano::block_status::gap_epoch_open_pending:
		{
			// A dependency below the cursor is missing. Keep the member (retained
			// + re-submitted); once enough gaps accumulate the spear pauses while the
			// repair heads re-scan and fill them. The per-result gap counters are
			// tracked in bootstrap_context::inspect.
			ctx.topology.block_gapped (hash);
		}
		break;
		default:
		{
			// Fork / bad signature / ... — terminally unusable, not a hole.
			ctx.topology.block_terminal (hash);
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::block_failed);
		}
		break;
	}
}

void topo_strategy::run_scan ()
{
	auto const poll_interval = nano::is_dev_run () ? 1s : 15s;

	while (!ctx.stopped)
	{
		// Spear poll mode: once caught up, periodically re-page from the tip to detect
		// blocks appended to the index.
		{
			nano::unique_lock<nano::mutex> lock{ ctx.mutex };
			if (ctx.topology.caught_up ())
			{
				ctx.condition.wait_for (lock, poll_interval, [this] () { return ctx.stopped; });
				if (!ctx.stopped)
				{
					ctx.topology.repoll ();
				}
				continue;
			}
		}

		// Wait (with backoff) for a head to have work, weighting the spear (spear_weight:1).
		auto picked = wait_scan_head ();
		if (!picked)
		{
			continue; // caught_up / stopped — re-evaluate at the top.
		}

		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::loop_topo_index);
		if (auto channel = wait_topo_index_channel ())
		{
			request_index (picked->first, picked->second, channel);
		}
	}
}

std::optional<std::pair<std::size_t, nano::topo_key>> topo_strategy::wait_scan_head ()
{
	std::optional<std::pair<std::size_t, nano::topo_key>> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		// Nothing more to schedule once the topology stops indexing (drives the outer
		// loop into poll mode / parking).
		if (!ctx.topology.indexing ())
		{
			return true;
		}
		result = pick_scan_head (std::chrono::steady_clock::now ());
		return result.has_value ();
	});
	return result;
}

std::optional<std::pair<std::size_t, nano::topo_key>> topo_strategy::pick_scan_head (std::chrono::steady_clock::time_point now)
{
	debug_assert (!ctx.mutex.try_lock ());

	std::size_t const heads = ctx.topology.head_count ();
	// Preferred head this tick, weighted spear:repair = `spear_weight`:1. Each cycle of
	// (spear_weight + 1) ticks gives the spear (head 0) the first `spear_weight` ticks and
	// a repair head the last one (round-robin across cycles). So the spear gets
	// spear_weight/(spear_weight+1) of the requests and the repair heads share the rest.
	// (topology.next has no side effects when it returns nullopt, so trying several heads
	// here commits at most the one we serve.)
	std::size_t const spear_weight = ctx.config.topo_scan.spear_weight == 0 ? 1 : ctx.config.topo_scan.spear_weight;
	std::size_t const cycle = spear_weight + 1;
	std::size_t preferred;
	if (heads <= 1 || (scan_tick % cycle) < spear_weight)
	{
		preferred = 0;
	}
	else
	{
		preferred = 1 + ((scan_tick / cycle) % (heads - 1));
	}
	++scan_tick;

	if (auto pos = ctx.topology.next (preferred, now))
	{
		return std::make_pair (preferred, *pos);
	}
	// Preferred head had nothing; fall back to any other head with work (lowest index
	// first, so the spear soaks up spare capacity) rather than waste the tick.
	for (std::size_t h = 0; h < heads; ++h)
	{
		if (auto pos = ctx.topology.next (h, now))
		{
			return std::make_pair (h, *pos);
		}
	}
	return std::nullopt;
}

void topo_strategy::run_blocks ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		if (!ctx.topology.indexing ())
		{
			ctx.condition.wait_for (lock, nano::is_dev_run () ? 500ms : 5s, [this] () { return ctx.stopped; });
			continue;
		}

		lock.unlock ();

		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::loop_topo_blocks);
		run_one_blocks ();

		lock.lock ();
	}
}

void topo_strategy::run_one_blocks ()
{
	auto hashes = wait_block_batch ();
	if (hashes.empty ())
	{
		return;
	}

	// Pre-fetch redundancy filter: never fetch a block we already have. Ledger
	// reads run WITHOUT ctx.mutex; only the in-memory tag takes the lock.
	std::deque<nano::block_hash> to_fetch;
	std::deque<nano::block_hash> redundant;
	{
		auto tx = ctx.ledger.tx_begin_read ();
		for (auto const & hash : hashes)
		{
			if (ctx.ledger.any.block_exists (tx, hash))
			{
				redundant.push_back (hash);
			}
			else
			{
				to_fetch.push_back (hash);
			}
		}
	}

	if (!redundant.empty ())
	{
		nano::lock_guard<nano::mutex> guard{ ctx.mutex };
		for (auto const & hash : redundant)
		{
			ctx.topology.mark_redundant (hash);
		}
		ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::redundant, redundant.size ());
	}

	if (to_fetch.empty ())
	{
		return;
	}

	auto channel = wait_topo_index_channel ();
	if (!channel)
	{
		return;
	}

	request_blocks (std::move (to_fetch), channel);
}

void topo_strategy::run_processing ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		if (!ctx.topology.indexing ())
		{
			ctx.condition.wait_for (lock, nano::is_dev_run () ? 500ms : 5s, [this] () { return ctx.stopped; });
			continue;
		}

		lock.unlock ();

		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::loop_topo_processing);
		run_one_processing ();

		lock.lock ();
	}
}

void topo_strategy::run_one_processing ()
{
	ctx.wait_block_processor (strategy::topology, ctx.config.topo_scan.block_processor_threshold);

	auto ordered = wait_submit_batch ();
	if (ordered.empty ())
	{
		return;
	}

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::ordered, ordered.size ());

	// Tag each block with `topology_index` so the inspect callback routes the
	// result back to the topo scan.
	auto submitted = ctx.block_processor.add_many (ordered,
	nano::block_source::bootstrap, ctx.block_processor_channel (strategy::topology),
	/* last_callback */ {},
	/* tag */ std::any{ query_source::topology_index });
	debug_assert (submitted == ordered.size ()); // We wait for capacity first, so all should be submitted.

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::submitted, submitted);
}

std::deque<nano::block_hash> topo_strategy::wait_block_batch ()
{
	std::deque<nano::block_hash> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		result = ctx.topology.next_blocks (ctx.config.topo_scan.block_batch_size);
		return !result.empty () || !ctx.topology.indexing ();
	});
	return result;
}

std::deque<std::shared_ptr<nano::block>> topo_strategy::wait_submit_batch ()
{
	std::deque<std::shared_ptr<nano::block>> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		// Bound the drain so a single add_many can't overshoot the processor's
		// per-source queue cap.
		result = ctx.topology.next_submit (ctx.config.block_processor_threshold);
		return !result.empty () || !ctx.topology.indexing ();
	});
	return result;
}

std::shared_ptr<nano::transport::channel> topo_strategy::wait_topo_index_channel ()
{
	return ctx.wait_channel (strategy::topology, [] (std::shared_ptr<nano::transport::channel> const & channel) {
		return channel->get_flags ().test (nano::node_capabilities::topo_index);
	});
}

bool topo_strategy::request_index (std::size_t h, nano::topo_key cursor, std::shared_ptr<nano::transport::channel> const & channel)
{
	async_tag tag{};
	tag.type = query_type::topo_index;
	tag.source = query_source::topology_index;
	tag.hash = cursor.hash;

	topo_index_tag_payload payload{};
	payload.cursor = cursor;
	payload.head = h;
	tag.payload = payload;

	nano::messages::asc_pull_req message{ ctx.network_constants };
	message.id = tag.id;
	message.type = nano::messages::asc_pull_type::topo_index;

	nano::messages::asc_pull_req::topo_index_payload msg_pld;
	msg_pld.start = cursor;
	msg_pld.count = nano::narrow_cast<uint16_t> (std::min<std::size_t> (ctx.config.topo_scan.candidates, nano::messages::asc_pull_ack::topo_index_payload::max_entries));
	message.payload = msg_pld;
	message.update_header ();

	ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_index);
	ctx.logger.debug (nano::log::type::bootstrap, "Requesting topo index (head {}) from cursor topo_height={} hash={} from: {}", h, cursor.topo_height, cursor.hash.to_string (), channel);

	return ctx.send (channel, std::move (message), tag);
}

bool topo_strategy::request_blocks (std::deque<nano::block_hash> hashes, std::shared_ptr<nano::transport::channel> const & channel)
{
	debug_assert (!hashes.empty ());

	async_tag tag{};
	tag.type = query_type::blocks_random;
	tag.source = query_source::topology_blocks;
	tag.hash = hashes.front ();

	blocks_random_tag_payload payload{};
	payload.hashes = hashes;
	tag.payload = payload;

	nano::messages::asc_pull_req message{ ctx.network_constants };
	message.id = tag.id;
	message.type = nano::messages::asc_pull_type::blocks_random;

	nano::messages::asc_pull_req::blocks_random_payload msg_pld;
	msg_pld.hashes = std::move (hashes);
	message.payload = std::move (msg_pld);
	message.update_header ();

	ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_blocks);
	ctx.logger.debug (nano::log::type::bootstrap, "Requesting {} random topology blocks from: {}", payload.hashes.size (), channel);

	return ctx.send (channel, std::move (message), tag);
}

verify_result topo_strategy::verify (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag) const
{
	release_assert (std::holds_alternative<topo_index_tag_payload> (tag.payload));
	auto const & payload = std::get<topo_index_tag_payload> (tag.payload);
	auto const & entries = response.entries;

	// Non-empty entries must be sorted strictly ascending and start at or past the
	// requested cursor. Topo heights are contiguous: a block at height H has a
	// dependency at H-1 that must also be in the peer's ledger, so by induction every
	// height from 1 up is populated. Hence consecutive entries are at most ONE height
	// apart (0 = same height/different hash, 1 = next height); a larger jump means the
	// peer's index has a hole — broken, incomplete, or malicious — so reject it. Empty
	// entries are valid: end-of-topology signal.
	if (!entries.empty ())
	{
		if (entries.front () < payload.cursor)
		{
			return verify_result::invalid;
		}
		nano::topo_key previous{};
		bool first = true;
		for (auto const & entry : entries)
		{
			if (entry <= previous)
			{
				return verify_result::invalid;
			}
			if (!first && entry.topo_height > previous.topo_height + 1)
			{
				return verify_result::invalid; // gap in the height space
			}
			previous = entry;
			first = false;
		}
	}

	return verify_result::ok;
}

bool topo_strategy::process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type == query_type::topo_index);

	release_assert (std::holds_alternative<topo_index_tag_payload> (tag.payload));
	auto const & payload = std::get<topo_index_tag_payload> (tag.payload);

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::topo_index);

	auto const result = verify (response, tag);
	switch (result)
	{
		case verify_result::ok:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::ok);
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::process_topo);
			ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::topo_index, response.entries.size ());

			bool advanced = ctx.topology.process (payload.head, payload.cursor, response.entries);
			if (advanced)
			{
				ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::advance);
			}
		}
		break;
		case verify_result::nothing_new:
		{
			ctx.stats.inc (nano::stat::type::bootstrap_verify_topo, nano::stat::detail::nothing_new);
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

bool topo_strategy::process_blocks (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.source == query_source::topology_blocks);

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::blocks);
	ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::process_blocks);
	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::blocks, response.blocks.size ());

	for (auto const & block : response.blocks)
	{
		if (block)
		{
			ctx.topology.block_received (block->hash (), block);
		}
	}

	return true;
}
}
