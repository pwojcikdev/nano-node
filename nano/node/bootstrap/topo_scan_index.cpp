#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>

namespace nano::bootstrap
{
topo_scan_index::topo_scan_index (nano::topo_scan_config const & config_a) :
	config{ config_a },
	rollback_distance{ config_a.rollback_min }
{
}

void topo_scan_index::reset ()
{
	head.reset ();
	indexed = {};
	indexed_at_reset = {};
	drained_at = std::chrono::steady_clock::now ();
	rollback_distance = config.rollback_min;
	blocks.clear ();
	pending_count = 0;
	in_flight_count = 0;
	received_count = 0;
	submitted_count = 0;
	redundant_count = 0;
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

void topo_scan_index::mark_indexed (nano::block_hash const & hash, uint64_t topo_height)
{
	debug_assert (topo_height > 0); // topo_height=0 is reserved for "not in topology"

	nano::topo_key const new_key{ topo_height, hash };
	if (new_key <= indexed)
	{
		return; // Already at or past this position
	}

	indexed = new_key;
}

void topo_scan_index::rewind (uint64_t distance)
{
	auto const height = indexed.topo_height;
	auto const target = height > distance ? height - distance : 0;
	// Drop the hash: we want the peer to walk from the start of `target`, not
	// from some specific (and now meaningless) block at the old height.
	indexed = nano::topo_key{ target, nano::block_hash{ 0 } };
}

void topo_scan_index::reset_discovery ()
{
	head.reset ();
	head.cursor = indexed;
	blocks.clear ();
	pending_count = 0;
	in_flight_count = 0;
	received_count = 0;
	submitted_count = 0;
	redundant_count = 0;
	// Baseline the new cycle's progress check against wherever `indexed` is now
	// (post-rewind, if `check_poisoning` rewound it before calling this).
	// Forward progress == `indexed` moving strictly past this snapshot.
	indexed_at_reset = indexed;
}

bool topo_scan_index::check_poisoning (std::chrono::steady_clock::time_point now)
{
	// Nothing queued → nothing can be stuck.
	if (count_outstanding () == 0)
	{
		return false;
	}

	// The queue drained recently → healthy, even if slow.
	auto const since_drain = now - drained_at;
	if (since_drain < config.poisoning_timeout)
	{
		return false;
	}

	// Stuck: outstanding work, but no tracked block has drained for a full
	// timeout window. Decide based on *real forward progress* — did `indexed`
	// advance since this cycle started? Re-chewing already-known blocks as
	// `old` keeps the queue moving (and was holding the trigger off) but does
	// not advance `indexed`, so it does not count here.
	bool const advanced = indexed > indexed_at_reset;
	if (advanced)
	{
		// The frontier genuinely moved — the anchor is workable. Clear the
		// stuck tail and retry from the new (higher) `indexed`. Re-arm the
		// escalation step.
		reset_discovery ();
		rollback_distance = config.rollback_min;
	}
	else
	{
		// No frontier progress at all: `indexed` sits past a gap the ledger
		// can't bridge. Rewind it, then widen the next step so consecutive
		// failures walk back through history in O(log n) steps.
		rewind (rollback_distance);
		rollback_distance = std::min (rollback_distance * 2, config.rollback_max);
		reset_discovery ();
	}

	// Give the new attempt a full timeout window before judging it again.
	drained_at = now;
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
		// Once a block has been drained to the processor (submitted), already
		// received, or tagged redundant (it's in our ledger — never fetch it),
		// a tardy peer delivery must not backtrack the state.
		if (it->state == block_state::submitted || it->state == block_state::received || it->state == block_state::redundant)
		{
			return;
		}
		state_change (it->state, block_state::received);
		by_hash.modify (it, [&block] (block_entry & entry) {
			entry.state = block_state::received;
			entry.block = block;
		});
	}
}

std::deque<std::shared_ptr<nano::block>> topo_scan_index::next_ordered_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now)
{
	auto & by_seq = blocks.get<tag_sequenced> ();

	// Phase 1: drain the strictly-contiguous `redundant` prefix at the head.
	// Because we only ever consume the head and stop at the first
	// non-redundant entry, the cursor advance stays in topological order and
	// can't jump past unconfirmed work — this is the gap-safe equivalent of
	// the processor `old` path, without the network/processor round trip.
	// Unbounded by `max_count`: a rollback re-walk can have a huge prefix of
	// already-known blocks and we want the cursor to catch up in one pass.
	while (!by_seq.empty ())
	{
		auto it = by_seq.begin ();
		if (it->state != block_state::redundant)
		{
			break;
		}
		// (topo_height==0 ⇒ pre-topo-index block: still redundant/evicted, just
		// not a cursor anchor.) `mark_indexed` is monotonic.
		if (it->ledger_topo_height > 0)
		{
			mark_indexed (it->hash, it->ledger_topo_height);
		}
		--redundant_count;
		by_seq.erase (it);
		// Genuine queue movement — keep the poisoning trigger from mistaking a
		// fast redundant-prefix drain (rollback recovery) for a stall.
		drained_at = now;
	}

	// Phase 2: collect up to `max_count` `received` blocks for submission.
	std::deque<std::shared_ptr<nano::block>> result;
	for (auto it = by_seq.begin (); it != by_seq.end () && result.size () < max_count; ++it)
	{
		// Already handed to the processor on a prior call, or a redundant entry
		// not (yet) at the contiguous head — skip past it but don't stop, so
		// later `received` blocks can still be submitted for throughput.
		if (it->state == block_state::submitted || it->state == block_state::redundant)
		{
			continue;
		}
		// Stop at the first not-yet-received (pending / in_flight) entry —
		// preserves the topological-order guarantee.
		if (it->state != block_state::received)
		{
			break;
		}
		result.push_back (it->block);
		// Transition into `submitted` rather than erasing: keep the slot in the
		// queue so it still counts toward `count_outstanding` (backpressure)
		// and so a re-discovered topo_key doesn't trigger a redundant fetch.
		// `block_done` (fired by the inspect callback) is what finally evicts
		// the entry once the block_processor confirms.
		state_change (block_state::received, block_state::submitted);
		by_seq.modify (it, [] (block_entry & entry) {
			entry.state = block_state::submitted;
		});
	}
	return result;
}

bool topo_scan_index::erase_tracked (nano::block_hash const & hash)
{
	auto & by_hash = blocks.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return false;
	}
	switch (it->state)
	{
		case block_state::pending:
			--pending_count;
			break;
		case block_state::in_flight:
			--in_flight_count;
			break;
		case block_state::received:
			--received_count;
			break;
		case block_state::submitted:
			--submitted_count;
			break;
		case block_state::redundant:
			--redundant_count;
			break;
	}
	by_hash.erase (it);
	return true;
}

void topo_scan_index::block_done (nano::block_hash const & hash, std::chrono::steady_clock::time_point now)
{
	if (!erase_tracked (hash))
	{
		// Not a tracked block (e.g. a ghost completion of a pre-reset / dropped
		// submission). Deliberately NOT counted as progress — otherwise stalled
		// queues would be masked by unrelated ledger activity.
		return;
	}

	// Drain heartbeat: a tracked block actually left the queue. Refreshes the
	// poisoning *trigger* clock only — this fires for `old` re-processing too,
	// so it must NOT be treated as forward progress (that's `indexed`'s job).
	drained_at = now;
}

void topo_scan_index::mark_redundant (nano::block_hash const & hash, uint64_t ledger_topo_height)
{
	auto & by_hash = blocks.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		// Raced away (e.g. concurrent live `block_done` evicted it). The block
		// is in the ledger anyway, so nothing to do — discovery won't re-list
		// it past the cursor.
		return;
	}
	if (it->state == block_state::redundant)
	{
		return; // Already tagged.
	}
	// Tag only — do NOT advance the cursor here. The cursor advance is applied
	// strictly in topological order at the contiguous queue head by
	// `next_ordered_blocks`, which keeps it gap-safe regardless of the
	// (parallel, unordered) fetch order this is called from.
	state_change (it->state, block_state::redundant);
	by_hash.modify (it, [ledger_topo_height] (block_entry & entry) {
		entry.state = block_state::redundant;
		entry.ledger_topo_height = ledger_topo_height;
	});
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
		case block_state::received:
			--received_count;
			break;
		case block_state::submitted:
			--submitted_count;
			break;
		case block_state::redundant:
			--redundant_count;
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
		case block_state::received:
			++received_count;
			break;
		case block_state::submitted:
			++submitted_count;
			break;
		case block_state::redundant:
			++redundant_count;
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

std::size_t topo_scan_index::count_received () const
{
	return received_count;
}

std::size_t topo_scan_index::count_submitted () const
{
	return submitted_count;
}

std::size_t topo_scan_index::count_redundant () const
{
	return redundant_count;
}

nano::container_info topo_scan_index::container_info () const
{
	auto collect_cursors = [&] () {
		nano::container_info info;
		info.put ("cursor", head.cursor.topo_height);
		info.put ("indexed", indexed.topo_height);
		return info;
	};

	nano::container_info info;
	info.put ("blocks", blocks.size ());
	info.put ("blocks_pending", pending_count);
	info.put ("blocks_in_flight", in_flight_count);
	info.put ("blocks_received", received_count);
	info.put ("blocks_submitted", submitted_count);
	info.put ("blocks_redundant", redundant_count);
	info.put ("indexing_done", head.done ? std::size_t{ 1 } : std::size_t{ 0 });
	info.put ("candidates", head.candidates.size ());
	info.put ("rollback_distance", rollback_distance);
	info.add ("cursors", collect_cursors ());
	return info;
}
}
