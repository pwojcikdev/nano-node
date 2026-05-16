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
 * Tracks the in-memory topology scan state for the bootstrap topology strategy.
 *
 * Phase 1 (index discovery): walks a `discovery cursor` across the topology
 *   space. At each cursor position the head accumulates a deduplicated union
 *   of candidate `topo_key` entries from `consideration_count` peer responses.
 *   After enough responses arrive, the union is trimmed to `candidates` entries
 *   and queued for block fetching, and the cursor advances to the last queued
 *   key. The discovery cursor is open-loop with respect to ledger processing
 *   so the pipeline can keep peers busy ahead of the block processor; the
 *   `max_blocks_queued` backpressure check in `next()` bounds how far ahead
 *   discovery can get from processing.
 *
 * Phase 2 (block fetching): tracks per-block state (pending, in_flight, received,
 *   submitted) and exposes both a topologically-ordered drain (`next_ordered_blocks`)
 *   and an unconditional eviction (`block_done`) for blocks delivered or completed
 *   via other channels.
 *
 * Phase 3 (ledger feedback): `mark_indexed` records a (hash, topo_height) pair
 *   for blocks that successfully entered the ledger. The highest such key is
 *   the `indexed` cursor — the externally-reported topological position. This
 *   is closed-loop with ledger processing and lags the discovery cursor by at
 *   most the queue depth. `cursor()` returns this value, not the discovery
 *   cursor, so external observers see the genuinely-processed position.
 *
 * Phase 4 (stall recovery): `check_poisoning` fires when the queue has
 *   outstanding work but its drain heartbeat (`block_done` evicting a tracked
 *   entry) has been silent for `poisoning_timeout`. The recovery action then
 *   hinges on whether the cycle made *real forward progress* — i.e. whether
 *   `indexed` advanced since the cycle started. Re-chewing already-known
 *   blocks as `old` moves the queue (refreshes the heartbeat) but does NOT
 *   advance `indexed`, so it is not progress. If `indexed` advanced: gentle
 *   reset (clear queue, retry from the new `indexed`) and re-arm the rollback
 *   step. If it did not (the anchor sits past a gap the ledger can't bridge):
 *   rewind `indexed` by a doubling distance and reset, so successive stuck
 *   cycles walk back through history in O(log n) steps until a workable
 *   position is found.
 *
 * This class is not internally synchronized; callers must hold the bootstrap
 * context mutex when invoking its methods.
 */
class topo_scan_index
{
public:
	explicit topo_scan_index (nano::topo_scan_config const &);

	// --- Phase 1: index discovery ---

	// Returns the next position to query, or nullopt when the discovery cursor
	// is in cooldown, topology end has been reached, or block-fetch backpressure
	// is engaged.
	// `now` is injectable for deterministic testing of cooldown behavior.
	std::optional<nano::topo_key> next (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Accumulate a peer response. Returns true when the discovery cursor advances
	// (new entries queued or end-of-topology detected). Does not affect `indexed`.
	bool process (nano::topo_key start, std::deque<nano::topo_key> const & entries);

	// --- Phase 2: block fetching ---

	// `now` is injectable for deterministic testing of in-flight retry timing.
	std::deque<nano::block_hash> next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());
	void block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block);
	std::deque<std::shared_ptr<nano::block>> next_ordered_blocks (std::size_t max_count);
	// Evict a tracked block once the processor confirms it (the inspect callback
	// fires for any source/result). When the hash is actually found in the queue
	// this is the pipeline's drain heartbeat — it refreshes the poisoning
	// *trigger* clock (`drained_at`). It does NOT by itself count as forward
	// progress for the gentle-vs-escalate decision (re-processing a known block
	// as `old` evicts an entry but doesn't advance `indexed`). A miss (hash not
	// tracked, e.g. a ghost of a pre-reset submission) is a full no-op. `now` is
	// injectable for deterministic testing.
	void block_done (nano::block_hash const & hash, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// --- Phase 3: ledger feedback ---

	// Record that a block has been confirmed processed into the ledger.
	// Monotonically advances the externally-reported `indexed` cursor to the
	// supplied (topo_height, hash) when it strictly exceeds the current value.
	// No-op otherwise. Does not affect the discovery cursor or per-round state.
	void mark_indexed (nano::block_hash const & hash, uint64_t topo_height);

	// Reset in-flight requests whose timestamp is older than `block_retry` back to
	// pending so they can be retried via the standard pending-pickup path. This is
	// a fail-safe: when `count_in_flight()` reaches `max_blocks_outstanding`, the
	// retry branch in `next_blocks` is gated by backpressure and never runs, so
	// stale slots would otherwise be held indefinitely. Returns the number of
	// entries reset, so the caller can record a "timed out" stat. Intended to be
	// invoked periodically by an external scheduler — `next_blocks` does not call
	// this on its own. `now` is injectable for deterministic testing.
	std::size_t cleanup (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Clear everything queued and resume discovery from the current `indexed`
	// cursor (`head.cursor = indexed`). The indexed cursor itself is preserved.
	// Snapshots `indexed` as the new cycle's progress baseline. Used by
	// `check_poisoning` and available for any other externally-decided trigger.
	void reset_discovery ();

	// Stall recovery. Fires when the queue has outstanding work but no tracked
	// block has drained for `poisoning_timeout`. If `indexed` advanced since the
	// previous reset (real forward progress), performs a gentle `reset_discovery`
	// and re-arms the rollback step. Otherwise the `indexed` anchor is past an
	// unbridgeable gap: rewinds `indexed` by the current rollback distance (then
	// doubles it, capped at `rollback_max`) before resetting, so successive
	// failures walk back through history in O(log n) steps. Returns true if any
	// rollback was performed. `now` is injectable for deterministic testing.
	bool check_poisoning (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// --- Lifecycle / queries ---

	bool indexing () const;
	// The externally-reported topological position: the highest topo_key whose
	// block has been confirmed processed into the ledger via `mark_indexed`.
	// (Distinct from the internal discovery cursor, which advances open-loop
	// on peer responses to keep the pipeline saturated.)
	nano::topo_key cursor () const;
	bool has_blocks_pending () const;
	// Total queue depth across every state (pending + in_flight + received +
	// submitted). Drives the discovery backpressure check — submitted entries
	// (drained but not yet confirmed processed) are deliberately counted so
	// discovery can't race past blocks still in the processor's pipeline.
	std::size_t count_outstanding () const;
	std::size_t count_pending () const;
	std::size_t count_in_flight () const;
	std::size_t count_received () const;
	std::size_t count_submitted () const;
	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	nano::topo_scan_config const & config;

private:
	// Open-loop discovery state. The `cursor` advances on each quorum-reached
	// peer response to the highest queued entry; the head accumulates candidates
	// from peers at the current cursor position until the round completes.
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

	// The closed-loop tracking cursor: highest topo_key whose block has been
	// confirmed processed into the ledger (via `mark_indexed`). Lags
	// `head.cursor` by at most the queue depth (bounded by `max_blocks_queued`).
	// Exposed via `cursor()`; used for telemetry and (future) persistence.
	nano::topo_key indexed{};

	// Drain heartbeat: timestamp of the last `block_done` that actually evicted
	// a tracked entry (or construction / reset). This is the poisoning *trigger*
	// clock — keyed on OUR queue, not on `mark_indexed`, so ghost completions of
	// pre-reset submissions can't mask a stalled queue. Note: re-processing an
	// already-known block as `old` also refreshes this (the queue *is* moving),
	// which is why it can't be used for the gentle-vs-escalate *decision*.
	std::chrono::steady_clock::time_point drained_at{ std::chrono::steady_clock::now () };

	// Snapshot of `indexed` taken at the start of the current reset cycle
	// (construction / reset / reset_discovery). `check_poisoning` compares the
	// live `indexed` against this to decide real forward progress: only a
	// genuine ledger-frontier advance (`mark_indexed` with a strictly greater
	// key) counts — re-chewing already-known blocks as `old` does not, because
	// their topo_height is <= indexed and `mark_indexed` early-returns.
	nano::topo_key indexed_at_reset{};

	// Current escalating-rollback step (topo-heights). Starts at
	// `config.rollback_min`, doubles (capped at `config.rollback_max`) on every
	// unproductive reset, snaps back to `config.rollback_min` on a productive one.
	uint64_t rollback_distance;

	// Rewind `indexed` back by `distance` topo-heights (clamped at genesis).
	void rewind (uint64_t distance);

	enum class block_state
	{
		pending, // Known but not requested yet.
		in_flight, // Request sent to a peer, awaiting response.
		received, // Received from a peer, awaiting topological-order drain.
		submitted, // Drained and handed off to the block processor, awaiting
		// confirmation via `block_done` (fired by the inspect callback).
		// Held in the queue so backpressure accounts for in-flight processing.
	};

	struct block_entry
	{
		nano::block_hash hash{ 0 };
		nano::topo_key key{};
		block_state state{ block_state::pending };
		std::chrono::steady_clock::time_point timestamp{};
		std::shared_ptr<nano::block> block;
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

	void state_change (block_state old_state, block_state new_state);
};
}
