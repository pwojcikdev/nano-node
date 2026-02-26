#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <deque>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * Topo scan implements a two-phase bootstrap strategy:
 * Phase 1 (index): Fetches lightweight (topo_index, block_hash) entries from peers to build a map of the topology.
 * Phase 2 (blocks): Uses the collected index to concurrently fetch blocks from multiple peers.
 * This class is independently testable and does not depend on the network or ledger.
 */
class topo_scan
{
public:
	struct index_entry
	{
		uint64_t topo_index{ 0 };
		nano::block_hash hash{ 0 };
	};

	// Query position for index fetching. Both zero = start from beginning.
	struct index_query
	{
		uint64_t start_index{ 0 };
		nano::block_hash start_hash{ 0 };

		bool is_zero () const
		{
			return start_index == 0 && start_hash.is_zero ();
		}
	};

	topo_scan (topo_scan_config const &, nano::stats &);

	// --- Phase 1: Index fetching ---

	// Returns the next position to query from a peer.
	// Returns a zero query if nothing is available (all in cooldown or done).
	index_query next ();

	// Process an index response from a peer.
	// Returns true if this head advanced (converged).
	bool process (index_query start, std::deque<index_entry> const & entries);

	// --- Phase 2: Block fetching ---

	// Returns up to max_count block hashes that need fetching.
	// Marks returned hashes as in-flight.
	std::deque<nano::block_hash> next_blocks (std::size_t max_count);

	// Marks a single block as received.
	void received (nano::block_hash const & hash);

	// --- State queries ---

	// Returns true if there are still index ranges to scan
	bool indexing () const;

	// Returns true if there are blocks waiting to be fetched or in-flight
	bool has_blocks_pending () const;

	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	topo_scan_config const & config;
	nano::stats & stats;

private:
	// Represents a scanning head that tracks progress through a portion of the topo index
	struct topo_head
	{
		// Current scan position (cursor advances forward as entries are received)
		uint64_t cursor_index{ 0 };
		nano::block_hash cursor_hash{ 0 };

		unsigned requests{ 0 };
		unsigned completed{ 0 };
		std::chrono::steady_clock::time_point timestamp{};

		bool done{ false }; // True when this head received a short/empty response

		void reset_progress ()
		{
			requests = 0;
			completed = 0;
			timestamp = {};
			done = false;
			cursor_index = 0;
			cursor_hash = { 0 };
		}
	};

	topo_head head;

	// Block tracking for phase 2
	enum class block_status
	{
		pending,   // Waiting to be fetched
		in_flight, // Request sent, awaiting response
		completed, // Block received
	};

	struct block_entry
	{
		nano::block_hash hash{ 0 };
		uint64_t topo_index{ 0 };
		block_status status{ block_status::pending };
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
};
}
