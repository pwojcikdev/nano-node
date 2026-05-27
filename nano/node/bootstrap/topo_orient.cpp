#include <nano/node/bootstrap/topo_orient.hpp>

#include <algorithm>

namespace nano::bootstrap
{
topo_orient::topo_orient (uint64_t base_height) :
	climb{ std::clamp<uint64_t> (base_height, 1, climb_cap) }
{
}

bool topo_orient::done () const
{
	// Converged once the ceiling is within one height of the confirmed floor: the watermark
	// is pinned to `present`. Never done while still climbing (no ceiling yet).
	return hi != unknown && hi <= present.topo_height + 1;
}

std::optional<uint64_t> topo_orient::next () const
{
	if (done ())
	{
		return std::nullopt;
	}
	if (hi == unknown)
	{
		return climb; // Exponential climb until the first bounded probe sets a ceiling.
	}
	// Binary search strictly between the confirmed floor and the ceiling. done() guarantees
	// hi - lo >= 2 here, so mid lands in (lo, hi).
	uint64_t const lo = present.topo_height;
	return lo + (hi - lo) / 2;
}

void topo_orient::observe (uint64_t /* probed */, std::optional<nano::topo_key> present_through, bool bounded, uint64_t bound_height)
{
	// Raise the floor (monotonic): a probe can only confirm more, never less.
	if (present_through && *present_through > present)
	{
		present = *present_through;
	}
	if (bounded)
	{
		// Lower the ceiling. `unknown` is max, so std::min adopts the first real bound and
		// keeps the tightest one thereafter.
		hi = std::min (hi, bound_height);
	}
	else if (hi == unknown)
	{
		// Fully present and still climbing: double the probe height, but never below the
		// confirmed floor so the next probe always makes progress. Capped to avoid overflow.
		uint64_t const doubled = (climb >= climb_cap) ? climb_cap : climb * 2;
		climb = std::clamp<uint64_t> (std::max (doubled, present.topo_height + 1), 1, climb_cap);
	}
	// Fully present while searching (hi known) needs no extra work: present_through already
	// advanced the floor above the probed mid, shrinking the interval.
}

nano::topo_key topo_orient::watermark () const
{
	return present;
}
}
