#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/round.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
class frontier_round final : public round
{
public:
	// Opens a voting round for the scan position within the owning head range
	frontier_round (nano::frontier_scan_config const &, nano::account position, nano::account range_end);

	// Records one frontier response and retains the nearest candidate accounts after position
	void feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	// True once enough useful samples have identified at least one candidate
	bool done () const;
	// True once enough empty samples found no candidates
	bool done_empty () const;
	// True when the round can be concluded, either with candidates or with no candidates
	bool settled () const;
	// Returns the selected next scan cursor, range end, or nullopt when no sample completed
	std::optional<nano::account> settle () const;

	// Account cursor this round samples from
	nano::account const & position () const;
	// Number of responses incorporated into this round
	size_t completed () const;
	// Number of retained candidate accounts
	size_t candidate_count () const;

private:
	nano::frontier_scan_config const & config; // Tuning values that bound samples and candidate retention
	nano::account const position_m; // Account cursor this round samples from
	nano::account const range_end_m; // End of the head range, used when samples find no frontier
	std::set<nano::account> candidates; // Smallest candidate accounts greater than position
	size_t completed_m{ 0 }; // Count of processed frontier responses
};

class frontier_scan_engine final
{
public:
	// Splits the account space into independently scanned heads
	frontier_scan_engine (nano::frontier_scan_config const &, nano::stats &);

	// Concludes any open rounds whose samples, peer availability, or caps make them settled
	void settle (peer_probes const &, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Returns the next round that can accept a launched sample
	std::shared_ptr<frontier_round> next_round (peer_probes const &, std::chrono::steady_clock::time_point now);

	// Moves the first-considered head forward after one acquire attempt
	void advance ();

	// Drops a sample by scan position when only the original request start is known
	void erase_sample (id_t tag_id, nano::account const & start);

	// Applies a frontier response to its round and returns false for stale or unknown samples
	bool process (id_t tag_id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	// Restarts all heads from their range starts and clears open samples
	void reset ();

	// Exposes scan progress and active sample counts for diagnostics
	nano::container_info container_info () const;

	// Hard cap multiplier for samples in one round before accepting a partial conclusion
	static size_t constexpr max_round_samples_factor = 4;

private:
	struct head_state
	{
		size_t const index; // Stable head index used for round-robin launch ordering
		nano::account const start; // Inclusive lower bound for this head range
		nano::account const end; // Upper bound used to wrap the head back to start
		nano::account cursor; // Next account position to scan within the head range
		std::shared_ptr<frontier_round> round; // Active round for the cursor, if samples are outstanding
		std::chrono::steady_clock::time_point pause_until{}; // Cooldown gate after partial conclusions
		size_t rounds{ 0 }; // Number of concluded rounds for diagnostics
		size_t processed{ 0 }; // Number of retained candidates advanced past for diagnostics

		// Initializes one range head with its cursor at the range start
		head_state (size_t index, nano::account start, nano::account end);
		// Clears active work and moves the cursor back to the range start
		void reset ();
	};

	// Applies a round result to its head cursor, stats, and cooldown state
	void conclude (head_state &, std::chrono::steady_clock::time_point now);
	// Finds the head whose range owns the supplied account position
	head_state & find_head (nano::account const & position);
	// Finds the head whose range owns the supplied account position
	head_state const & find_head (nano::account const & position) const;

	nano::frontier_scan_config const & config; // Tuning values for head count, sample count, and pacing
	nano::stats & stats; // Stat sink for scan outcomes and invalid sample references
	std::vector<head_state> heads; // Partitioned account ranges scanned in round-robin order
	size_t robin{ 0 }; // Next head index considered first when selecting launch work
};
}
