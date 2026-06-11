#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/secure/fwd.hpp>

#include <deque>
#include <optional>
#include <set>
#include <utility>

namespace nano::bootstrap
{
/*
 * Candidate voting for a single frontier scan round at a fixed position.
 * Pure value type: completed samples are fed in and once enough are gathered the round yields the
 * position the scan should advance to. The lifetime of an instance is the round itself.
 */
class frontier_round
{
public:
	frontier_round (nano::frontier_scan_config const &, nano::account position, nano::account range_end);

	// Merges one completed sample. Only accounts past the scan position become candidates; an empty
	// sample still counts towards the consensus that nothing exists past the position.
	void feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	// Enough samples gathered and at least one candidate to advance to
	bool done () const;
	// Persistently empty samples indicate that the range past the position holds no accounts
	bool empty_range () const;
	// The round is settled once either of the above holds; no further samples are needed
	bool settled () const;

	// The position the scan should advance to:
	// - the largest kept candidate when the gathered samples produced any
	// - the range end when the completed samples unanimously say nothing exists past the position
	// - nullopt when no sample completed; nothing was learned and the position should be retried
	std::optional<nano::account> conclude () const;

	size_t completed () const;
	size_t candidate_count () const;

private: // Dependencies
	nano::frontier_scan_config const & config;

private:
	nano::account const position;
	nano::account const range_end;

	std::set<nano::account> candidates;
	size_t completed_m{ 0 };
};

/*
 * Result of reconciling a batch of peer frontiers against the local ledger.
 * `prioritize` lists accounts that should enter the priority set: accounts whose advertised frontier
 * is unknown locally, and accounts missing locally that have pending entries in the ledger.
 */
struct frontier_classification
{
	std::deque<nano::account> prioritize;
	size_t outdated{ 0 };
	size_t pending{ 0 };
};

// Deterministic over the ledger snapshot. Frontiers must be sorted by account, ascending.
frontier_classification classify_frontiers (nano::ledger &, nano::secure::transaction const &, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);
}
