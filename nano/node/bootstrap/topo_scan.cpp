#include <nano/node/bootstrap/topo_scan.hpp>

nano::bootstrap::topo_scan::topo_scan (topo_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
}

void nano::bootstrap::topo_scan::reset ()
{
	head.reset_progress ();
	blocks.clear ();
}

auto nano::bootstrap::topo_scan::next () -> std::optional<index_query>
{
	if (head.done)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
		return std::nullopt;
	}

	// Pause indexing when too many blocks are queued (lookahead limit)
	if (count_outstanding () >= config.max_blocks_queued)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
		return std::nullopt;
	}

	auto const cutoff = std::chrono::steady_clock::now () - config.cooldown;

	// Check if the head has pending requests or is in cooldown
	if (head.requests > 0 && head.timestamp > cutoff)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
		return std::nullopt;
	}

	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_topo_index);

	index_query result;
	result.start_index = head.cursor_index;
	result.start_hash = head.cursor_hash;

	head.requests += 1;
	head.timestamp = std::chrono::steady_clock::now ();

	return result;
}

bool nano::bootstrap::topo_scan::process (index_query start, std::deque<index_entry> const & entries)
{
	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::process);

	head.completed += 1;

	if (entries.empty ())
	{
		// Empty response signals end of topo space
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::done_empty);
		head.done = true;
		return true;
	}

	// Add entries to block tracking (phase 2)
	for (auto const & entry : entries)
	{
		auto & blocks_by_hash = blocks.get<tag_hash> ();
		if (blocks_by_hash.find (entry.hash) == blocks_by_hash.end ())
		{
			blocks.push_back ({ entry.hash, entry.topo_index, block_status::pending });
		}
	}

	// Advance cursor to the last entry
	auto const & last = entries.back ();
	head.cursor_index = last.topo_index;
	head.cursor_hash = last.hash;

	// Short response signals end of topo space
	if (entries.size () < config.index_batch_size)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::done);
		head.done = true;
		return true;
	}

	// Reset request tracking for next batch
	head.requests = 0;
	head.completed = 0;
	head.timestamp = {};

	return false;
}

std::deque<nano::block_hash> nano::bootstrap::topo_scan::next_blocks (std::size_t max_count)
{
	std::deque<nano::block_hash> result;

	auto in_flight = count_in_flight ();

	// Don't send more requests when too many are already in-flight
	if (in_flight >= config.max_blocks_outstanding)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
		return result;
	}

	// Cap how many new requests we can add
	auto const capacity = config.max_blocks_outstanding - in_flight;
	max_count = std::min (max_count, capacity);

	auto const now = std::chrono::steady_clock::now ();
	auto const retry_cutoff = now - config.block_retry;

	auto & blocks_by_order = blocks.get<tag_sequenced> ();
	for (auto it = blocks_by_order.begin (); it != blocks_by_order.end () && result.size () < max_count; ++it)
	{
		bool eligible = it->status == block_status::pending
						|| (it->status == block_status::in_flight && it->timestamp < retry_cutoff);

		if (eligible)
		{
			result.push_back (it->hash);
			blocks_by_order.modify (it, [now] (block_entry & entry) {
				entry.status = block_status::in_flight;
				entry.timestamp = now;
			});
		}
	}

	if (!result.empty ())
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_topo_block);
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
	}

	return result;
}

void nano::bootstrap::topo_scan::received (nano::block_hash const & hash)
{
	auto & blocks_by_hash = blocks.get<tag_hash> ();
	auto it = blocks_by_hash.find (hash);
	if (it != blocks_by_hash.end ())
	{
		blocks_by_hash.modify (it, [] (block_entry & entry) {
			entry.status = block_status::completed;
		});
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::received);
	}
}

bool nano::bootstrap::topo_scan::indexing () const
{
	return !head.done;
}

bool nano::bootstrap::topo_scan::has_blocks_pending () const
{
	return count_outstanding () > 0;
}

std::size_t nano::bootstrap::topo_scan::count_pending () const
{
	std::size_t count = 0;
	for (auto const & entry : blocks)
	{
		if (entry.status == block_status::pending)
		{
			++count;
		}
	}
	return count;
}

std::size_t nano::bootstrap::topo_scan::count_in_flight () const
{
	std::size_t count = 0;
	for (auto const & entry : blocks)
	{
		if (entry.status == block_status::in_flight)
		{
			++count;
		}
	}
	return count;
}

std::size_t nano::bootstrap::topo_scan::count_outstanding () const
{
	std::size_t count = 0;
	for (auto const & entry : blocks)
	{
		if (entry.status != block_status::completed)
		{
			++count;
		}
	}
	return count;
}

nano::container_info nano::bootstrap::topo_scan::container_info () const
{
	std::size_t pending = 0, in_flight = 0, completed = 0;
	for (auto const & entry : blocks)
	{
		switch (entry.status)
		{
			case block_status::pending:
				++pending;
				break;
			case block_status::in_flight:
				++in_flight;
				break;
			case block_status::completed:
				++completed;
				break;
		}
	}

	nano::container_info info;
	info.put ("blocks_total", blocks.size ());
	info.put ("blocks_pending", pending);
	info.put ("blocks_in_flight", in_flight);
	info.put ("blocks_completed", completed);
	info.put ("cursor_index", head.cursor_index);
	info.put ("indexing_done", head.done ? 1 : 0);
	return info;
}
