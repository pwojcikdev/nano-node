#pragma once

#include <nano/lib/numbers.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace nano::bootstrap
{
// Tracks the distinct peers (by node id) queried for one logical unit during a single
// redundancy round, plus the adaptive round size. Used to spread the redundant requests of
// a unit across different peers and to finalize the round on however many distinct peers
// are actually reachable.
//
// The seen-set is tiny (bounded by consideration_count, default 4), so it is a fixed inline
// array with linear search — no allocation, no tree. Not internally synchronized: callers
// hold the bootstrap context mutex (like the index classes that embed it).
struct distinct_peers
{
	// Linear-search ceiling and hard cap on the round size. Far above the default
	// consideration_count of 4; a consideration_count configured above this is capped here.
	static constexpr std::size_t capacity = 16;

	std::array<nano::node_id, capacity> seen{}; // node ids queried this round
	std::size_t count{ 0 };
	unsigned target{ 0 }; // frozen round size; 0 = not yet frozen

	bool contains (nano::node_id id) const
	{
		return std::find (seen.begin (), seen.begin () + count, id) != seen.begin () + count;
	}

	// Record a queried peer. Returns false if already present or the set is full.
	bool add (nano::node_id id)
	{
		if (contains (id) || count >= capacity)
		{
			return false;
		}
		seen[count++] = id;
		return true;
	}

	std::size_t size () const
	{
		return count;
	}

	// Freeze the round size to the smaller of the desired redundancy and the reachable peer
	// pool (clamped to [1, capacity]). No-op once frozen, so the size is stable for the round.
	void freeze (std::size_t consideration, std::size_t capable)
	{
		if (target == 0)
		{
			target = static_cast<unsigned> (std::clamp<std::size_t> (std::min ({ consideration, capable, capacity }), 1, capacity));
		}
	}

	// Cooldown sub-round reset: reopen the peer pool for a retry round, keeping `target`.
	void new_round ()
	{
		count = 0;
	}

	// Full reset when the unit advances.
	void clear ()
	{
		count = 0;
		target = 0;
	}
};
}
