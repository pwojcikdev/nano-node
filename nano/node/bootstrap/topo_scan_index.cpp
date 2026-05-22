#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>

#include <algorithm>

namespace nano::bootstrap
{
topo_scan_index::topo_scan_index (nano::topo_scan_config const & config_a) :
	config{ config_a },
	rollback_distance{ config_a.rollback_min }
{
}

std::optional<nano::topo_key> topo_scan_index::next (std::chrono::steady_clock::time_point now)
{
	if (head.done)
	{
		return std::nullopt;
	}

	// Backpressure: pause discovery while the queue is full. This is the
	// coupling between discovery and processing.
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
		// A peer actually delivered a block: this cycle has network reach, so a
		// later stall (if any) is a real anchor problem, not an outage.
		fetched_since_reset++;

		// A tardy peer delivery must not backtrack a block past `received`.
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

	// Phase 1: drain the contiguous `redundant` prefix at the head, advancing
	// the cursor. Consuming only the head and stopping at the first
	// non-redundant entry keeps the advance in topological order — it can't
	// skip past unconfirmed work. Unbounded by `max_count` so a rollback
	// re-walk's long known-block prefix catches up in one pass.
	while (!by_seq.empty ())
	{
		auto it = by_seq.begin ();
		if (it->state != block_state::redundant)
		{
			break;
		}
		// topo_height==0: pre-topo-index block — evicted, but not a cursor
		// anchor. `mark_indexed` is monotonic.
		if (it->ledger_topo_height > 0)
		{
			mark_indexed (it->hash, it->ledger_topo_height);
		}
		--redundant_count;
		by_seq.erase (it);
		drained_at = now; // queue moved — don't mistake a fast drain for a stall
	}

	// Phase 2: collect up to `max_count` `received` blocks for submission.
	std::deque<std::shared_ptr<nano::block>> result;
	for (auto it = by_seq.begin (); it != by_seq.end () && result.size () < max_count; ++it)
	{
		// Skip past (don't stop on) submitted entries and interior redundant
		// entries so later `received` blocks can still be submitted.
		if (it->state == block_state::submitted || it->state == block_state::redundant)
		{
			continue;
		}
		// Stop at the first not-yet-received entry — preserves topological order.
		if (it->state != block_state::received)
		{
			break;
		}
		result.push_back (it->block);
		// Transition to `submitted` (not erase) so the slot still counts toward
		// backpressure; `block_done` evicts it once the processor confirms.
		state_change (block_state::received, block_state::submitted);
		by_seq.modify (it, [] (block_entry & entry) {
			entry.state = block_state::submitted;
		});
	}
	return result;
}

void topo_scan_index::block_done (nano::block_hash const & hash, std::chrono::steady_clock::time_point now)
{
	if (!erase_tracked (hash))
	{
		// Untracked hash (e.g. a dropped/pre-reset submission). Not queue
		// movement — must not refresh the heartbeat, or unrelated ledger
		// activity would mask a stalled queue.
		return;
	}

	// A tracked block left the queue: refresh the drain heartbeat.
	drained_at = now;
}

void topo_scan_index::block_failed (nano::block_hash const & hash)
{
	// A submitted block came back as a terminal non-progress result (gap / fork
	// / bad signature). Free the slot so it stops counting toward backpressure,
	// but deliberately do NOT touch `drained_at`: a gap is not forward progress,
	// and refreshing the heartbeat here would prevent `check_poisoning` from
	// ever detecting a gap-induced stall (discovery would just run to the end of
	// the topology, gapping everything above the hole). A miss (untracked hash)
	// is a no-op. Gap accounting is separate (`note_failed`): this evicts every
	// terminal result, gap or not.
	erase_tracked (hash);
}

void topo_scan_index::note_progress ()
{
	progressed_since_reset++;
}

void topo_scan_index::note_failed ()
{
	failed_since_reset++;
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

void topo_scan_index::mark_redundant (nano::block_hash const & hash, uint64_t ledger_topo_height)
{
	auto & by_hash = blocks.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return; // Raced away (e.g. concurrent live `block_done`).
	}
	if (it->state == block_state::redundant)
	{
		return; // Already tagged.
	}
	// Tag only. The cursor advance is applied in topological order at the
	// contiguous head by `next_ordered_blocks`, so this is order-independent.
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
		// Eligible for refetch if in-flight but past the retry cutoff
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
	fetched_since_reset = 0;
	progressed_since_reset = 0;
	failed_since_reset = 0;
	// High-water baseline for the cycle's progress check. A rewind lowers
	// `indexed` before this runs, but `std::max` keeps the baseline at the old
	// frontier — so re-confirming the rewound-over region climbs `indexed` back
	// up to (not past) the baseline and is correctly seen as non-progress.
	indexed_at_reset = std::max (indexed_at_reset, indexed);
}

void topo_scan_index::reset ()
{
	head.reset ();
	indexed = {};
	indexed_at_reset = {};
	drained_at = std::chrono::steady_clock::now ();
	rollback_distance = config.rollback_min;
	fetched_since_reset = 0;
	progressed_since_reset = 0;
	failed_since_reset = 0;
	blocks.clear ();
	pending_count = 0;
	in_flight_count = 0;
	received_count = 0;
	submitted_count = 0;
	redundant_count = 0;
}

void topo_scan_index::rewind (uint64_t distance)
{
	auto const height = indexed.topo_height;
	auto const target = height > distance ? height - distance : 0;
	// Drop the hash: we want the peer to walk from the start of `target`, not
	// from some specific (and now meaningless) block at the old height.
	indexed = nano::topo_key{ target, nano::block_hash{ 0 } };
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

	// Stuck. Was the cycle productive? Either confirmations outnumbered gaps
	// (`progressed_since_reset > failed_since_reset` — a trickle of inserts amid
	// mostly-gapping blocks does NOT qualify; the cursor is over holes it can't
	// reach), or `indexed` advanced into never-before-seen territory
	// (`indexed > indexed_at_reset` — the redundant drain catching up to our
	// ledger's true extent). Merely re-confirming the region a previous rewind
	// walked back over is neither: it only climbs `indexed` up to the preserved
	// high-water baseline, which is what lets the escalation keep doubling toward
	// a deep gap instead of resetting every cycle. If productive, the anchor is
	// workable: clear the stuck tail, retry from `indexed`, re-arm the
	// escalation. Non-destructive (no rewind), so taken regardless of connectivity.
	//
	// NOTE: this only takes effect once the body runs, i.e. after the drain
	// heartbeat goes stale. A confirmation trickle faster than `poisoning_timeout`
	// keeps `drained_at` warm (via `block_done`) and bypasses this entirely —
	// that regime needs a failure-dominance trigger or targeted gap repair.
	if (progressed_since_reset > failed_since_reset || indexed > indexed_at_reset)
	{
		reset_discovery ();
		rollback_distance = config.rollback_min;
		drained_at = now;
		return true;
	}

	// No frontier progress. Rewinding `indexed` is destructive, so only do it
	// with evidence the data itself is bad — we fetched blocks but they
	// wouldn't connect. If no peer delivered anything this cycle the stall is a
	// connectivity outage: leave `indexed` alone (rewinding here would discard
	// correct progress and keep doubling for the whole outage); `cleanup`
	// retries the in-flight requests when the network returns.
	if (!fetched_since_reset)
	{
		return false;
	}

	// `indexed` sits past a gap the ledger can't bridge. Rewind it and widen
	// the next step so repeated stalls bisect back in O(log n).
	rewind (rollback_distance);
	rollback_distance = std::min (rollback_distance * 2, config.rollback_max);
	reset_discovery ();
	drained_at = now;
	return true;
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
		info.put ("delta", head.cursor.topo_height > indexed.topo_height ? head.cursor.topo_height - indexed.topo_height : 0);
		return info;
	};

	auto collect_counters = [&] () {
		nano::container_info info;
		info.put ("fetched_since_reset", fetched_since_reset);
		info.put ("progressed_since_reset", progressed_since_reset);
		info.put ("failed_since_reset", failed_since_reset);
		info.put ("rollback_distance", rollback_distance);
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

	info.add ("counters", collect_counters ());
	info.add ("cursors", collect_cursors ());
	return info;
}
}
