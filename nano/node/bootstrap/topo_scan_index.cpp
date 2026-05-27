#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>

#include <algorithm>

namespace nano::bootstrap
{
topo_scan_index::topo_scan_index (nano::topo_scan_config const & config_a) :
	config{ config_a }
{
	reset ();
}

std::size_t topo_scan_index::head_count () const
{
	return heads.size ();
}

bool topo_scan_index::eligible (scan_head const & head, std::chrono::steady_clock::time_point now) const
{
	auto const cutoff = now - config.cooldown;
	// Capacity to issue another quorum request (the strategy steers each to a distinct peer),
	// or the cooldown since the last request at this cursor has elapsed.
	return head.requests < head_target (head) || head.timestamp < cutoff;
}

std::optional<nano::topo_key> topo_scan_index::next (std::size_t h, std::chrono::steady_clock::time_point now)
{
	debug_assert (h < heads.size ());
	auto & head = heads[h];

	if (!head.repair)
	{
		// Spear: scan the frontier forward.
		if (head.done)
		{
			return std::nullopt; // Topology end reached (re-armed by poll mode).
		}
		// Pause once pending gaps reach the threshold: wait for the repair heads to
		// clear every gap (the latch resets at zero) before extending the frontier. This is
		// the ONLY gap-driven pause — below the threshold gaps never stall the spear.
		if (repair_wait)
		{
			return std::nullopt;
		}
		// Backpressure: pause discovery while the active (unconfirmed) window is full, so we
		// don't outrun fetching/processing. Confirmed-but-unpruned members do not count (see
		// outstanding()), so gaps wedging the prune can't trip this.
		if (outstanding () >= config.max_blocks_queued)
		{
			return std::nullopt;
		}
		if (!eligible (head, now))
		{
			return std::nullopt;
		}
		head.requests += 1;
		head.timestamp = now;
		return head.cursor;
	}

	// Repair head: continuous UPWARD sweep (normal topo order — dependencies before
	// dependents, so the fetch/submit pipeline can drain what it discovers) within a
	// FROZEN [floor, ceiling] range. Quiesce once the spear reached the tip and no gap
	// remains, so re-discovery of already-synced keys stops and `members` can drain to
	// empty (else `caught_up` could never be reached). A paused spear (gapped_count > 0)
	// leaves repair active.
	if (heads[0].done && gapped_count == 0)
	{
		return std::nullopt;
	}

	nano::topo_key const spear = heads[0].cursor;
	// Kick in only once the spear frontier has climbed past the activation threshold.
	// This skips the dense, dependency-free low layer (topo_height ~1: genesis + the
	// many epoch-open blocks) the spear grinds through at the start — re-scanning it just
	// thrashes, and those root blocks can never gap so they never need repair.
	if (spear.topo_height < config.repair_activation_height)
	{
		return std::nullopt;
	}

	// FREEZE the sweep range at the current spear frontier on first start and on every
	// wrap; hold it fixed in between. The ceiling is a SNAPSHOT of the spear, not the
	// live frontier — so a fast-moving spear can't drag the ceiling away and make the
	// head chase it forever (which degenerates into one endless trailing sweep that never
	// re-scans). Head h covers the top ceiling/2^(h-1): head 1 the full [0, ceiling]
	// (floor 0 — the guarantor that every key is re-scanned), head 2 [ceiling/2, ceiling],
	// head 3 [3*ceiling/4, ceiling], … The floor is derived from the frozen ceiling, so
	// both are fixed for the whole sweep.
	if (head.ceiling == 0 || head.cursor.topo_height >= head.ceiling)
	{
		head.ceiling = spear.topo_height; // snapshot / re-snapshot at the wrap
		uint64_t const shift = h - 1;
		uint64_t const reach = (shift >= 64) ? 0 : (head.ceiling >> shift);
		uint64_t const floor_height = head.ceiling - reach;
		head.cursor = nano::topo_key{ floor_height, nano::block_hash{ 0 } };
		head.reset_union ();
	}

	// Same quorum eligibility as the spear (the strategy steers each request to a distinct
	// peer): a repair head bursts up to repair_consideration_count requests per page, then
	// waits out the cooldown. A response resets `timestamp` to {} via `reset_union` so the
	// next page fires at once; an unanswered cursor waits one cooldown before re-querying.
	if (!eligible (head, now))
	{
		return std::nullopt;
	}
	head.requests += 1;
	head.timestamp = now;
	return head.cursor;
}

bool topo_scan_index::process (std::size_t h, nano::topo_key const start, std::deque<nano::topo_key> const & entries)
{
	debug_assert (h < heads.size ());
	debug_assert (std::is_sorted (entries.begin (), entries.end ()));
	auto & head = heads[h];

	// Reject stale responses (cursor already moved off `start`).
	if (start != head.cursor)
	{
		return false;
	}

	head.completed += 1;

	// Accumulate the strictly-post-cursor entries.
	for (auto const & entry : entries)
	{
		if (entry > head.cursor)
		{
			head.candidates.insert (entry);
		}
	}

	// Every head unions a full quorum (one response per distinct peer) before finalizing —
	// the spear consideration_count peers, a repair head the smaller repair_consideration_count.
	if (head.completed < head_target (head))
	{
		return false;
	}

	if (head.candidates.empty ())
	{
		// Spear: a sustained empty quorum means the topology end. Because each round queries
		// `target` DISTINCT peers, target*2 empties is >=2 distinct rounds agreeing empty —
		// a single fast empty peer can no longer trip this. A repair head can also see an
		// empty quorum (lagging peers with nothing past the cursor); it makes no progress and
		// re-queries the same cursor on cooldown with fresh peers.
		if (!head.repair && head.completed >= head_target (head) * 2)
		{
			head.done = true;
			tip_reached = true;
			return true;
		}
		return false;
	}

	// Finalize: keep the lowest `candidates` of the union as a chunk, deduped against the
	// member set so a re-scan only adds previously-skipped keys.
	auto & by_hash = members.get<tag_hash> ();
	uint64_t const chunk_id = next_chunk_id;
	nano::topo_key last_inserted = head.cursor;
	std::size_t kept = 0, added = 0;
	nano::topo_key lo{}, hi{};
	for (auto const & candidate : head.candidates)
	{
		if (kept >= config.candidates)
		{
			break;
		}
		last_inserted = candidate;
		++kept;

		if (by_hash.find (candidate.hash) != by_hash.end ())
		{
			continue; // Already tracked — dedup.
		}

		member m{};
		m.hash = candidate.hash;
		m.key = candidate;
		m.state = member_state::pending;
		m.chunk_id = chunk_id;
		members.insert (m);
		++pending_count;
		if (added == 0)
		{
			lo = candidate;
		}
		hi = candidate;
		++added;
	}

	if (added > 0)
	{
		chunk c{};
		c.id = chunk_id;
		c.head = h;
		c.lo = lo;
		c.hi = hi;
		c.total = added;
		chunks.emplace (chunk_id, c);
		++next_chunk_id;
	}

	// Advance the head's cursor to the last (highest) kept key.
	debug_assert (head.cursor < last_inserted);
	head.cursor = last_inserted;
	head.reset_union ();
	return true;
}

std::deque<nano::block_hash> topo_scan_index::next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now)
{
	std::deque<nano::block_hash> result;

	if (in_flight_count >= config.max_blocks_outstanding)
	{
		return result; // In-flight cap.
	}

	auto const retry_cutoff = now - config.block_retry;
	auto & by_key = members.get<tag_key> ();
	for (auto it = by_key.begin (); it != by_key.end () && result.size () < max_count; ++it)
	{
		bool const want = it->state == member_state::pending
		|| (it->state == member_state::in_flight && it->timestamp < retry_cutoff);
		if (want)
		{
			result.push_back (it->hash);
			set_state (*it, member_state::in_flight);
			by_key.modify (it, [now] (member & m) {
				m.state = member_state::in_flight;
				m.timestamp = now;
			});
		}
	}
	return result;
}

void topo_scan_index::block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block)
{
	auto & by_hash = members.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return;
	}
	// A tardy delivery must not backtrack a block past `received`.
	if (it->state != member_state::pending && it->state != member_state::in_flight)
	{
		return;
	}
	set_state (*it, member_state::received);
	by_hash.modify (it, [&block] (member & m) {
		m.state = member_state::received;
		m.block = block;
	});
}

std::deque<std::shared_ptr<nano::block>> topo_scan_index::next_submit (std::size_t max_count, std::chrono::steady_clock::time_point now)
{
	std::deque<std::shared_ptr<nano::block>> result;

	// Submit in topo_key order (deps before dependents) over the contiguous front:
	// submit `received` members and re-submit `gapped` members past their retry
	// interval (their dep may now be filled by a repair head); skip blocks already
	// in the processor pipeline (`submitted`) or confirmed (`in_ledger`/`terminal`);
	// STOP at the first un-fetched member (`pending`/`in_flight`) — submitting past
	// an un-fetched hole would only gap its dependents. A `gapped` member STOPS the
	// walk too while the gap-pause latch is set (`repair_wait`), so no dependents are
	// submitted past it and the gap set can only shrink; below the threshold the walk
	// continues past gaps to make progress on independent chains (bounded by it).
	bool const paused = repair_wait;
	auto const retry_cutoff = now - config.block_retry;
	auto & by_key = members.get<tag_key> ();
	auto const submit = [&] (auto it) {
		result.push_back (it->block);
		set_state (*it, member_state::submitted);
		by_key.modify (it, [now] (member & m) {
			m.state = member_state::submitted;
			m.timestamp = now;
		});
	};
	for (auto it = by_key.begin (); it != by_key.end () && result.size () < max_count; ++it)
	{
		if (it->state == member_state::pending || it->state == member_state::in_flight)
		{
			break;
		}
		if (it->state == member_state::gapped)
		{
			if (it->timestamp < retry_cutoff)
			{
				submit (it);
			}
			if (paused)
			{
				break; // Threshold reached: do not submit dependents past the gap.
			}
			continue;
		}
		if (it->state == member_state::received)
		{
			submit (it);
		}
	}
	return result;
}

void topo_scan_index::resolve_member (nano::block_hash const & hash)
{
	auto & by_hash = members.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return;
	}
	if (it->state == member_state::in_ledger || it->state == member_state::terminal)
	{
		return; // Already resolved.
	}
	auto const cid = it->chunk_id;
	set_state (*it, member_state::in_ledger);
	by_hash.modify (it, [] (member & m) { m.state = member_state::in_ledger; });
	chunk_resolve (cid);
	advance_watermark ();
}

void topo_scan_index::block_indexed (nano::block_hash const & hash)
{
	resolve_member (hash);
}

void topo_scan_index::mark_redundant (nano::block_hash const & hash)
{
	resolve_member (hash);
}

void topo_scan_index::block_gapped (nano::block_hash const & hash, std::chrono::steady_clock::time_point now)
{
	auto & by_hash = members.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return;
	}
	// Only a submitted block can come back as a gap. Retain its block and schedule a
	// re-submit; once the gap count reaches the threshold the spear pauses for repair.
	if (it->state != member_state::submitted)
	{
		return;
	}
	set_state (*it, member_state::gapped); // bumps gapped_count / maybe sets repair_wait
	by_hash.modify (it, [now] (member & m) {
		m.state = member_state::gapped;
		m.timestamp = now;
	});
}

void topo_scan_index::block_terminal (nano::block_hash const & hash)
{
	auto & by_hash = members.get<tag_hash> ();
	auto it = by_hash.find (hash);
	if (it == by_hash.end ())
	{
		return;
	}
	auto const cid = it->chunk_id;
	set_state (*it, member_state::terminal);
	by_hash.modify (it, [] (member & m) { m.state = member_state::terminal; });
	chunk_resolve (cid);
	advance_watermark (); // a terminal block at the front is not a hole — advance past it
}

std::size_t topo_scan_index::cleanup (std::chrono::steady_clock::time_point now)
{
	auto const cutoff = now - config.block_retry;
	std::size_t reset_count = 0;
	auto & by_key = members.get<tag_key> ();
	for (auto it = by_key.begin (); it != by_key.end (); ++it)
	{
		if (it->state == member_state::in_flight && it->timestamp < cutoff)
		{
			set_state (*it, member_state::pending);
			by_key.modify (it, [] (member & m) {
				m.state = member_state::pending;
				m.timestamp = {};
			});
			++reset_count;
		}
	}
	return reset_count;
}

void topo_scan_index::reset ()
{
	members.clear ();
	chunks.clear ();
	next_chunk_id = 0;
	confirmed_watermark = {};
	tip_reached = false;
	repair_wait = false;
	pending_count = in_flight_count = received_count = submitted_count = in_ledger_count = gapped_count = 0;

	std::size_t const count = std::max<std::size_t> (1, config.head_count);
	heads.assign (count, scan_head{});
	for (std::size_t i = 0; i < count; ++i)
	{
		heads[i].repair = (i != 0); // head 0 == spear
	}
}

void topo_scan_index::seed (nano::topo_key const & start)
{
	// Position the spear above the already-synced prefix and report the watermark there.
	// Repair head ceilings are snapshotted from the spear frontier, so they derive from this
	// seeded position too. Intended for the fresh state right after reset(); guard against a
	// non-empty member set just in case.
	debug_assert (members.empty ());
	heads[0].cursor = start;
	confirmed_watermark = start;
}

void topo_scan_index::repoll ()
{
	// Poll mode: re-arm the spear to re-page from its cursor and detect new
	// blocks appended to the index since the tip was reached.
	heads[0].done = false;
	tip_reached = false;
	heads[0].reset_union ();
}

void topo_scan_index::set_state (member const & m, member_state ns)
{
	switch (m.state)
	{
		case member_state::pending:
			--pending_count;
			break;
		case member_state::in_flight:
			--in_flight_count;
			break;
		case member_state::received:
			--received_count;
			break;
		case member_state::submitted:
			--submitted_count;
			break;
		case member_state::in_ledger:
			--in_ledger_count;
			break;
		case member_state::gapped:
			--gapped_count;
			break;
		case member_state::terminal:
			break;
	}
	switch (ns)
	{
		case member_state::pending:
			++pending_count;
			break;
		case member_state::in_flight:
			++in_flight_count;
			break;
		case member_state::received:
			++received_count;
			break;
		case member_state::submitted:
			++submitted_count;
			break;
		case member_state::in_ledger:
			++in_ledger_count;
			break;
		case member_state::gapped:
			++gapped_count;
			break;
		case member_state::terminal:
			break;
	}

	// Maintain the spear gap-pause latch (hysteresis): pause once gaps reach the
	// threshold, resume only when every gap has cleared.
	if (gapped_count >= effective_gap_threshold ())
	{
		repair_wait = true;
	}
	else if (gapped_count == 0)
	{
		repair_wait = false;
	}
}

void topo_scan_index::advance_watermark ()
{
	auto & by_key = members.get<tag_key> ();
	for (auto it = by_key.begin (); it != by_key.end ();)
	{
		if (it->state == member_state::in_ledger || it->state == member_state::terminal)
		{
			// Confirmed: advance the watermark and release it. Monotonic — a repair head
			// re-finding a dependency BELOW the (false-advanced) watermark prunes it here,
			// but that must not drag the reported position backward.
			confirmed_watermark = std::max (confirmed_watermark, it->key);
			if (it->state == member_state::in_ledger)
			{
				--in_ledger_count;
			}
			it = by_key.erase (it);
		}
		else if (it->key <= confirmed_watermark)
		{
			// An unresolved member (gapped) in already-confirmed territory — a repair head
			// re-found a hole the watermark already passed. Keep it for resolution, but do NOT
			// let it wedge the prune of confirmed members above it: otherwise the confirmed
			// backlog piles up against max_blocks_queued and stalls the spear well below
			// gap_threshold (the gap latch's job).
			++it;
		}
		else
		{
			// First unresolved member at/above the frontier: stop the contiguous advance. A
			// real frontier gap holds the watermark here (and, once they accumulate to
			// gap_threshold, pauses the spear via repair_wait).
			break;
		}
	}
}

void topo_scan_index::chunk_resolve (uint64_t chunk_id)
{
	auto it = chunks.find (chunk_id);
	if (it == chunks.end ())
	{
		return;
	}
	++it->second.resolved;
	if (it->second.done ())
	{
		chunks.erase (it);
	}
}

std::size_t topo_scan_index::effective_gap_threshold () const
{
	return std::max<std::size_t> (1, config.gap_threshold);
}

std::size_t topo_scan_index::head_consideration (scan_head const & head) const
{
	// The spear must be robust against a fast un-synced peer, so it unions more peers; a
	// repair head needs less per-page redundancy (coverage also builds across wrap-arounds).
	return head.repair ? config.repair_consideration_count : config.consideration_count;
}

std::size_t topo_scan_index::head_target (scan_head const & head) const
{
	// The frozen adaptive size, or head_consideration when the strategy hasn't frozen one
	// (the latter preserves behavior when the index is driven directly, e.g. in unit tests).
	return head.peers.target != 0 ? head.peers.target : head_consideration (head);
}

void topo_scan_index::freeze_target (std::size_t h, std::size_t capable_peers)
{
	debug_assert (h < heads.size ());
	auto & head = heads[h];
	head.peers.freeze (head_consideration (head), capable_peers);
}

void topo_scan_index::record_query (std::size_t h, nano::node_id peer)
{
	debug_assert (h < heads.size ());
	heads[h].peers.add (peer);
}

void topo_scan_index::new_round (std::size_t h)
{
	debug_assert (h < heads.size ());
	heads[h].peers.new_round ();
}

void topo_scan_index::cap_target (std::size_t h)
{
	debug_assert (h < heads.size ());
	heads[h].peers.cap_to_seen ();
}

bool topo_scan_index::round_full (std::size_t h) const
{
	debug_assert (h < heads.size ());
	auto const & head = heads[h];
	return head.peers.size () >= head_target (head);
}

std::vector<nano::node_id> topo_scan_index::seen_peers (std::size_t h) const
{
	debug_assert (h < heads.size ());
	return heads[h].peers.to_vector ();
}

std::size_t topo_scan_index::outstanding () const
{
	// Active discovery window: members still being fetched/processed, or gapped and awaiting
	// retry. Deliberately EXCLUDES in_ledger/terminal members — they are done and only awaiting
	// prune, so a few gaps wedging the prune can't inflate this and stall the spear. Gap-driven
	// pausing is solely the repair_wait latch's job (engaged at gap_threshold).
	return pending_count + in_flight_count + received_count + submitted_count + gapped_count;
}

bool topo_scan_index::indexing () const
{
	return !heads[0].done || !members.empty ();
}

bool topo_scan_index::caught_up () const
{
	return tip_reached && members.empty ();
}

nano::topo_key topo_scan_index::cursor () const
{
	return confirmed_watermark;
}

std::size_t topo_scan_index::count_members () const
{
	return members.size ();
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
std::size_t topo_scan_index::count_in_ledger () const
{
	return in_ledger_count;
}
std::size_t topo_scan_index::count_gapped () const
{
	return gapped_count;
}
std::size_t topo_scan_index::count_chunks () const
{
	return chunks.size ();
}

nano::container_info topo_scan_index::container_info () const
{
	auto collect_heads = [&] () {
		nano::container_info info;
		info.put ("watermark", confirmed_watermark.topo_height);
		for (std::size_t i = 0; i < heads.size (); ++i)
		{
			info.put (std::to_string (i), heads[i].cursor.topo_height);
		}
		return info;
	};

	nano::container_info info;
	info.put ("members", members.size ());
	info.put ("pending", pending_count);
	info.put ("in_flight", in_flight_count);
	info.put ("received", received_count);
	info.put ("submitted", submitted_count);
	info.put ("in_ledger", in_ledger_count);
	info.put ("gapped", gapped_count);
	info.put ("chunks", chunks.size ());
	info.put ("tip_reached", tip_reached ? std::size_t{ 1 } : std::size_t{ 0 });
	info.put ("repair_wait", repair_wait ? std::size_t{ 1 } : std::size_t{ 0 });
	info.add ("heads", collect_heads ());
	return info;
}
}
