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
	debug_assert (!thread_index.joinable ());
	debug_assert (!thread_blocks.joinable ());
	debug_assert (!thread_processing.joinable ());

	thread_index = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_index);
		run_index ();
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
	join_or_pass (thread_index);
	join_or_pass (thread_blocks);
	join_or_pass (thread_processing);
}

void topo_strategy::inspect (nano::secure::transaction const & txn, nano::block_status const & status, nano::block_context const & context)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (std::any_cast<query_source> (context.tag) == query_source::topology_index);

	auto const & hash = context.block->hash ();

	switch (status)
	{
		case nano::block_status::progress:
		{
			// Newly inserted blocks carry the sideband set by ledger.process(); its
			// topo_height is the authoritative source for closed-loop cursor advance.
			debug_assert (context.block->has_sideband ());
			auto const topo_height = context.block->sideband ().topo_height;
			debug_assert (topo_height > 0);

			ctx.topology.mark_indexed (hash, topo_height);
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::indexed);
		}
		break;
		case nano::block_status::old:
		{
			// The network block has no sideband for "old" results — ledger.process()
			// doesn't set one when the block is already stored. Look up the stored
			// block to read the authoritative topo_height.
			if (auto stored = ctx.ledger.any.block_get (txn, hash))
			{
				debug_assert (stored->has_sideband ());
				auto const topo_height = stored->sideband ().topo_height;
				debug_assert (topo_height > 0);

				ctx.topology.mark_indexed (hash, topo_height);
				ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::indexed);
			}
		}
		break;
		default:
			break;
	}
}

void topo_strategy::run_index ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		// Park while indexing is finished. Will be woken on stop or reset.
		if (!ctx.topology.indexing ())
		{
			ctx.condition.wait_for (lock, nano::is_dev_run () ? 500ms : 5s, [this] () { return ctx.stopped; });
			continue;
		}

		lock.unlock ();

		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::loop_topo_index);
		run_one_index ();

		lock.lock ();
	}
}

void topo_strategy::run_one_index ()
{
	auto channel = wait_topo_index_channel ();
	if (!channel)
	{
		return;
	}
	auto pos = wait_position ();
	if (!pos)
	{
		return;
	}
	request_index (*pos, channel);
}

void topo_strategy::run_blocks ()
{
	nano::unique_lock<nano::mutex> lock{ ctx.mutex };
	while (!ctx.stopped)
	{
		// Park while there's nothing to fetch and indexing is done.
		if (!ctx.topology.has_blocks_pending () && !ctx.topology.indexing ())
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

	// Pre-fetch redundancy filter: never spend a network fetch on a block we
	// already have. The ledger reads run WITHOUT ctx.mutex (DB I/O must not
	// stall the other strategies); only the tiny in-memory state tag takes the
	// lock. We do NOT advance the cursor here — `mark_redundant` just tags the
	// entry; the cursor advance is deferred to `next_ordered_blocks`, which
	// applies it strictly in topological order (gap-safe) regardless of the
	// parallel fetch order.
	std::deque<nano::block_hash> to_fetch;
	std::deque<std::pair<nano::block_hash, uint64_t>> redundant;
	{
		auto tx = ctx.ledger.tx_begin_read ();
		for (auto const & hash : hashes)
		{
			if (auto existing = ctx.ledger.any.block_get (tx, hash))
			{
				debug_assert (existing->has_sideband ());
				redundant.emplace_back (hash, existing->sideband ().topo_height);
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
		for (auto const & [hash, topo_height] : redundant)
		{
			ctx.topology.mark_redundant (hash, topo_height);
			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::redundant);
		}
	}

	if (to_fetch.empty ())
	{
		return;
	}

	auto const min_version = ctx.network_constants.topo_bootstrap_protocol_version_min;
	// auto channel = ctx.wait_channel ([min_version] (std::shared_ptr<nano::transport::channel> const & channel) {
	// return channel->get_network_version () >= min_version;
	// });
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
		// Park while there's nothing pending and indexing is done.
		if (!ctx.topology.has_blocks_pending () && !ctx.topology.indexing ())
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
	ctx.wait_block_processor ();

	auto ordered = wait_ordered_blocks ();
	if (ordered.empty ())
	{
		return;
	}

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::ordered, ordered.size ());

	// Tag each submitted block with `topology_index` so that the inspect callback can route it back to this strategy and advance the closed-loop cursor.
	// Redundant (already-in-ledger) blocks were filtered out pre-fetch in
	// run_one_blocks, so `ordered` is genuinely-new work; the inspect `old`
	// path still covers the rare race where a block became old after fetch.
	auto submitted = ctx.block_processor.add_many (ordered, nano::block_source::bootstrap, /* channel */ nullptr, /* last_callback */ {}, std::any{ query_source::topology_index });
	debug_assert (submitted == ordered.size ()); // We wait for capacity before, so all should be submitted.

	ctx.stats.add (nano::stat::type::bootstrap_topo, nano::stat::detail::submitted, submitted);
}

std::optional<nano::topo_key> topo_strategy::wait_position ()
{
	std::optional<nano::topo_key> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		result = ctx.topology.next ();
		// Wake up if we have a position to query, or indexing is done.
		return result.has_value () || !ctx.topology.indexing ();
	});
	return result;
}

std::deque<nano::block_hash> topo_strategy::wait_block_batch ()
{
	std::deque<nano::block_hash> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		result = ctx.topology.next_blocks (ctx.config.topo_scan.block_batch_size);
		// Wake up if we have hashes to fetch, or there's nothing left to do.
		return !result.empty () || (!ctx.topology.has_blocks_pending () && !ctx.topology.indexing ());
	});
	return result;
}

std::deque<std::shared_ptr<nano::block>> topo_strategy::wait_ordered_blocks ()
{
	std::deque<std::shared_ptr<nano::block>> result;
	ctx.wait ([this, &result] () {
		debug_assert (!ctx.mutex.try_lock ());
		// Bound the drain so a single add_many can't overshoot the block
		// processor's per-source queue cap and silently drop blocks.
		result = ctx.topology.next_ordered_blocks (ctx.config.block_processor_threshold);
		// Wake up if we have blocks to submit, or there's nothing left to do.
		return !result.empty () || (!ctx.topology.has_blocks_pending () && !ctx.topology.indexing ());
	});
	return result;
}

std::shared_ptr<nano::transport::channel> topo_strategy::wait_topo_index_channel ()
{
	return ctx.wait_channel ([] (std::shared_ptr<nano::transport::channel> const & channel) {
		return channel->get_flags ().test (nano::node_capabilities::topo_index);
	});
}

bool topo_strategy::request_index (nano::topo_key cursor, std::shared_ptr<nano::transport::channel> const & channel)
{
	async_tag tag{};
	tag.type = query_type::topo_index;
	tag.source = query_source::topology_index;
	tag.hash = cursor.hash;

	topo_index_tag_payload payload{};
	payload.cursor = cursor;
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
	ctx.logger.debug (nano::log::type::bootstrap, "Requesting topo index from cursor topo_height={} hash={} from: {}", cursor.topo_height, cursor.hash.to_string (), channel);

	bool sent = ctx.send (channel, std::move (message), tag);
	if (sent)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_index);
	}
	else
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_index_failed);
	}
	return sent;
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

	// ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_blocks);
	ctx.logger.debug (nano::log::type::bootstrap, "Requesting {} random topology blocks from: {}", payload.hashes.size (), channel);

	bool sent = ctx.send (channel, std::move (message), tag);
	if (sent)
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_blocks);
	}
	else
	{
		ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::request_blocks_failed);
	}
	return sent;
}

verify_result topo_strategy::verify (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag) const
{
	release_assert (std::holds_alternative<topo_index_tag_payload> (tag.payload));
	auto const & payload = std::get<topo_index_tag_payload> (tag.payload);
	auto const & entries = response.entries;

	// Non-empty entries must be sorted strictly ascending and start at or past the requested cursor.
	// Empty entries are valid: they signal "peer has nothing past cursor" and feed end-of-topology detection.
	if (!entries.empty ())
	{
		if (entries.front () < payload.cursor)
		{
			return verify_result::invalid;
		}

		nano::topo_key previous{};
		for (auto const & entry : entries)
		{
			if (entry <= previous)
			{
				return verify_result::invalid;
			}
			previous = entry;
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
			ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::topo_index, nano::stat::dir::in, response.entries.size ());

			ctx.stats.inc (nano::stat::type::bootstrap_topo, nano::stat::detail::process);

			bool advanced = ctx.topology.process (payload.cursor, response.entries);
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
	ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

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
