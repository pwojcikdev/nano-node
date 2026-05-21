#pragma once

#include <nano/lib/container_info.hpp>
#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <set>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * In-memory state for the topology bootstrap strategy. Not internally
 * synchronized: callers hold the bootstrap context mutex.
 *
 * Pipeline: discover -> fetch -> submit -> confirm.
 *
 * Discovery (`next` / `process`): an open-loop `head.cursor` walks the
 *   topology space. Each cursor position gathers a quorum of candidate
 *   `topo_key`s from `consideration_count` peer responses, queues the first
 *   `candidates` of them, and advances the cursor to the last queued key.
 *   Backpressure (`max_blocks_queued` in `next`) bounds how far discovery
 *   runs ahead of processing.
 *
 * Block queue: every queued entry moves pending -> in_flight -> received ->
 *   submitted, then is evicted by `block_done` once the processor confirms it.
 *   Entries already in our ledger are tagged `redundant` by the pre-fetch
 *   check and never fetched. `next_ordered_blocks` drains in strict
 *   topological order.
 *
 * Indexed cursor: `indexed` is the highest topo_key whose block reached the
 *   ledger (`mark_indexed`). It is the externally-reported position
 *   (`cursor()`), closed-loop with processing. Redundant entries advance it
 *   only via the in-order prefix drain in `next_ordered_blocks`, so it never
 *   skips past unconfirmed work (a gap).
 *
 * Stall recovery (`check_poisoning`): if the queue is non-empty but nothing
 *   has drained for `poisoning_timeout`, recover. A stall where no block was
 *   ever fetched this cycle is a connectivity problem (unreachable peers), not
 *   a bad anchor — it is a no-op (`cleanup` retries in-flight requests when
 *   the network returns; `indexed` is left untouched). Otherwise, if `indexed`
 *   advanced this cycle the anchor is good: clear the queue and retry from
 *   `indexed`. If it did not, the anchor sits past a gap the ledger can't
 *   bridge: rewind `indexed` by an exponentially growing distance so repeated
 *   stalls bisect back to a workable position.
 */
class topo_scan_index
{
public:
	explicit topo_scan_index (nano::topo_scan_config const &);

	// --- Discovery ---

	// Next position to query, or nullopt under cooldown / topology-end /
	// backpressure. `now` injectable for tests.
	std::optional<nano::topo_key> next (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Accumulate a peer response. Returns true when the discovery cursor
	// advances (entries queued or topology end). Does not touch `indexed`.
	bool process (nano::topo_key start, std::deque<nano::topo_key> const & entries);

	// --- Block queue ---

	// Hashes to fetch (pending, or in_flight past `block_retry`), topological
	// order, transitioned to in_flight. `now` injectable for tests.
	std::deque<nano::block_hash> next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());
	void block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block);
	// Two phases: (1) advance the cursor through the strictly-contiguous
	// `redundant` prefix at the head (in-order, gap-safe); (2) return up to
	// `max_count` `received` blocks for submission. `now` injectable for tests.
	std::deque<std::shared_ptr<nano::block>> next_ordered_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());
	// Evict a confirmed block (inspect callback, any source/result). A hit
	// refreshes the drain heartbeat; a miss (untracked hash) is a no-op. Note:
	// this is queue movement, not frontier progress — re-processing a known
	// block as `old` evicts an entry without advancing `indexed`.
	void block_done (nano::block_hash const & hash, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// --- Ledger feedback ---

	// A block reached the ledger. Monotonically advances `indexed` to
	// (topo_height, hash) when it strictly exceeds the current value.
	void mark_indexed (nano::block_hash const & hash, uint64_t topo_height);

	// Tag a queued entry as already in our ledger so it is never fetched. Only
	// records the state + ledger topo_height; the cursor advance is deferred to
	// `next_ordered_blocks` (in topological order), so this is safe to call in
	// any order. No-op if the hash isn't tracked.
	void mark_redundant (nano::block_hash const & hash, uint64_t ledger_topo_height);

	// Return in_flight entries older than `block_retry` to pending so they can
	// be retried. Fail-safe for slots stranded when the in-flight cap gates the
	// `next_blocks` retry path. Returns the count reset. Driven externally;
	// `next_blocks` does not call it. `now` injectable for tests.
	std::size_t cleanup (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Clear the queue and resume discovery from `indexed` (preserved).
	// Snapshots `indexed` as the cycle's progress baseline.
	void reset_discovery ();

	// Stall recovery. No-op unless the queue is non-empty and nothing has
	// drained for `poisoning_timeout`. Then: if no block was fetched this cycle
	// the stall is a connectivity problem — no-op (don't rewind a good anchor
	// during an outage). Else if `indexed` advanced this cycle, gentle
	// `reset_discovery` and re-arm the rollback step; otherwise rewind
	// `indexed` by the rollback distance (doubled, capped at `rollback_max`)
	// before resetting. Returns true if it rolled back. `now` injectable for tests.
	bool check_poisoning (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	void reset ();

	// --- Lifecycle / queries ---

	bool indexing () const;
	// The externally-reported position: highest topo_key confirmed into the
	// ledger (`indexed`). Distinct from the open-loop discovery cursor.
	nano::topo_key cursor () const;
	bool has_blocks_pending () const;
	// Total queue depth (all states). Drives discovery backpressure; submitted
	// entries are counted so discovery can't race past in-flight processing.
	std::size_t count_outstanding () const;
	std::size_t count_pending () const;
	std::size_t count_in_flight () const;
	std::size_t count_received () const;
	std::size_t count_submitted () const;
	std::size_t count_redundant () const;

	nano::container_info container_info () const;

private: // Dependencies
	nano::topo_scan_config const & config;

private:
	// Open-loop discovery state: the `cursor` and the candidate quorum being
	// accumulated at it.
	struct topo_head
	{
		nano::topo_key cursor{};

		std::set<nano::topo_key> candidates;

		unsigned requests{ 0 };
		unsigned completed{ 0 };
		std::chrono::steady_clock::time_point timestamp{};

		bool done{ false };

		void reset ()
		{
			cursor = {};
			requests = 0;
			completed = 0;
			timestamp = {};
			done = false;
			candidates.clear ();
		}
	};

	topo_head head;

	// Closed-loop tracking cursor: highest topo_key confirmed into the ledger
	// (`mark_indexed`). Lags `head.cursor` by at most the queue depth. Exposed
	// via `cursor()`.
	nano::topo_key indexed{};

	// Drain heartbeat: time of the last `block_done` that evicted a tracked
	// entry (or construction / reset). The poisoning trigger clock — keyed on
	// our queue, not on `mark_indexed`, so unrelated ledger activity can't mask
	// a stalled queue.
	std::chrono::steady_clock::time_point drained_at{ std::chrono::steady_clock::now () };

	// `indexed` snapshot at the start of the current cycle. `check_poisoning`
	// treats `indexed > indexed_at_reset` as real forward progress (re-chewing
	// known blocks as `old` doesn't advance `indexed`, so it doesn't count).
	nano::topo_key indexed_at_reset{};

	// Set when a block is fetched from a peer (reaches `received`) this cycle;
	// cleared on every reset. A stall with this still false means no peer
	// delivered anything — a connectivity outage, not a poisoned anchor — so
	// `check_poisoning` must not rewind `indexed`.
	uint64_t fetched_since_reset{ false };

	// Escalating-rollback step (topo-heights): `config.rollback_min`, doubled
	// (capped at `config.rollback_max`) per unproductive stall, reset to the
	// minimum on a productive one.
	uint64_t rollback_distance;

	enum class block_state
	{
		pending, // Known but not requested yet.
		in_flight, // Request sent to a peer, awaiting response.
		received, // Received from a peer, awaiting topological-order drain.
		submitted, // Handed to the block processor, held until `block_done`
		// confirms it (so backpressure accounts for in-flight processing).
		redundant, // Already in our ledger (pre-fetch check); never fetched.
		// Cursor advance deferred to the in-order `next_ordered_blocks` drain.
	};

	struct block_entry
	{
		nano::block_hash hash{ 0 };
		nano::topo_key key{};
		block_state state{ block_state::pending };
		std::chrono::steady_clock::time_point timestamp{};
		std::shared_ptr<nano::block> block;
		// Our ledger's topo_height; only meaningful while state == redundant,
		// used as the `mark_indexed` anchor at the contiguous head.
		uint64_t ledger_topo_height{ 0 };
	};

	// clang-format off
	class tag_sequenced {};
	class tag_hash {};

	using ordered_blocks = boost::multi_index_container<block_entry,
	mi::indexed_by<
		mi::sequenced<mi::tag<tag_sequenced>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<block_entry, nano::block_hash, &block_entry::hash>>
	>>;
	// clang-format on

	ordered_blocks blocks;

	// O(1) counters, kept in sync with state transitions
	std::size_t pending_count{ 0 };
	std::size_t in_flight_count{ 0 };
	std::size_t received_count{ 0 };
	std::size_t submitted_count{ 0 };
	std::size_t redundant_count{ 0 };

private:
	void state_change (block_state old_state, block_state new_state);

	// Rewind `indexed` back by `distance` topo-heights (clamped at genesis).
	void rewind (uint64_t distance);

	// Remove a tracked entry by hash, keeping the O(1) state counters in sync.
	// Returns true if an entry was actually found and erased.
	bool erase_tracked (nano::block_hash const & hash);
};
}
