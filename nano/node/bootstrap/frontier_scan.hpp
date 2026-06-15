#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/peer_pool.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
class frontier_round final
{
public:
	// Opens a voting round for the scan position within the owning head range
	frontier_round (nano::frontier_scan_config const &, nano::account position, nano::account range_end);

	// Records one frontier response and retains the nearest candidate accounts after position
	void feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	// True once enough useful samples have identified at least one candidate
	bool done () const;
	// True once repeated samples show no candidates in the remaining range
	bool empty_range () const;
	// True when the round can be concluded, either with candidates or as an empty range
	bool settled () const;
	// Returns the selected next scan cursor, range end, or nullopt when no sample completed
	std::optional<nano::account> conclude () const;

	// Number of responses incorporated into this round
	size_t completed () const;
	// Number of retained candidate accounts
	size_t candidate_count () const;

private:
	nano::frontier_scan_config const & config; // Tuning values that bound samples and candidate retention
	nano::account const position; // Account cursor this round samples from
	nano::account const range_end; // End of the head range, used when samples find no frontier
	std::set<nano::account> candidates; // Smallest candidate accounts greater than position
	size_t completed_m{ 0 }; // Count of processed frontier responses
};

class frontier_scan_engine final
{
public:
	struct probes
	{
		std::function<peer_probe_status (std::span<nano::account const> exclude)> peer_status; // Capacity check for a non-excluded peer
		std::function<size_t (std::span<id_t const> tag_ids)> count_inflight; // Counts still-active samples by request tag
	};

	struct launch_slot
	{
		size_t head_index; // Head to commit against if the caller launches this sample
		nano::account position; // Scan cursor to request frontiers from
		std::span<nano::account const> exclude; // Peers already sampled in the current round
	};

	// Splits the account space into independently scanned heads
	frontier_scan_engine (nano::frontier_scan_config const &, nano::stats &);

	// Concludes any open rounds whose samples, peer availability, or caps make them settled
	void settle (std::chrono::steady_clock::time_point now, probes const &);
	// Returns the next sample that can be launched without mutating engine state
	std::optional<launch_slot> peek_launch (std::chrono::steady_clock::time_point now, probes const &);
	// Commits a launched sample to its head and tracks its peer and tag until completion
	void commit (size_t head_index, nano::account const & position, nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now);
	// Drops a sample by head index when the owning request is cancelled or expires
	void erase_sample (size_t head_index, id_t tag_id);
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
	struct round_state
	{
		nano::account const position; // Cursor all samples in this round are querying
		frontier_round votes; // Aggregated frontier responses for this cursor
		std::vector<nano::account> used; // Peer node IDs already sampled, preserving launch order
		std::vector<id_t> tag_ids; // Request tags still allowed to update this round
		size_t launched{ 0 }; // Total samples launched, including ones already completed or erased
		std::chrono::steady_clock::time_point last_launch{}; // Last launch time for cooldown pacing

		// Creates the vote accumulator for one scan cursor
		round_state (nano::frontier_scan_config const &, nano::account position, nano::account range_end);
		// Returns true if the request tag still belongs to this round
		bool owns (id_t tag_id) const;
		// Removes a live request tag and reports whether it belonged to this round
		bool erase (id_t tag_id);
	};

	struct head_state
	{
		size_t const index; // Stable slot index used by callers to commit launch slots
		nano::account const start; // Inclusive lower bound for this head range
		nano::account const end; // Upper bound used to wrap the head back to start
		nano::account cursor; // Next account position to scan within the head range
		std::unique_ptr<round_state> round; // Active round for the cursor, if samples are outstanding
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
