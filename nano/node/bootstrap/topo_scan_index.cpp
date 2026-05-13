#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>

namespace nano::bootstrap
{
topo_scan_index::topo_scan_index (nano::topo_scan_config const & config_a) :
	config{ config_a }
{
}

void topo_scan_index::reset ()
{
	head.reset ();
	indexed = {};
	indexed_advanced_at = std::chrono::steady_clock::now ();
	blocks.clear ();
	pending_count = 0;
	in_flight_count = 0;
	completed_count = 0;
}

std::optional<nano::topo_key> topo_scan_index::next (std::chrono::steady_clock::time_point now)
{
	if (head.done)
	{
		return std::nullopt;
	}

	// Pause indexing under block-fetch backpressure. This is what couples
	// discovery to processing: when the queue is full because `mark_indexed`
	// hasn't caught up, discovery pauses until blocks drain.
	if (count_outstanding () >= config.max_blocks_queued)
	{
		return std::nullopt;
	}

	auto const cutoff = now - config.cooldown;

	// Eligible for query if we have capacity to consider more candidates, or if cooldown has elapsed since last request
	bool eligible = head.requests < config.consideration_count || head.timestamp < cutoff;
	if (!eligible)
	{
		return std::nullopt;
	}

	head.requests += 1;
	head.timestamp = now;

	return head.cursor;
}

bool topo_scan_index::process (nano::topo_key const start, std::deque<nano::topo_key> const & entries)
{
	// Keys in response must be in ascending order
	debug_assert (std::is_sorted (entries.begin (), entries.end ()));

	// Reject stale responses (cursor already advanced past `start`)
	if (start != head.cursor)
	{
		return false; // Not advanced
	}

	head.completed += 1;

	// Accumulate union of strictly-post-cursor entries
	for (auto const & entry : entries)
	{
		if (entry > head.cursor)
		{
			head.candidates.insert (entry);
		}
	}

	// Wait for full quorum of responses before finalizing
	if (head.completed < config.consideration_count)
	{
		return false; // Not advanced
	}

	// All consideration_count responses received - finalize
	if (head.candidates.empty () && head.completed >= config.consideration_count * 2)
	{
		// Every queried peer reported nothing past the cursor: topology end
		head.done = true;
		return true; // Advanced (to end)
	}

	if (head.candidates.empty ())
	{
		return false; // Not advanced (not enough candidates, but not enough responses to conclude end)
	}

	// Insert kept entries into the block-fetch queue, deduped against existing by multiindex constraints
	nano::topo_key last_inserted{};
	size_t kept_count = 0;
	for (auto const & candidate : head.candidates)
	{
		// Trim union to only the first `candidates` entries
		if (kept_count >= config.candidates)
		{
			break;
		}

		block_entry entry{};
		entry.hash = candidate.hash;
		entry.key = candidate;
		entry.state = block_state::pending;

		auto [_, inserted] = blocks.push_back (entry);
		if (inserted)
		{
			++pending_count;
		}

		last_inserted = candidate;
		++kept_count;
	}

	// Advance discovery cursor to the last kept entry
	debug_assert (head.cursor < last_inserted);
	head.cursor = last_inserted;
	head.requests = 0;
	head.completed = 0;
	head.timestamp = {};
	head.candidates.clear ();

	return true; // Advanced
}

void topo_scan_index::mark_indexed (nano::block_hash const & hash, uint64_t topo_height, std::chrono::steady_clock::time_point now)
{
	debug_assert (topo_height > 0); // topo_height=0 is reserved for "not in topology"

	nano::topo_key const new_key{ topo_height, hash };
	if (new_key <= indexed)
	{
		return; // Already at or past this position
	}

	indexed = new_key;
	indexed_advanced_at = now;
}

void topo_scan_index::reset_discovery ()
{
	head.reset ();
	head.cursor = indexed;
	blocks.clear ();
	pending_count = 0;
	in_flight_count = 0;
	completed_count = 0;
	// The indexed cursor itself is preserved — that's the whole point: we trust
	// the closed-loop position and resume open-loop discovery from there.
}

bool topo_scan_index::check_poisoning (std::chrono::steady_clock::time_point now)
{
	// Healthy state: discovery cursor is at or behind the indexed cursor. Nothing
	// to roll back to.
	if (head.cursor <= indexed)
	{
		return false;
	}

	auto const since_progress = now - indexed_advanced_at;
	if (since_progress < config.poisoning_timeout)
	{
		return false;
	}

	reset_discovery ();
	// Reset the progress clock so the next round of attempts gets a full timeout
	// window before we give up again.
	indexed_advanced_at = now;
	return true;
}

std::deque<nano::block_hash> topo_scan_index::next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now)
{
	std::deque<nano::block_hash> result;

	auto const retry_cutoff = now - config.block_retry;

	// Pause block fetching if we have too many outstanding requests (backpressure)
	if (count_in_flight () >= config.max_blocks_outstanding)
	{
		return result;
	}

	// Iterate in order of insertion (topological order)
	auto & by_seq = blocks.get<tag_sequenced> ();
	for (auto it = by_seq.begin (); it != by_seq.end () && result.size () < max_count; ++it)
	{
		// Eligible for fetch if pending, or in-flight but past retry cutoff
		bool const eligible = it->state == block_state::pending || (it->state == block_state::in_flight && it->timestamp < retry_cutoff);
		if (eligible)
		{
			result.push_back (it->hash);
			state_change (it->state, block_state::in_flight);
			by_seq.modify (it, [now] (block_entry & entry) {
				entry.state = block_state::in_flight;
				entry.timestamp = now;
			});
		}
	}

	return result;
}

void topo_scan_index::block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block)
{
	auto & by_hash = blocks.get<tag_hash> ();
	if (auto it = by_hash.find (hash); it != by_hash.end ())
	{
		state_change (it->state, block_state::completed);
		by_hash.modify (it, [&block] (block_entry & entry) {
			entry.state = block_state::completed;
			entry.block = block;
		});
	}
}

std::deque<std::shared_ptr<nano::block>> topo_scan_index::next_ordered_blocks (std::size_t max_count)
{
	std::deque<std::shared_ptr<nano::block>> result;
	auto & by_seq = blocks.get<tag_sequenced> ();
	while (!by_seq.empty () && result.size () < max_count)
	{
		auto it = by_seq.begin ();
		if (it->state != block_state::completed)
		{
			break; // Stop at first non-completed to maintain topological order
		}
		result.push_back (it->block);
		--completed_count;
		by_seq.erase (it);
	}
	return result;
}

void topo_scan_index::block_done (nano::block_hash const & hash)
{
	auto & by_hash = blocks.get<tag_hash> ();
	if (auto it = by_hash.find (hash); it != by_hash.end ())
	{
		switch (it->state)
		{
			case block_state::pending:
				--pending_count;
				break;
			case block_state::in_flight:
				--in_flight_count;
				break;
			case block_state::completed:
				--completed_count;
				break;
		}
		by_hash.erase (it);
	}
}

std::size_t topo_scan_index::cleanup (std::chrono::steady_clock::time_point now)
{
	auto const stale_cutoff = now - config.block_retry;

	std::size_t reset_count = 0;
	auto & by_seq = blocks.get<tag_sequenced> ();
	for (auto it = by_seq.begin (); it != by_seq.end (); ++it)
	{
		if (it->state == block_state::in_flight && it->timestamp < stale_cutoff)
		{
			state_change (it->state, block_state::pending);
			by_seq.modify (it, [] (block_entry & entry) {
				entry.state = block_state::pending;
				entry.timestamp = {};
			});
			++reset_count;
		}
	}
	return reset_count;
}

void topo_scan_index::state_change (block_state old_state, block_state new_state)
{
	switch (old_state)
	{
		case block_state::pending:
			--pending_count;
			break;
		case block_state::in_flight:
			--in_flight_count;
			break;
		case block_state::completed:
			--completed_count;
			break;
	}
	switch (new_state)
	{
		case block_state::pending:
			++pending_count;
			break;
		case block_state::in_flight:
			++in_flight_count;
			break;
		case block_state::completed:
			++completed_count;
			break;
	}
}

bool topo_scan_index::indexing () const
{
	return !head.done;
}

nano::topo_key topo_scan_index::cursor () const
{
	return indexed;
}

bool topo_scan_index::has_blocks_pending () const
{
	return !blocks.empty ();
}

std::size_t topo_scan_index::count_outstanding () const
{
	return blocks.size ();
}

std::size_t topo_scan_index::count_pending () const
{
	return pending_count;
}

std::size_t topo_scan_index::count_in_flight () const
{
	return in_flight_count;
}

std::size_t topo_scan_index::count_completed () const
{
	return completed_count;
}

nano::container_info topo_scan_index::container_info () const
{
	nano::container_info info;
	info.put ("blocks_outstanding", blocks.size ());
	info.put ("blocks_pending", pending_count);
	info.put ("blocks_in_flight", in_flight_count);
	info.put ("blocks_completed", completed_count);
	info.put ("cursor_height", head.cursor.topo_height);
	info.put ("indexed_height", indexed.topo_height);
	info.put ("indexing_done", head.done ? std::size_t{ 1 } : std::size_t{ 0 });
	info.put ("candidates", head.candidates.size ());
	return info;
}
}
