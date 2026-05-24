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
		// Backpressure: pause discovery while the held window is full. Repair
		// heads are exempt so they can always fill the hole that's blocking it.
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

	// Repair head h chases the (h-1)-th lowest gap.
	auto const g = nth_gap_height (h - 1);
	if (!g)
	{
		head.target = {}; // No gap for this head — idle.
		return std::nullopt;
	}

	auto const back_off = [] (uint64_t height, uint64_t distance) {
		return nano::topo_key{ height > distance ? height - distance : 0, nano::block_hash{ 0 } };
	};

	if (head.target.topo_height != *g)
	{
		// New target gap — home below it.
		head.target = nano::topo_key{ *g, nano::block_hash{ 0 } };
		head.rollback = config.rollback_min;
		head.cursor = back_off (*g, head.rollback);
		head.reset_union ();
	}
	else if (head.cursor.topo_height >= *g)
	{
		// Reached the gap height but it's still gapped — the skipped key is
		// deeper (or was missed again). Walk further back and re-scan.
		head.rollback = std::min (head.rollback * 2, config.rollback_max);
		head.cursor = back_off (*g, head.rollback);
		head.reset_union ();
	}

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

	// Reject stale responses (cursor already advanced past `start`).
	if (start != head.cursor)
	{
		return false;
	}

	head.completed += 1;

	// Accumulate the union of strictly-post-cursor entries.
	for (auto const & entry : entries)
	{
		if (entry > head.cursor)
		{
			head.candidates.insert (entry);
		}
	}

	// Wait for the full quorum before finalizing.
	if (head.completed < config.consideration_count)
	{
		return false;
	}

	if (head.candidates.empty ())
	{
		// Spear: a sustained empty quorum means the topology end. Repair heads
		// scan below the frontier where entries always exist, so they don't reach
		// this (a transient empty response just re-queries on cooldown).
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

	// Advance the head's cursor to the last kept key.
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

	// Submit in topo_key order (deps before dependents) over the contiguous
	// front: submit `received` members and re-submit `gapped` members past their
	// retry interval (their dep may now be filled); skip blocks already in the
	// processor pipeline (`submitted`) or confirmed (`in_ledger`/`terminal`);
	// STOP at the first un-fetched member (`pending`/`in_flight`) — submitting
	// past an un-fetched hole would only gap its dependents.
	auto const retry_cutoff = now - config.block_retry;
	auto & by_key = members.get<tag_key> ();
	for (auto it = by_key.begin (); it != by_key.end () && result.size () < max_count; ++it)
	{
		if (it->state == member_state::pending || it->state == member_state::in_flight)
		{
			break;
		}
		bool const submit = it->state == member_state::received
		|| (it->state == member_state::gapped && it->timestamp < retry_cutoff);
		if (submit)
		{
			result.push_back (it->block);
			set_state (*it, member_state::submitted);
			by_key.modify (it, [now] (member & m) {
				m.state = member_state::submitted;
				m.timestamp = now;
			});
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
	// Only a submitted block can come back as a gap. Retain its block and
	// schedule a re-submit; its (lowest) height homes a repair head's rollback.
	if (it->state != member_state::submitted)
	{
		return;
	}
	set_state (*it, member_state::gapped); // inserts into gapped_keys
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
	gapped_keys.clear ();
	next_chunk_id = 0;
	confirmed_watermark = {};
	tip_reached = false;
	pending_count = in_flight_count = received_count = submitted_count = in_ledger_count = gapped_count = 0;

	std::size_t const count = std::max<std::size_t> (1, config.head_count);
	heads.assign (count, scan_head{});
	for (std::size_t i = 0; i < count; ++i)
	{
		heads[i].repair = (i != 0); // head 0 == spear
		heads[i].rollback = config.rollback_min;
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
			gapped_keys.erase (m.key);
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
			gapped_keys.insert (m.key);
			break;
		case member_state::terminal:
			break;
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

std::optional<uint64_t> topo_scan_index::nth_gap_height (std::size_t n) const
{
	if (gapped_keys.size () <= n)
	{
		return std::nullopt;
	}
	auto it = gapped_keys.begin ();
	std::advance (it, n);
	return it->topo_height;
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
	info.add ("heads", collect_heads ());
	return info;
}
}
