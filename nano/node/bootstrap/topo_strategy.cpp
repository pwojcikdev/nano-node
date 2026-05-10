#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/messages/asc_pull.hpp>
#include <nano/node/block_processor.hpp>
#include <nano/node/bootstrap/topo_strategy.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/transport/formatting.hpp>

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

	thread_index = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_index);
		run_index ();
	});

	thread_blocks = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::bootstrap_topo_blocks);
		run_blocks ();
	});
}

void topo_strategy::stop ()
{
	join_or_pass (thread_index);
	join_or_pass (thread_blocks);
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

		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo_index);
		run_one_index ();

		lock.lock ();
	}
}

void topo_strategy::run_one_index ()
{
	auto channel = ctx.wait_channel ();
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

		ctx.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::loop_topo_blocks);
		run_one_blocks ();

		lock.lock ();
	}
}

void topo_strategy::run_one_blocks ()
{
	ctx.wait_block_processor ();
	auto channel = ctx.wait_channel ();
	if (!channel)
	{
		return;
	}
	auto hashes = wait_block_batch ();
	if (hashes.empty ())
	{
		return;
	}
	request_blocks (std::move (hashes), channel);
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

	ctx.logger.debug (nano::log::type::bootstrap, "Requesting topo index from cursor topo_height={} hash={} from: {}", cursor.topo_height, cursor.hash.to_string (), channel);

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

	ctx.logger.debug (nano::log::type::bootstrap, "Requesting {} random topology blocks from: {}", payload.hashes.size (), channel);

	return ctx.send (channel, std::move (message), tag);
}

bool topo_strategy::process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.type == query_type::topo_index);

	release_assert (std::holds_alternative<topo_index_tag_payload> (tag.payload));
	auto const & payload = std::get<topo_index_tag_payload> (tag.payload);

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::topo_index);

	// Validate response: entries must be strictly increasing in topo_key order
	// and not below the requested cursor.
	auto const & entries = response.entries;
	for (std::size_t n = 0; n < entries.size (); ++n)
	{
		if (entries[n] <= payload.cursor)
		{
			ctx.stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::invalid);
			return false;
		}
		if (n > 0 && !(entries[n - 1] < entries[n]))
		{
			ctx.stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::invalid);
			return false;
		}
	}

	ctx.topology.process (payload.cursor, entries);
	return true;
}

bool topo_strategy::process_blocks (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag)
{
	debug_assert (!ctx.mutex.try_lock ());
	debug_assert (tag.source == query_source::topology_blocks);

	ctx.stats.inc (nano::stat::type::bootstrap_process, nano::stat::detail::blocks);
	ctx.stats.add (nano::stat::type::bootstrap, nano::stat::detail::blocks, nano::stat::dir::in, response.blocks.size ());

	for (auto const & block : response.blocks)
	{
		if (!block)
		{
			continue;
		}
		ctx.topology.block_received (block->hash (), block);
	}

	auto ordered = ctx.topology.next_ordered_blocks (std::numeric_limits<std::size_t>::max ());
	for (auto const & block : ordered)
	{
		ctx.block_processor.add (block, nano::block_source::bootstrap);
	}
	return true;
}
}
