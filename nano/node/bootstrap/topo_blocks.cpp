#include <nano/lib/blocks.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/topo_blocks.hpp>

namespace nano::bootstrap
{
topo_blocks::topo_blocks (topo_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
}

void topo_blocks::add (std::deque<nano::topo_key> const & new_entries)
{
	auto & by_key = entries.get<tag_key> ();
	for (auto const & key : new_entries)
	{
		auto it = by_key.find (key);
		if (it == by_key.end ())
		{
			by_key.insert (entry{ .key = key });
			++pending;
			stats.inc (nano::stat::type::bootstrap_topo_fetch, nano::stat::detail::pending);
		}
		else if (it->state == entry_state::skipped)
		{
			// Repair re-discovery: give a previously abandoned gap another chance
			by_key.modify (it, [] (entry & e) {
				e.state = entry_state::pending;
				e.attempts = 0;
				e.last_requested = {};
			});
			++pending;
		}
	}
}

bool topo_blocks::has_fetchable (std::chrono::steady_clock::time_point now) const
{
	for (auto const & e : entries.get<tag_key> ())
	{
		if (e.state != entry_state::pending)
		{
			continue;
		}
		// Over-attempt entries are "fetchable" so the fetch loop runs and demotes them to tolerated gaps
		if (e.attempts >= config.max_fetch_attempts)
		{
			return true;
		}
		if (e.last_requested == std::chrono::steady_clock::time_point{} || (now - e.last_requested) >= config.fetch_cooldown)
		{
			return true;
		}
	}
	return false;
}

std::deque<nano::block_hash> topo_blocks::next_fetch_batch (std::size_t max, std::chrono::steady_clock::time_point now)
{
	std::deque<nano::block_hash> batch;
	auto & by_key = entries.get<tag_key> ();

	for (auto it = by_key.begin (); it != by_key.end () && batch.size () < max; ++it)
	{
		if (it->state != entry_state::pending)
		{
			continue;
		}
		if (it->attempts >= config.max_fetch_attempts)
		{
			// Abandon as a tolerated gap so it never blocks the submit cursor
			by_key.modify (it, [] (entry & e) { e.state = entry_state::skipped; });
			--pending;
			stats.inc (nano::stat::type::bootstrap_topo_fetch, nano::stat::detail::skip);
			continue;
		}
		bool const ready = it->last_requested == std::chrono::steady_clock::time_point{} || (now - it->last_requested) >= config.fetch_cooldown;
		if (ready)
		{
			batch.push_back (it->key.hash);
			by_key.modify (it, [now] (entry & e) {
				e.last_requested = now;
				e.attempts += 1;
			});
		}
	}

	return batch;
}

void topo_blocks::process_fetched (std::deque<std::shared_ptr<nano::block>> const & blocks)
{
	auto & by_hash = entries.get<tag_hash> ();
	for (auto const & block : blocks)
	{
		auto it = by_hash.find (block->hash ());
		if (it != by_hash.end () && it->state == entry_state::pending)
		{
			by_hash.modify (it, [&block] (entry & e) {
				e.state = entry_state::fetched;
				e.block = block;
			});
			--pending;
			stats.inc (nano::stat::type::bootstrap_topo_fetch, nano::stat::detail::fetched);
		}
	}
}

void topo_blocks::rearm (std::deque<nano::block_hash> const & hashes)
{
	auto & by_hash = entries.get<tag_hash> ();
	for (auto const & hash : hashes)
	{
		auto it = by_hash.find (hash);
		if (it != by_hash.end () && it->state == entry_state::pending)
		{
			// Make immediately eligible again; the spent attempt still counts toward backoff
			by_hash.modify (it, [] (entry & e) { e.last_requested = {}; });
		}
	}
}

bool topo_blocks::has_submittable () const
{
	auto const & by_key = entries.get<tag_key> ();
	if (by_key.empty ())
	{
		return false;
	}
	auto const & front = *by_key.begin ();
	return front.state == entry_state::fetched || front.state == entry_state::skipped;
}

std::deque<std::shared_ptr<nano::block>> topo_blocks::next_submit_batch (std::size_t max)
{
	std::deque<std::shared_ptr<nano::block>> batch;
	auto & by_key = entries.get<tag_key> ();

	for (auto it = by_key.begin (); it != by_key.end () && batch.size () < max;)
	{
		if (it->state == entry_state::pending)
		{
			break; // Real gap: cannot release anything past it in topological order
		}
		if (it->state == entry_state::fetched)
		{
			batch.push_back (it->block);
			stats.inc (nano::stat::type::bootstrap_topo_submit, nano::stat::detail::submitted);
		}
		else
		{
			stats.inc (nano::stat::type::bootstrap_topo_submit, nano::stat::detail::gap);
		}
		// Both fetched and tolerated-gap entries are consumed as the cursor advances
		it = by_key.erase (it);
	}

	return batch;
}

std::size_t topo_blocks::pending_count () const
{
	return pending;
}

void topo_blocks::reset ()
{
	entries.clear ();
	pending = 0;
}

nano::container_info topo_blocks::container_info () const
{
	nano::container_info info;
	info.put ("total", entries.size ());
	info.put ("pending", pending);
	return info;
}
}
