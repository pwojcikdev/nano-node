#include <nano/lib/container_info.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/topo_scan.hpp>

namespace nano::bootstrap
{
topo_scan::topo_scan (topo_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
	reset ();
}

void topo_scan::reset ()
{
	heads.clear ();
	inflight.clear ();
	frontier = {};

	// Head 0 is the spearhead (anchored to our local tip by orient); heads 1..N are repair heads
	heads.insert (head{ .type = head_type::spearhead, .id = 0 });
	for (unsigned i = 0; i < config.repair_heads; ++i)
	{
		heads.insert (head{ .type = head_type::repair, .id = i + 1 });
	}
}

void topo_scan::orient (nano::topo_key latest)
{
	frontier = std::max (frontier, latest);

	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (0); // The spearhead
	if (it != by_id.end ())
	{
		by_id.modify (it, [latest] (head & h) { h.cursor = latest; });
	}
}

topo_scan::band topo_scan::repair_band (unsigned index) const
{
	// Divide [1, frontier] into config.repair_heads equal bands
	auto const n = config.repair_heads > 0 ? config.repair_heads : 1;
	uint64_t lo_height = 1 + (static_cast<uint64_t> (index) * frontier.topo_height) / n;
	uint64_t hi_height = 1 + (static_cast<uint64_t> (index + 1) * frontier.topo_height) / n;
	return { nano::topo_key{ lo_height, 0 }, nano::topo_key{ hi_height, 0 } };
}

std::optional<topo_scan::request> topo_scan::next (bool include_spearhead, id_t id, std::chrono::steady_clock::time_point now)
{
	auto const cutoff = now - config.cooldown;

	// Heads are ordered by last-query time, so the first eligible one is always the one queried longest ago
	auto & by_timestamp = heads.get<tag_timestamp> ();
	for (auto it = by_timestamp.begin (); it != by_timestamp.end (); ++it)
	{
		if (it->is_spearhead () && !include_spearhead)
		{
			continue; // Spearhead paused by back-pressure
		}

		// The spearhead samples each position consideration_count times before advancing; a repair head only
		// once. A head is due while it still owes samples for the current cursor, or once its cooldown elapses.
		unsigned const consideration = it->is_spearhead () ? config.consideration_count : 1;
		if (it->requests < consideration || it->timestamp < cutoff)
		{
			stats.inc (nano::stat::type::bootstrap_topo_scan, it->is_spearhead () ? nano::stat::detail::next_spearhead : nano::stat::detail::next_repair);

			by_timestamp.modify (it, [this, now] (head & h) {
				// The single place a repair band is set: arm any unarmed head (its first sweep, or after it wrapped)
				if (!h.is_spearhead () && !h.armed ())
				{
					h.start_sweep (repair_band (h.id - 1));
				}
				h.requests += 1;
				h.timestamp = now;
			});

			inflight[id] = it->id;

			return request{ it->cursor, config.request_count };
		}
	}

	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
	return std::nullopt;
}

topo_scan::page topo_scan::process (id_t id, std::deque<nano::topo_key> const & entries)
{
	// Resolve which head this response belongs to; a missing id means it was cancelled or already handled
	auto inflight_it = inflight.find (id);
	if (inflight_it == inflight.end ())
	{
		return {};
	}
	unsigned const head_id = inflight_it->second;
	inflight.erase (inflight_it);

	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (head_id);
	if (it == by_id.end ())
	{
		return {};
	}

	std::deque<nano::topo_key> retire;
	by_id.modify (it, [this, &entries, &retire] (head & h) {
		h.processed += entries.size ();
		retire = h.is_spearhead () ? process_spearhead (h, entries) : process_repair (h, entries);
	});
	return { head_id, std::move (retire) };
}

std::deque<nano::topo_key> topo_scan::process_spearhead (head & h, std::deque<nano::topo_key> const & entries)
{
	h.completed += 1; // Count this reply toward the consideration_count needed before advancing

	// Aggregate this reply's new entries, capping the set to the smallest `candidates` so one aggressive peer
	// cannot make us advance past entries that lagging peers have not reported yet
	for (auto const & entry : entries)
	{
		if (h.cursor < entry)
		{
			h.candidates.insert (entry);
		}
	}
	while (!h.candidates.empty () && h.candidates.size () > config.candidates)
	{
		h.candidates.erase (std::prev (h.candidates.end ())); // Drop the largest, keep the smallest
	}

	// Keep sampling until enough peers have replied for the current cursor
	if (h.completed < config.consideration_count || h.candidates.empty ())
	{
		return {};
	}

	// Advance to the largest of the kept (smallest) candidates and retire them for fetching. The next request
	// re-returns this entry as its first (a cheap health marker); the `cursor < entry` filter above ignores it.
	release_assert (!h.candidates.empty ());
	auto const furthest = *h.candidates.rbegin ();
	frontier = std::max (frontier, furthest);
	h.cursor = furthest;

	std::deque<nano::topo_key> retire{ h.candidates.begin (), h.candidates.end () };

	// Reset and prime the head to immediately start sampling the new cursor
	h.candidates.clear ();
	h.requests = 0;
	h.completed = 0;
	h.timestamp = {};

	return retire;
}

std::deque<nano::topo_key> topo_scan::process_repair (head & h, std::deque<nano::topo_key> const & entries)
{
	// Nothing new (empty, or nothing past the cursor): keep the cursor and let the cooldown back off
	if (entries.empty () || !(h.cursor < entries.back ()))
	{
		return {};
	}

	// Advance within the band; reaching its end disarms the head so next () re-arms a fresh band
	if (entries.back () < h.range.hi)
	{
		h.cursor = entries.back ();
	}
	else
	{
		h.disarm ();
	}

	// Made progress: retire the spent request and clear the cooldown so the head re-fires promptly
	h.requests -= h.requests > 0 ? 1 : 0;
	h.timestamp = {};

	return { entries.begin (), entries.end () };
}

void topo_scan::cancel (id_t id)
{
	inflight.erase (id);
}

nano::container_info topo_scan::container_info () const
{
	auto collect_heads = [&] () {
		nano::container_info info;
		for (auto const & h : heads)
		{
			info.put (std::to_string (h.id), h.cursor.topo_height);
		}
		info.put ("frontier", frontier.topo_height);
		return info;
	};

	auto collect_processed = [&] () {
		nano::container_info info;
		for (auto const & h : heads)
		{
			info.put (std::to_string (h.id), h.processed);
		}
		return info;
	};

	nano::container_info info;
	info.put ("inflight", inflight.size ());
	info.add ("heads", collect_heads ());
	info.add ("processed", collect_processed ());
	return info;
}
}
