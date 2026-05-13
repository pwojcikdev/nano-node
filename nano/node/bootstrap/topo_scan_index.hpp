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
 * Phase 1 (index discovery): walks a single global cursor across the topology
 *   space. The cursor is the last topo_key whose block has been confirmed
 *   processed into the ledger (`mark_indexed`) — closed-loop with ledger
 *   processing. Peer queries always start from this cursor; responses
 *   accumulate a deduplicated union of candidate entries that get queued for
 *   block fetching. The cursor advances only when the strategy reports a
 *   block as having reached the ledger.
 *
 * Phase 2 (block fetching): tracks per-block state (pending, in_flight, completed)
 *   and exposes both a topologically-ordered drain (`next_ordered_blocks`) and an
 *   unconditional cleanup (`received`) for blocks delivered via other channels.
 *
 * Phase 3 (ledger feedback): `mark_indexed` records a (hash, topo_height) pair
 *   for blocks that successfully entered the ledger. The highest such key drives
 *   the discovery cursor forward, so discovery progress is bounded by actual
 *   block processing.
 *
 * This class is not internally synchronized; callers must hold the bootstrap
 * context mutex when invoking its methods.
 */
class topo_scan_index
{
public:
	explicit topo_scan_index (nano::topo_scan_config const &);

	// --- Phase 1: index discovery ---

	// Returns the next position to query, or nullopt when the cursor is in
	// cooldown / topology end has been reached / block backpressure engaged.
	// The returned key is always the current `indexed` cursor — closed-loop
	// with ledger processing.
	// `now` is injectable for deterministic testing of cooldown behavior.
	std::optional<nano::topo_key> next (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Accumulate a peer response. Returns true when discovery progresses (new
	// entries queued or end-of-topology detected). Note: does NOT advance the
	// cursor — only `mark_indexed` does.
	bool process (nano::topo_key start, std::deque<nano::topo_key> const & entries);

	// --- Phase 2: block fetching ---

	// `now` is injectable for deterministic testing of in-flight retry timing.
	std::deque<nano::block_hash> next_blocks (std::size_t max_count, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());
	void block_received (nano::block_hash const & hash, std::shared_ptr<nano::block> const & block);
	std::deque<std::shared_ptr<nano::block>> next_ordered_blocks (std::size_t max_count);
	void block_done (nano::block_hash const & hash);

	// --- Phase 3: ledger feedback ---

	// Record that a block has been confirmed processed into the ledger.
	// Monotonically advances the discovery cursor (`indexed`) to the supplied
	// (topo_height, hash) when it strictly exceeds the current cursor. Resets
	// the per-round discovery state so a fresh query batch can start at the
	// new cursor position. No-op if the supplied key is not strictly greater
	// than the current cursor.
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

	// --- Lifecycle / queries ---

	bool indexing () const;
	// The current discovery cursor: last topo_key confirmed processed into the ledger.
	nano::topo_key cursor () const;
	bool has_blocks_pending () const;
	std::size_t count_outstanding () const;
	std::size_t count_pending () const;
	std::size_t count_in_flight () const;
	std::size_t count_completed () const;
	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	nano::topo_scan_config const & config;

private:
	// Per-round discovery state. A "round" is a batch of up to
	// `consideration_count` peer responses at the same cursor position.
	struct topo_head
	{
		std::set<nano::topo_key> candidates;

		unsigned requests{ 0 };
		unsigned completed{ 0 };
		std::chrono::steady_clock::time_point timestamp{};

		bool done{ false };

		void reset ()
		{
			requests = 0;
			completed = 0;
			timestamp = {};
			done = false;
			candidates.clear ();
		}
	};

	topo_head head;

	// The closed-loop discovery cursor. Advances only via `mark_indexed`, never
	// via peer responses. Represents the highest topo_key whose block has been
	// confirmed processed into the ledger.
	nano::topo_key indexed{};

	enum class block_state
	{
		pending,
		in_flight,
		completed,
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
	std::size_t completed_count{ 0 };

	void state_change (block_state old_state, block_state new_state);
};
}
