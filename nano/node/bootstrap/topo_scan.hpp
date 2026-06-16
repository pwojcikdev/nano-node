#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/round.hpp>
#include <nano/secure/common.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
class topo_round final : public round
{
public:
	// Opens a voting round for topology keys after the supplied scan position
	topo_round (nano::topo_scan_config const &, nano::topo_key position, unsigned quorum);

	// Records one topology page response and retains the nearest candidate keys after position
	void feed (std::deque<nano::topo_key> const & entries);

	// True once enough useful samples have identified at least one candidate
	bool done () const;
	// True once enough empty samples found no candidates
	bool done_empty () const;
	// True when the round can be concluded, either with candidates or with no candidates
	bool settled () const;
	// Returns the selected next scan cursor, the original position, or nullopt when no sample completed
	std::optional<nano::topo_key> settle () const;
	// Retained candidate keys in ascending topology order
	std::deque<nano::topo_key> candidates () const;

	// Topology cursor this round samples from
	nano::topo_key const & position () const;
	// Number of responses incorporated into this round
	size_t completed () const;
	// Number of retained candidate keys
	size_t candidate_count () const;
	// Distinct-peer response target for this round
	unsigned quorum () const;

private:
	nano::topo_scan_config const & config; // Tuning values that bound samples and candidate retention
	nano::topo_key const position_m; // Topology cursor this round samples from
	unsigned const quorum_m; // Minimum completed samples before the round can settle
	std::set<nano::topo_key> candidates_m; // Smallest candidate keys greater than position
	size_t completed_m{ 0 }; // Count of processed topology page responses
};

class topo_scan_engine final
{
public:
	// Creates spear and repair heads for topology discovery
	topo_scan_engine (nano::topo_scan_config const &, nano::stats &);

	// Restarts discovery from a known topology key and clears in-flight state
	void orient (nano::topo_key const &);
	// Last contiguous topology key resolved by the scan
	nano::topo_key cursor () const;

	// Concludes any open topology rounds whose samples, peer availability, or caps make them settled
	void settle (peer_probes const &, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());
	// Returns the next round that can accept a launched topology page sample
	std::shared_ptr<topo_round> next_round (peer_probes const &, std::chrono::steady_clock::time_point now);
	// Moves the first-considered head forward after one acquire attempt
	void advance ();
	// Applies a topology page response to its round and returns false for stale or unknown samples
	bool process_page (id_t tag_id, nano::topo_key const & start, std::deque<nano::topo_key> const & entries);
	// Drops a topology page sample by request tag and start key
	void erase_page (id_t tag_id, nano::topo_key const & start);

	// True when a queued hash is due and fetch capacity is available
	bool fetch_ready (std::chrono::steady_clock::time_point now) const;
	// Returns due block hashes to fetch, preserving action order
	std::deque<nano::block_hash> next_fetch_candidates (std::chrono::steady_clock::time_point now, size_t max_count) const;
	// Marks requested hashes as in-flight and resolves hashes already present locally
	void commit_fetch (std::deque<nano::block_hash> const & requested, std::deque<nano::block_hash> const & already_local, id_t tag_id, std::chrono::steady_clock::time_point now);
	// Applies fetched blocks to queued members and retries missing hashes
	bool process_blocks (id_t tag_id, std::deque<std::shared_ptr<nano::block>> const & blocks, std::chrono::steady_clock::time_point now);
	// Abandons an in-flight fetch and schedules its hashes for retry
	void erase_fetch (id_t tag_id, std::chrono::steady_clock::time_point now);

	// True when a fetched block can be submitted to the block processor
	bool submit_ready (std::chrono::steady_clock::time_point now) const;
	// Returns a batch of fetched blocks, prioritizing contiguous frontier order
	std::deque<std::shared_ptr<nano::block>> next_submit_batch (std::chrono::steady_clock::time_point now, size_t max_count);
	// Applies block processor results and advances or repairs topology state
	bool inspect (nano::block_hash const &, nano::block_status, std::chrono::steady_clock::time_point now);

	// True when the spear is at the tip and no tracked topology work remains
	bool caught_up () const;
	// Number of discovered topology members still tracked
	size_t queued () const;
	// Number of active block fetch requests
	size_t fetch_inflight () const;
	// Number of frontier blocks waiting on dependency gaps
	size_t frontier_gap_count () const;

	// Clears state and restarts from the zero topology key
	void reset ();

	// Exposes scan progress, queues, and active work for diagnostics
	nano::container_info container_info () const;

	// Hard cap multiplier for samples in one round before accepting a partial conclusion
	static size_t constexpr max_round_samples_factor = 4;

private:
	// Lifecycle state for a discovered topology member
	enum class member_state
	{
		discovered, // Hash is known and ready to be fetched
		fetching, // Hash is included in an outstanding blocks_random request
		fetched, // Block contents are available locally
		submitted, // Block was handed to the block processor
		done, // Block is resolved and can advance the submit frontier
	};

	struct member
	{
		nano::topo_key key; // Topology key used for ordering and repair coverage
		nano::block_hash hash; // Block hash discovered in the topology index
		member_state state{ member_state::discovered }; // Current fetch/submit lifecycle state
		std::shared_ptr<nano::block> block; // Fetched block retained until submission or resolution
		bool frontier{ false }; // True when this member is ahead of the submit frontier
		bool frontier_gap{ false }; // True when a frontier block is delayed by a dependency gap
		bool fetch_queued{ false }; // True when present in fetch_queue
		bool submit_queued{ false }; // True when present in submit_queue
		std::chrono::steady_clock::time_point next_action{}; // Due time used by the active action queue
	};

	struct head_state
	{
		size_t const index; // Stable head index used for round-robin launch ordering
		nano::topo_key cursor{}; // Next topology position to scan for this head
		std::shared_ptr<topo_round> round; // Active round for the cursor, if samples are outstanding
		std::chrono::steady_clock::time_point pause_until{}; // Cooldown gate after empty or partial conclusions
		uint64_t floor_height{ 0 }; // Inclusive lower topology height for a repair head band
		uint64_t ceiling_height{ 0 }; // Exclusive upper topology height for a repair head band
		size_t empty_rounds{ 0 }; // Consecutive empty spear rounds used to detect the tip
		size_t rounds{ 0 }; // Number of concluded rounds for diagnostics
		size_t pages{ 0 }; // Number of non-empty pages processed by this head
		size_t processed{ 0 }; // Number of retained candidates advanced past for diagnostics

		// Initializes one scan head by stable index
		explicit head_state (size_t index);
		// True for the forward tip-tracking head
		bool spear () const;
		// Clears active work and sets the cursor from the supplied starting point
		void reset (nano::topo_key const & start);
	};

	struct fetch_state
	{
		std::deque<nano::block_hash> hashes; // Hashes committed to one outstanding fetch request
	};

	struct discover_result
	{
		std::optional<nano::topo_key> last; // Last key accepted or skipped during discovery
		bool blocked{ false }; // True when queue pressure stopped discovery before the page ended
	};

	// Queue key ordering actions first by due time, then by topology key
	using action_key = std::pair<std::chrono::steady_clock::time_point, nano::topo_key>;

	// Returns the quorum target for spear or repair heads
	unsigned quorum (head_state const &) const;
	// Applies a round result to its head cursor, stats, and cooldown state
	void conclude (head_state &, std::chrono::steady_clock::time_point now);
	// Tracks newly discovered topology keys and queues their block fetches
	discover_result discover (std::deque<nano::topo_key> const &);
	// Recomputes a repair head's height band behind the spear
	void refresh_repair_band (head_state &);
	// Moves a repair head back to the start of its current band after cooldown
	void wrap_repair (head_state &, std::chrono::steady_clock::time_point now);
	// True when repair heads can idle because no unresolved discovered work remains
	bool repair_quiesced () const;
	// True when discovery should pause due to queue pressure or dependency gaps
	bool discovery_backpressured () const;

	// Schedules a member for block fetching
	void enqueue_fetch (member &, std::chrono::steady_clock::time_point);
	// Removes a member from the fetch queue if present
	void erase_fetch_queue (member &);
	// Schedules a fetched member for out-of-order submission
	void enqueue_submit (member &, std::chrono::steady_clock::time_point);
	// Removes a member from the submit queue if present
	void erase_submit_queue (member &);
	// Erases a tracked member from all indexes and updates gap counters
	void erase_member (std::unordered_map<nano::block_hash, member>::iterator);
	// Marks a member locally resolved without submitting its block
	void mark_resolved (member &);
	// Advances the contiguous submit frontier through resolved members
	void advance_frontier ();
	// True for block processor results that should not be retried as dependency gaps
	bool terminal (nano::block_status) const;
	// True for block processor results caused by missing dependencies
	bool dependency_gap (nano::block_status) const;

	std::unordered_map<nano::block_hash, member> members; // Tracked discovered members indexed by hash
	std::map<nano::topo_key, nano::block_hash> members_by_key; // Ordered lookup for frontier advancement and batches
	std::set<action_key> fetch_queue; // Due fetch actions ordered by retry time and topology key
	std::set<action_key> submit_queue; // Due non-frontier submissions ordered by retry time and topology key
	std::unordered_map<id_t, fetch_state> fetches; // Outstanding fetch requests indexed by async tag id

	nano::topo_scan_config const & config; // Tuning values for heads, pacing, queues, and repair behavior
	nano::stats & stats; // Stat sink for scan outcomes and invalid sample references
	std::vector<head_state> heads; // Spear head followed by repair heads scanning older height bands
	size_t robin{ 0 }; // Next head index considered first when selecting launch work
	nano::topo_key submit_frontier{}; // Last contiguous topology key resolved for ordered submission
	bool spear_at_tip{ false }; // True once repeated empty spear rounds indicate no newer keys
	size_t frontier_gaps{ 0 }; // Count of frontier members delayed by dependency gaps
};
}
