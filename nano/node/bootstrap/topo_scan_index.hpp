#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * In-memory state for the topology bootstrap strategy. Not internally
 * synchronized: callers hold the bootstrap context mutex.
 *
 * MULTI-HEAD MODEL
 * ----------------
 * Several independent scanning `head`s walk the topology index concurrently,
 * each accumulating a `consideration_count`-peer union at its own cursor and
 * advancing only over that union (the proven, gap-safe discovery from the
 * previous design — kept verbatim, just one accumulator per head):
 *
 *   - head 0 is the SPEAR: it scans the highest discovered region forward,
 *     extending the frontier. This is the bulk of progress.
 *   - heads 1..K-1 are REPAIR heads: free-roaming cursors that roll BACK to
 *     wherever a gap is and re-scan upward with a fresh union. They are NOT
 *     bounded by the confirmed watermark. A key the spear's union skipped sits
 *     below the watermark once its chunk completes, so a repair head rolls its
 *     cursor back to below a gapped block's height and re-unions that region;
 *     a fresh peer sample re-enumerates the skipped key. If the gap persists,
 *     the head's rollback distance escalates (it walks further back).
 *
 * No account resolution, no by-hash dependency chasing — repair is pure union
 * re-scan with free positioning. Account-based dependency walking is left to
 * the priority/dependency strategies.
 *
 * CHUNKS
 * ------
 * Each time a head finalizes its union it emits a `chunk`: the lowest
 * `candidates` keys of the union, submitted to the block processor as a unit
 * and evaluated by their per-block results. A chunk is `done` only when every
 * member reached the ledger; a `gapped` member keeps its chunk open (it is
 * retained and re-submitted on a retry timer, never silently dropped). The
 * head that owns a chunk reads its outcome to decide whether its region is
 * clean or needs another (deeper) re-scan.
 *
 * MEMBERS & WATERMARK
 * -------------------
 * All discovered keys live in one `members` container ordered by `topo_key`
 * (so repair-inserted low keys slot into their correct topological position)
 * and hashed by block hash (O(1) result routing). New unions dedup against it,
 * so a re-scan only adds the previously-skipped keys. `confirmed_watermark` is
 * the top of the contiguous in-ledger prefix — a progress REPORT (`cursor()`),
 * NOT a barrier; repair heads scan freely below it.
 *
 * TERMINATION
 * -----------
 * `caught_up` = the spear saw the tip (a short/empty page) AND no members are
 * gapped or in-flight (everything in_ledger/terminal). Until then a gapped
 * member keeps the bootstrap from declaring done, so a skipped key is never
 * silently lost — the repair loop keeps re-scanning for it.
 */
class topo_scan_index
{
public:
	explicit topo_scan_index (nano::topo_scan_config const &);

	// Number of concurrent scanning heads (head 0 = spear, rest = repair).
	std::size_t head_count () const;

	// --- Discovery (per head) ---

	// Next cursor for head `h` to query, or nullopt under cooldown / quorum-wait
	// / backpressure / (spear) topology-end. For a repair head this also (re)homes
	// the cursor onto the current gap region when its previous region is clean.
	// `now` injectable for tests.
	std::optional<nano::topo_key> next (std::size_t head, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Accumulate a peer response for head `h` queried at `start`. Returns true
	// when the head's cursor advances (a chunk was emitted, or — spear only —
	// the topology end was reached). Stale responses (start != head cursor) are
	// dropped. New keys are deduped against `members`; only previously-unseen
	// keys become fetchable members.
	bool process (std::size_t head, nano::topo_key start, std::deque<nano::topo_key> const & entries);

	// --- Fetch (shared across heads) ---

	// Hashes to fetch: members that are `pending`, or `in_flight` past
	// `block_retry`. Lowest topo_key first (deps before dependents). Transitioned
	// to `in_flight`. `now` injectable for tests.
	std::deque<nano::block_hash> next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// A peer delivered a block: member -> `received`, block retained (kept so a
	// gapped member can be re-submitted without re-fetching).
	void block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block);

	// Blocks to hand to the processor, lowest topo_key first: `received` members
	// (first submission) and `gapped` members past the re-submit retry interval
	// (their dependency may have since been filled by a repair head). Transitioned
	// to `submitted`. `now` injectable for tests.
	std::deque<std::shared_ptr<nano::block>> next_submit (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// --- Result feedback (from the inspect callback) ---

	// A submitted block reached the ledger (`progress` or `old`). Member ->
	// `in_ledger` (the member already carries its topo_key from discovery, so no
	// height is needed); advances `confirmed_watermark` over the contiguous prefix.
	void block_indexed (nano::block_hash const & hash);

	// A submitted block came back as a dependency gap (gap_previous / gap_source
	// / gap_epoch_open_pending). Member -> `gapped`: retained and re-submitted
	// later; its (lowest) height homes a repair head's rollback. NOT dropped.
	void block_gapped (nano::block_hash const & hash, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// A submitted block is terminally unusable here (fork / bad_signature / ...).
	// Member -> `terminal`: evicted from the open set (not a hole), logged loud.
	void block_terminal (nano::block_hash const & hash);

	// A queued member is already in our ledger (pre-fetch redundancy check) ->
	// treated as `in_ledger` without a fetch. No-op if untracked.
	void mark_redundant (nano::block_hash const & hash);

	// Return `in_flight` members older than `block_retry` to `pending` for retry.
	// Returns the count reset. `now` injectable for tests.
	std::size_t cleanup (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	void reset ();

	// Poll mode: re-arm the spear (after `caught_up`) to re-page from its cursor
	// and pick up blocks appended to the index since the tip was reached.
	void repoll ();

	// --- Lifecycle / queries ---

	// Work remains: spear hasn't reached the tip, or any member is unresolved
	// (pending / in_flight / received / gapped). Drives thread parking.
	bool indexing () const;
	// Tip reached AND every member in_ledger/terminal (no gaps, nothing in
	// flight). The whole topology is synced.
	bool caught_up () const;
	// Externally-reported position: top of the contiguous in-ledger prefix.
	// Report only — repair heads scan below it.
	nano::topo_key cursor () const;

	std::size_t count_members () const;
	std::size_t count_pending () const;
	std::size_t count_in_flight () const;
	std::size_t count_received () const;
	std::size_t count_submitted () const;
	std::size_t count_in_ledger () const;
	std::size_t count_gapped () const;
	std::size_t count_chunks () const;

	nano::container_info container_info () const;

private: // Dependencies
	nano::topo_scan_config const & config;

private:
	// --- Member ---

	enum class member_state
	{
		pending, // Discovered, not yet requested.
		in_flight, // Request sent, awaiting delivery.
		received, // Delivered by a peer, awaiting submission.
		submitted, // Handed to the processor, awaiting its result.
		in_ledger, // Confirmed into the ledger (progress / old / redundant).
		gapped, // Submitted but a dependency was missing — retained, re-submitted.
		terminal, // Fork / bad signature — evicted, not a hole.
	};

	struct member
	{
		nano::block_hash hash{ 0 };
		nano::topo_key key{};
		member_state state{ member_state::pending };
		std::shared_ptr<nano::block> block;
		std::chrono::steady_clock::time_point timestamp{}; // last fetch / submit
		uint64_t chunk_id{ 0 }; // owning chunk
	};

	// clang-format off
	class tag_key {};
	class tag_hash {};
	using member_set = boost::multi_index_container<member,
	mi::indexed_by<
		mi::ordered_unique<mi::tag<tag_key>,
			mi::member<member, nano::topo_key, &member::key>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<member, nano::block_hash, &member::hash>>
	>>;
	// clang-format on

	member_set members;

	// --- Chunk (a head's submission/evaluation batch) ---

	struct chunk
	{
		uint64_t id{ 0 };
		std::size_t head{ 0 }; // emitting head
		nano::topo_key lo{}, hi{}; // covered range
		std::size_t total{ 0 }; // members emitted
		std::size_t resolved{ 0 }; // in_ledger + terminal
		bool done () const
		{
			return resolved >= total;
		}
	};

	std::map<uint64_t, chunk> chunks;
	uint64_t next_chunk_id{ 0 };

	// --- Scanning head ---

	struct scan_head
	{
		bool repair{ false }; // false == spear
		nano::topo_key cursor{}; // current scan position

		// consideration_count-peer union accumulator (unchanged mechanism).
		std::set<nano::topo_key> candidates;
		unsigned requests{ 0 };
		unsigned completed{ 0 };
		std::chrono::steady_clock::time_point timestamp{};

		bool done{ false }; // spear: topology end reached
		uint64_t rollback{ 0 }; // repair: current rollback distance (escalates)
		nano::topo_key target{}; // repair: gap height this region is chasing

		void reset_union ()
		{
			requests = 0;
			completed = 0;
			timestamp = {};
			candidates.clear ();
		}
	};

	std::vector<scan_head> heads;

	// Top of the contiguous in-ledger prefix (by topo_key). Report only.
	nano::topo_key confirmed_watermark{};
	// Spear saw a short/empty page: the discovered frontier reached the tip.
	bool tip_reached{ false };

	// Keys of currently-gapped members, ordered. Homes repair-head rollback:
	// repair head `h` chases the (h-1)-th lowest gap. Kept in sync with the
	// `gapped` member transitions.
	std::set<nano::topo_key> gapped_keys;

	// O(1) state counters, kept in sync with transitions.
	std::size_t pending_count{ 0 };
	std::size_t in_flight_count{ 0 };
	std::size_t received_count{ 0 };
	std::size_t submitted_count{ 0 };
	std::size_t in_ledger_count{ 0 };
	std::size_t gapped_count{ 0 };

private:
	// Transition a member's state, keeping counters in sync.
	void set_state (member const &, member_state);
	// Advance `confirmed_watermark` over the contiguous in_ledger/terminal prefix
	// at the head of the ordered member set, erasing (pruning) those members.
	// Call after a member reaches in_ledger / terminal.
	void advance_watermark ();
	// Mark a member (found by hash) resolved into the ledger, prune-advance the
	// watermark. Shared by `block_indexed` and `mark_redundant`.
	void resolve_member (nano::block_hash const & hash);
	// topo_height of the (n)-th lowest gapped member, or nullopt if fewer than
	// n+1 gaps exist — homes repair head (n+1)'s rollback.
	std::optional<uint64_t> nth_gap_height (std::size_t n) const;
	// True if head `h`'s union is eligible to (re)query at its cursor now.
	bool eligible (scan_head const &, std::chrono::steady_clock::time_point now) const;
	// Mark a member resolved against its chunk; erase the chunk when fully done.
	void chunk_resolve (uint64_t chunk_id);
	// Discovery backpressure: total unresolved members across all heads.
	std::size_t outstanding () const;
};
}
