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

topo_scan::band topo_scan::repair_band (head_index index) const
{
	// Divide [1, frontier] into config.repair_heads equal bands
	auto const n = config.repair_heads > 0 ? config.repair_heads : 1;
	uint64_t lo_height = 1 + (static_cast<uint64_t> (index) * frontier.topo_height) / n;
	uint64_t hi_height = 1 + (static_cast<uint64_t> (index + 1) * frontier.topo_height) / n;
	return { nano::topo_key{ lo_height, 0 }, nano::topo_key{ hi_height, 0 } };
}

std::optional<topo_scan::request> topo_scan::next (bool include_spearhead, std::chrono::steady_clock::time_point now)
{
	auto const cutoff = now - config.cooldown;

	// Heads are ordered by last-query time, so the first eligible one is always the one queried longest ago
	auto & by_timestamp = heads.get<tag_timestamp> ();
	for (auto it = by_timestamp.begin (); it != by_timestamp.end (); ++it)
	{
		bool due;
		if (it->is_spearhead ())
		{
			// Keep sampling distinct peers until consideration_count is reached or the pool is exhausted, paced by cooldown
			due = include_spearhead && !it->exhausted && it->sampled.size () < config.consideration_count && it->timestamp < cutoff;
		}
		else
		{
			// A repair head holds at most one in-flight request; re-fire once it concludes and the cooldown elapses
			due = it->requests == 0 && it->timestamp < cutoff;
		}
		if (!due)
		{
			continue;
		}

		stats.inc (nano::stat::type::bootstrap_topo_scan, it->is_spearhead () ? nano::stat::detail::next_spearhead : nano::stat::detail::next_repair);

		unsigned fanout = 1;
		std::vector<nano::account> exclude;
		by_timestamp.modify (it, [this, now, &fanout, &exclude] (head & h) {
			// The single place a repair band is set: arm any unarmed head (its first sweep, or after it wrapped)
			if (!h.is_spearhead () && !h.armed ())
			{
				h.start_sweep (repair_band (h.id - 1));
			}
			h.timestamp = now;
			if (h.is_spearhead ())
			{
				// Top up to consideration_count, excluding peers already sampled for this cursor (cross-round distinctness)
				fanout = config.consideration_count - static_cast<unsigned> (h.sampled.size ());
				exclude.assign (h.sampled.begin (), h.sampled.end ());
			}
		});

		return request{ it->id, it->cursor, config.request_count, fanout, std::move (exclude) };
	}

	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::next_none);
	return std::nullopt;
}

bool topo_scan::dispatch (head_index head, nano::topo_key start, id_t id, nano::account node_id)
{
	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (head);
	if (it == by_id.end () || it->cursor != start)
	{
		return false; // gone, or advanced under us since next () → stale round
	}

	inflight[id] = reservation{ head, node_id };
	by_id.modify (it, [node_id] (auto & h) {
		if (h.is_spearhead ())
		{
			h.sampled.insert (node_id);
		}
		else
		{
			h.requests += 1;
		}
	});
	return true;
}

topo_scan::page topo_scan::exhausted (head_index head, nano::topo_key start)
{
	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (head);
	if (it == by_id.end () || !it->is_spearhead () || it->cursor != start)
	{
		return {}; // gone, repair head, or advanced under us
	}

	std::deque<nano::topo_key> retire;
	by_id.modify (it, [this, &retire] (auto & h) {
		h.exhausted = true;
		// Lowering the bar to the peers we actually reached may complete the round right away
		retire = maybe_advance (h);
	});
	return { head, std::move (retire) };
}

topo_scan::page topo_scan::process (id_t id, std::deque<nano::topo_key> const & entries)
{
	// Resolve which head this response belongs to; a missing id means it was cancelled or already handled
	auto inflight_it = inflight.find (id);
	if (inflight_it == inflight.end ())
	{
		return {};
	}
	auto const res = inflight_it->second;
	inflight.erase (inflight_it);

	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (res.head);
	if (it == by_id.end ())
	{
		return {};
	}

	std::deque<nano::topo_key> retire;
	by_id.modify (it, [this, &res, &entries, &retire] (head & h) {
		h.processed += entries.size ();
		retire = h.is_spearhead () ? process_spearhead (h, res.node_id, entries) : process_repair (h, entries);
	});
	return { res.head, std::move (retire) };
}

std::deque<nano::topo_key> topo_scan::process_spearhead (head & h, nano::account node_id, std::deque<nano::topo_key> const & entries)
{
	// Ignore a straggler from a previous cursor whose round was already reset
	if (h.sampled.find (node_id) == h.sampled.end ())
	{
		return {};
	}
	h.completed += 1; // A distinct peer replied for this cursor

	// Aggregate this reply's new entries, capping the set to the smallest `candidates` so one aggressive peer
	// cannot make us advance past entries that lagging peers have not reported yet
	for (auto const & entry : entries)
	{
		if (h.cursor < entry)
		{
			h.candidates.insert (entry);
		}
	}
	while (h.candidates.size () > config.candidates)
	{
		h.candidates.erase (std::prev (h.candidates.end ())); // Drop the largest, keep the smallest
	}

	return maybe_advance (h);
}

std::deque<nano::topo_key> topo_scan::maybe_advance (head & h)
{
	auto const reset = [] (head & hd) {
		hd.candidates.clear ();
		hd.sampled.clear ();
		hd.completed = 0;
		hd.exhausted = false;
	};

	// Target distinct replies: the full consideration_count, or fewer once the peer pool is exhausted
	std::size_t const target = h.exhausted ? h.sampled.size () : config.consideration_count;

	if (!h.sampled.empty () && h.completed >= target)
	{
		std::deque<nano::topo_key> retire;
		if (!h.candidates.empty ())
		{
			// Advance to the largest of the kept (smallest) candidates and retire them for fetching. The next
			// request re-returns this entry as its first (a cheap health marker); the `cursor < entry` filter ignores it.
			auto const furthest = *h.candidates.rbegin ();
			frontier = std::max (frontier, furthest);
			h.cursor = furthest;
			retire = { h.candidates.begin (), h.candidates.end () };
			h.timestamp = {}; // Chase the frontier: due immediately for the new cursor
		}
		// else: nothing new at the tip — keep the cursor and let the cooldown pace the next poll
		reset (h);
		return retire;
	}

	// Every sampled peer timed out without replying: clear the round so the head can retry from scratch
	if (h.sampled.empty ())
	{
		reset (h);
	}
	return {};
}

std::deque<nano::topo_key> topo_scan::process_repair (head & h, std::deque<nano::topo_key> const & entries)
{
	h.requests -= h.requests > 0 ? 1 : 0; // The request concluded

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

	h.timestamp = {}; // Made progress: re-fire promptly for the next slice of the band
	return { entries.begin (), entries.end () };
}

topo_scan::page topo_scan::cancel (id_t id)
{
	auto inflight_it = inflight.find (id);
	if (inflight_it == inflight.end ())
	{
		return {};
	}
	auto const res = inflight_it->second;
	inflight.erase (inflight_it);

	auto & by_id = heads.get<tag_id> ();
	auto it = by_id.find (res.head);
	if (it == by_id.end ())
	{
		return {};
	}

	std::deque<nano::topo_key> retire;
	by_id.modify (it, [this, &res, &retire] (head & h) {
		if (h.is_spearhead ())
		{
			// The timed-out peer never contributed; drop it so a retry can sample a fresh one, then
			// re-check whether the (possibly lowered) round target is now met
			h.sampled.erase (res.node_id);
			retire = maybe_advance (h);
		}
		else
		{
			h.requests -= h.requests > 0 ? 1 : 0;
		}
	});
	return { res.head, std::move (retire) };
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
