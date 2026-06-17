#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * Topo scan engine: walks the peers' topology index across multiple heads.
 *
 * One spearhead head advances the discovery frontier forward into unseen topo-height space; to avoid
 * over-advancing on a single (possibly lagging) peer it aggregates `consideration_count` replies for the
 * same position, keeps the smallest `candidates`, and advances past them. The repair heads each own a band
 * of the discovered range [1, frontier] and advance on a single reply, continuously re-scanning for gaps.
 *
 * Heads are ordered by last-query time so the scan loop always picks up the one queried longest ago.
 * Pure state, guarded externally by the bootstrap_context mutex (mirrors frontier_scan_index).
 */
class topo_scan
{
public:
	topo_scan (topo_scan_config const &, nano::stats &);

	// A request to issue: walk the peer's topo index from `start`, up to `count` entries
	struct request
	{
		nano::topo_key start;
		std::size_t count;
	};

	// A retired page handed back for pre-check, tagged with the head that produced it (0 is the spearhead)
	struct page
	{
		unsigned head{ 0 };
		std::deque<nano::topo_key> entries;
	};

	// Anchor the spearhead and frontier to our local topology tip
	void orient (nano::topo_key latest);

	// Reserve the oldest due head for request `id` and return the request to send, or nullopt if none is due.
	// `include_spearhead` gates only the spearhead (back-pressure); repair heads are always eligible.
	std::optional<request> next (bool include_spearhead, id_t id, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Apply a response page to the head that issued `id`. Returns the page to retire for pre-check (the head plus
	// the repair head's whole page, or the spearhead's aggregated set once it has advanced; empty otherwise).
	page process (id_t id, std::deque<nano::topo_key> const & entries);

	// Drop a reservation whose request failed or timed out (the head cools down and retries)
	void cancel (id_t id);

	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	topo_scan_config const & config;
	nano::stats & stats;

private:
	enum class head_type
	{
		spearhead, // Advances the discovery frontier forward into unseen topo-height space
		repair, // Re-scans a band of the discovered range to fill gaps
	};

	// A half-open range [lo, hi) of topo positions
	struct band
	{
		nano::topo_key lo;
		nano::topo_key hi;
	};

	struct head
	{
		head_type type{ head_type::repair };
		unsigned id{ 0 }; // Stable identity: 0 is the spearhead, 1..N are the repair heads
		nano::topo_key cursor{}; // Start position for this head's next request
		band range{}; // Repair only: the band frozen for the current sweep; hi is zero until first frozen
		std::set<nano::topo_key> candidates; // Spearhead only: entries aggregated across replies
		unsigned requests{ 0 }; // Requests issued for the current cursor
		unsigned completed{ 0 }; // Responses received for the current cursor
		std::chrono::steady_clock::time_point timestamp{}; // Last time this head was queried
		std::size_t processed{ 0 };

		bool is_spearhead () const
		{
			return type == head_type::spearhead;
		}

		// Begin a fresh sweep over `b`: adopt it as the frozen band and rewind the cursor to its start
		void start_sweep (band const & b)
		{
			range = b;
			cursor = b.lo;
		}

		// A repair head is armed once it holds a band to sweep, and disarmed when that band is exhausted
		bool armed () const
		{
			return range.hi != nano::topo_key{};
		}

		// Mark the band exhausted; next () will re-arm the head with a fresh band
		void disarm ()
		{
			range = {};
		}
	};

	// clang-format off
	class tag_id {};
	class tag_timestamp {};

	using ordered_heads = boost::multi_index_container<head,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_id>,
			mi::member<head, unsigned, &head::id>>,
		mi::ordered_non_unique<mi::tag<tag_timestamp>,
			mi::member<head, std::chrono::steady_clock::time_point, &head::timestamp>>
	>>;
	// clang-format on

	// Aggregate a reply into the spearhead and, once enough have arrived, advance past the smallest candidates
	std::deque<nano::topo_key> process_spearhead (head &, std::deque<nano::topo_key> const & entries);
	// Advance a repair head on a single reply, wrapping at the end of its band
	std::deque<nano::topo_key> process_repair (head &, std::deque<nano::topo_key> const & entries);

	// The band owned by repair head `index`, computed from the current frontier
	band repair_band (unsigned index) const;

	ordered_heads heads;
	nano::topo_key frontier{}; // Highest discovered topo position
	std::unordered_map<id_t, unsigned> inflight; // request id -> head id
};
}
