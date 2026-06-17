#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <deque>
#include <memory>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * Topo blocks engine: tracks discovered-but-missing blocks, drives random-block fetching, and
 * releases fetched blocks to the block processor in topological order.
 *
 * Entries are ordered by topo_key so the submit cursor can release a contiguous, dependency-safe
 * prefix; a hashed index on the block hash matches fetch responses back to their entry. A block
 * that cannot be fetched after `max_fetch_attempts` is demoted to a tolerated gap (skip) so it
 * never wedges the submit cursor. Pure state, guarded externally by the bootstrap_context mutex.
 */
class topo_blocks
{
public:
	topo_blocks (topo_scan_config const &, nano::stats &);

	// Record discovered topo entries that are missing from our ledger (insert-if-absent; re-arms tolerated gaps)
	void add (std::deque<nano::topo_key> const & entries);

	// True if any entry is ready to (re)fetch or needs demotion (used to block the fetch loop)
	bool has_fetchable (std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ()) const;

	// Up to `max` block hashes to fetch next (lowest topo_key first); marks them requested
	std::deque<nano::block_hash> next_fetch_batch (std::size_t max, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ());

	// Match fetched blocks to their entries and mark them ready for submission
	void process_fetched (std::deque<std::shared_ptr<nano::block>> const & blocks);

	// Re-arm requested entries for immediate retry after a request timed out or failed
	void rearm (std::deque<nano::block_hash> const & hashes);

	// True if the lowest entry is ready to release (fetched or a tolerated gap)
	bool has_submittable () const;

	// The next contiguous topologically-ordered batch of fetched blocks; removes released entries
	std::deque<std::shared_ptr<nano::block>> next_submit_batch (std::size_t max);

	// Number of entries still awaiting fetch (the spearhead back-pressure signal)
	std::size_t pending_count () const;

	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	topo_scan_config const & config;
	nano::stats & stats;

private:
	enum class entry_state
	{
		pending, // Missing from the ledger, awaiting fetch
		fetched, // Block in hand, awaiting in-order submission
		skipped, // Could not be fetched; a tolerated gap that does not block the submit cursor
	};

	struct entry
	{
		nano::topo_key key;
		std::shared_ptr<nano::block> block; // Set once fetched
		entry_state state{ entry_state::pending };
		unsigned attempts{ 0 };
		std::chrono::steady_clock::time_point last_requested{};

		nano::block_hash hash () const
		{
			return key.hash;
		}
	};

	// clang-format off
	class tag_key {};
	class tag_hash {};

	using ordered_entries = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::ordered_unique<mi::tag<tag_key>,
			mi::member<entry, nano::topo_key, &entry::key>>,
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::const_mem_fun<entry, nano::block_hash, &entry::hash>>
	>>;
	// clang-format on

	ordered_entries entries;
	std::size_t pending{ 0 }; // Count of entries awaiting fetch (the spearhead back-pressure signal)
};
}
