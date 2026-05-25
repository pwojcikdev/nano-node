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
	// Capacity to gather more of the quorum, or the cooldown since the last
	// request at this cursor has elapsed.
	return head.requests < config.consideration_count || head.timestamp < cutoff;
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
		// clear every gap (the latch resets at zero) before extending the frontier.
		if (repair_wait)
		{
			return std::nullopt;
		}
		// Backpressure: pause discovery while the held window is full.
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

	// Repair head: continuous sweep of [0, bound], wrapping at the bound. Quiesce
	// once the spear reached the tip and no gap remains, so re-discovery of already-
	// synced keys stops and `members` can drain to empty (else `caught_up` could
	// never be reached). A paused spear (gapped_count > 0) leaves repair active.
	if (heads[0].done && gapped_count == 0)
	{
		return std::nullopt;
	}

	// Upper bound for this head's sweep. Head 1 sweeps the full range up to the spear
	// — it MUST, as the guarantor that every skipped key below the frontier gets
	// re-scanned (halving it would leave the upper half unrepaired and could stall the
	// spear). Each subsequent head sweeps up to HALF the head ahead's height (head 2
	// up to head1/2, head 3 up to head2/2, …), concentrating the lower heads
	// geometrically on the low region where dependencies live.
	uint64_t bound_height = heads[h - 1].cursor.topo_height;
	if (h >= 2)
	{
		bound_height /= 2;
	}
	if (bound_height == 0)
	{
		// Nothing to sweep yet: the head ahead is at genesis (startup / just wrapped),
		// or so low that half its height rounds to zero. Wait for it to advance, then
		// sweep in behind it — a brief wait, not a stall.
		return std::nullopt;
	}

	if (head.cursor.topo_height >= bound_height)
	{
		// Reached the bound: wrap back to genesis and sweep the range again.
		head.cursor = nano::topo_key{};
		head.reset_union ();
		head.timestamp = now; // Throttle the restart by one cooldown (anti-spin).
	}

	// Repair eligibility is cooldown-since-last-fire only (single-response pages, no
	// consideration_count burst): a response resets `timestamp` to {} via
	// `reset_union` so the next page fires at once, while an unanswered cursor or a
	// fresh wrap waits out one cooldown.
	if (head.timestamp >= now - config.cooldown)
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

	// Reject stale responses (cursor already advanced past `start`).
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

	// The spear unions a full quorum before finalizing; a repair head finalizes on
	// the first response (coverage builds across wrap-arounds, not within a page).
	if (!head.repair && head.completed < config.consideration_count)
	{
		return false;
	}

	if (head.candidates.empty ())
	{
		// Spear: a sustained empty quorum means the topology end. A repair head can
		// also see an empty page (a lagging peer with nothing past the cursor) — it
		// just makes no progress and re-queries the same cursor on cooldown with a
		// fresh peer; it never concludes the tip.
		if (!head.repair && head.completed >= config.consideration_count * 2)
		{
			head.done = true;
			tip_reached = true;
			return true;
		}
		return false;
	}

	// Finalize: keep the lowest `candidates` of the union as a chunk, deduped
	// against the member set so a re-scan only adds previously-skipped keys.
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

	// Advance the head's cursor to the last kept key. (A repair page that returned
	// nothing past the cursor takes the empty branch above and never reaches here,
	// so the cursor always moves forward — but keep the assert spear-only for safety.)
	debug_assert (head.repair || head.cursor < last_inserted);
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
	while (!by_key.empty ())
	{
		auto it = by_key.begin ();
		if (it->state != member_state::in_ledger && it->state != member_state::terminal)
		{
			break;
		}
		// Monotonic: a repair head re-finding a dependency BELOW the (false-
		// advanced) watermark prunes it here, but that must not drag the reported
		// position backward — otherwise the watermark thrashes down to the dep's
		// height and climbs back on every deep repair.
		confirmed_watermark = std::max (confirmed_watermark, it->key);
		if (it->state == member_state::in_ledger)
		{
			--in_ledger_count;
		}
		by_key.erase (it);
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

std::size_t topo_scan_index::outstanding () const
{
	return members.size ();
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
