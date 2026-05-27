#pragma once

#include <nano/secure/common.hpp>

#include <cstdint>
#include <limits>
#include <optional>

namespace nano::bootstrap
{
/*
 * Startup orientation: a pure state machine that locates the topo-index WATERMARK — the
 * highest topo_key K such that our ledger already holds every block the network has at or
 * below K. Seeding the spear there lets a restarted bootstrap skip re-paging the whole
 * index from genesis (which only re-discovers already-synced blocks for hours).
 *
 * WHY PEERS, NOT THE LOCAL LEDGER
 * -------------------------------
 * The topo-height invariant (topo_height = 1 + max(dep heights), and a block enters the
 * ledger only once its dependencies have) makes our local index HEIGHT-contiguous: holding
 * any block at height H means holding at least one block at every height 1..H, all the way
 * up to any single out-of-range block another strategy fetched. So the local index is dense
 * in height up to those outliers and tells us nothing about WIDTH-completeness (do we have
 * all the blocks the network has at each height). Only a peer knows the width. Hence the
 * search probes peers and checks each returned key against our ledger.
 *
 * ALGORITHM
 * ---------
 * Exponential-then-binary search over probe HEIGHTS. Each probe asks a peer for its dense
 * page from {H,0} upward; the owner intersects that page with our ledger and reports:
 *   - "fully present" (every returned key is in our ledger) -> the watermark is at least the
 *     page top: climb (double the probe height while still climbing, else move the binary
 *     search floor up).
 *   - "first missing key at height M" / "empty page" (above the network top) -> the
 *     watermark is below M: lower the binary search ceiling.
 * The climb's first bounded probe brackets the watermark; binary search then narrows it, and
 * a probe whose page contains both present and missing keys pins it down exactly.
 *
 * MONOTONICITY / SAFETY NET
 * -------------------------
 * Binary search assumes the "fully present" predicate is monotone in height: complete below
 * the watermark, incomplete above it. A bottom-up bootstrap satisfies this — it confirms a
 * contiguous prefix and the only incompleteness is at/above the interrupted frontier. A rare
 * interior hole that violates it can make the search seed slightly high; the spear then gaps
 * above the hole and repair head 1 (floor 0) re-scans from genesis and refills it. So
 * orientation is a heuristic to skip the bulk re-scan, with the existing repair/gap machinery
 * as the correctness backstop — never a source of permanently-missed blocks.
 */
class topo_orient
{
public:
	// `base_height` is the first probe height of the exponential climb.
	explicit topo_orient (uint64_t base_height);

	// The next probe height to query, or nullopt once the watermark is finalized.
	// Pure: re-querying without an intervening observe() returns the same height (so a
	// failed probe is simply retried).
	std::optional<uint64_t> next () const;

	// Report the outcome of probing height `probed`:
	//   present_through — the highest key whose entire page-prefix (it and every lower key
	//                     the peers returned) is in our ledger; nullopt if the lowest
	//                     returned key is missing, or the peers returned nothing.
	//   bounded         — true if this probe proved the watermark is strictly below a
	//                     height (a missing key was found, or the page was empty).
	//   bound_height    — that height (ignored unless `bounded`).
	void observe (uint64_t probed, std::optional<nano::topo_key> present_through, bool bounded, uint64_t bound_height);

	// True once the search has converged (next() returns nullopt).
	bool done () const;

	// Best confirmed watermark: the highest contiguously-present key observed. Default
	// topo_key{} when nothing below the first probe is present (e.g. a fresh ledger).
	nano::topo_key watermark () const;

private:
	// Sentinel for "no upper bound yet" (still climbing).
	static constexpr uint64_t unknown = std::numeric_limits<uint64_t>::max ();
	// Cap on the climb height so doubling can't overflow or collide with `unknown`.
	static constexpr uint64_t climb_cap = uint64_t{ 1 } << 60;

	// Highest contiguously-present key seen so far (the search floor / result).
	nano::topo_key present{};
	// Lowest height proven incomplete (the search ceiling); `unknown` while still climbing.
	uint64_t hi{ unknown };
	// Next exponential probe height (climbing phase only).
	uint64_t climb{ 0 };
};
}
