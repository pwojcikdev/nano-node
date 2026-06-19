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
	heads.insert (head{ .type = head_type::spearhead, .id = 0, .consideration = config.consideration_count });
	for (unsigned i = 0; i < config.repair_heads; ++i)
	{
		heads.insert (head{ .type = head_type::repair, .id = i + 1, .consideration = config.repair_consideration });
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
		// Back-pressure hard-pauses the spearhead (too many submitted blocks stuck as gaps); repair heads are exempt
		bool const gate = it->is_spearhead () ? include_spearhead : true;
		if (!gate)
		{
			continue;
		}

		// A head wants more samples while below its consideration count and not exhausted. It is also due once its
		// cooldown elapses, which both paces re-polling a finished cursor (tip idle / no new gaps) and retries a
		// round whose replies are not arriving.
		bool const want_more = !it->exhausted && it->requests < it->consideration;
		bool const cooldown_expired = it->timestamp < cutoff;
		bool const due = want_more || cooldown_expired;
		if (!due)
		{
			continue;
		}

		stats.inc (nano::stat::type::bootstrap_topo_scan, it->is_spearhead () ? nano::stat::detail::next_spearhead : nano::stat::detail::next_repair);

		unsigned fanout = 0;
		std::vector<nano::account> exclude;
		by_timestamp.modify (it, [this, now, want_more, &fanout, &exclude] (head & h) {
			// The single place a repair band is set: arm any unarmed head (its first sweep, or after it wrapped)
			if (!h.is_spearhead () && !h.armed ())
			{
				h.start_sweep (repair_band (h.id - 1));
			}
			// A wake that is purely the cooldown (round already full or exhausted) is a re-poll/retry: start fresh,
			// so the fan-out below is always consideration - requests with requests < consideration, i.e. >= 1.
			if (!want_more)
			{
				h.restart ();
			}
			h.timestamp = now;
			// Top up to the consideration count, excluding peers already sampled for this cursor (cross-round distinctness)
			fanout = h.consideration - h.requests;
			exclude.assign (h.sampled.begin (), h.sampled.end ());
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

	inflight[id] = reservation{ head, node_id, start };
	by_id.modify (it, [node_id] (auto & h) {
		if (h.sampled.insert (node_id).second)
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
	if (it == by_id.end () || it->cursor != start)
	{
		return {}; // gone, or advanced under us
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
		// Stale reply: the head advanced past where this request started, so it sampled a different position
		if (res.start != h.cursor)
		{
			return;
		}
		h.processed += entries.size ();
		h.completed += 1; // A distinct peer replied for the current cursor

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

		retire = maybe_advance (h);
	});
	return { res.head, std::move (retire) };
}

std::deque<nano::topo_key> topo_scan::maybe_advance (head & h)
{
	// Target distinct replies: the full consideration count, or fewer once the peer pool is exhausted
	unsigned const target = h.exhausted ? h.requests : h.consideration;

	if (h.requests > 0 && h.completed >= target)
	{
		if (h.candidates.empty ())
		{
			// Nothing new — tip idle (spearhead) or no gaps in the band right now (repair). Park the finished
			// round (keep `requests`, so the head stops being want_more) and let the cooldown pace the re-poll.
			return {};
		}

		// Advance to the largest of the kept (smallest) candidates and retire them for fetching. The next request
		// re-returns this entry as its first (a cheap health marker); the `cursor < entry` filter ignores it.
		auto const furthest = *h.candidates.rbegin ();
		if (h.is_spearhead ())
		{
			frontier = std::max (frontier, furthest); // Push the discovery frontier forward
			h.cursor = furthest;
		}
		else if (furthest < h.range.hi)
		{
			h.cursor = furthest; // Advance within the band
		}
		else
		{
			h.disarm (); // Reached the band end; next () re-arms a fresh band
		}
		std::deque<nano::topo_key> retire{ h.candidates.begin (), h.candidates.end () };
		h.restart (); // Begin a fresh round at the new cursor
		h.timestamp = {}; // Made progress: re-fire promptly (chase the frontier / sweep the band)
		return retire;
	}

	// Every sampled peer timed out without replying: clear the round so the head can retry from scratch
	if (h.requests == 0)
	{
		h.restart ();
	}
	return {};
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
		// Stale timeout: the head advanced past where this request started; it belongs to a finished round
		if (res.start != h.cursor)
		{
			return;
		}
		// The timed-out peer never contributed; drop it so a retry can sample a fresh one, then re-check
		// whether the (possibly lowered) round target is now met
		if (h.sampled.erase (res.node_id) > 0)
		{
			h.requests -= 1;
		}
		retire = maybe_advance (h);
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
