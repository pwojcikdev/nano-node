#pragma once

#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/distinct_peers.hpp>

#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/mem_fun.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/random_access_index.hpp>
#include <boost/multi_index/sequenced_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <map>
#include <set>
#include <vector>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * Frontier scan divides the account space into ranges and scans each range for outdated frontiers in parallel.
 * This class is used to track the progress of each range.
 */
class frontier_scan_index
{
public:
	frontier_scan_index (frontier_scan_config const &, nano::stats &);

	nano::account next ();
	bool process (nano::account start, std::deque<std::pair<nano::account, nano::block_hash>> const & response);

	// Distinct-peer round management for the head covering `start` (driven by the strategy,
	// which holds the channels), mirroring the topo spear: spread the consideration_count
	// requests across distinct peers and size the round to the reachable peer pool.
	void freeze_target (nano::account start, std::size_t capable_peers);
	void record_query (nano::account start, nano::node_id peer);
	void new_round (nano::account start);
	void cap_target (nano::account start);
	bool round_full (nano::account start) const;
	std::vector<nano::node_id> seen_peers (nano::account start) const;

	void reset ();

	nano::container_info container_info () const;

private: // Dependencies
	frontier_scan_config const & config;
	nano::stats & stats;

private:
	// Represents a range of accounts to scan, once the full range is scanned (goes past `end`) the head wraps around (to the `start`)
	struct frontier_head
	{
		frontier_head (nano::account start_a, nano::account end_a) :
			start{ start_a },
			end{ end_a },
			next{ start_a }
		{
		}

		// The range of accounts to scan is [start, end)
		nano::account const start;
		nano::account const end;

		// We scan the range by querying frontiers starting at 'next' and gathering candidates
		nano::account next;
		std::set<nano::account> candidates;

		unsigned requests{ 0 };
		unsigned completed{ 0 };
		std::chrono::steady_clock::time_point timestamp{};
		size_t processed{ 0 }; // Total number of accounts processed

		// Distinct peers queried for the current page plus the adaptive round size, so the
		// consideration_count requests spread across different peers.
		nano::bootstrap::distinct_peers peers;

		nano::account index () const
		{
			return start;
		}

		void reset ()
		{
			next = start;
			candidates.clear ();
			requests = 0;
			completed = 0;
			timestamp = {};
			processed = 0;
			peers.clear ();
		}
	};

	// clang-format off
	class tag_sequenced {};
	class tag_start {};
	class tag_timestamp {};

	using ordered_heads = boost::multi_index_container<frontier_head,
	mi::indexed_by<
		mi::random_access<mi::tag<tag_sequenced>>,
		mi::ordered_unique<mi::tag<tag_start>,
			mi::const_mem_fun<frontier_head, nano::account, &frontier_head::index>>,
		mi::ordered_non_unique<mi::tag<tag_timestamp>,
			mi::member<frontier_head, std::chrono::steady_clock::time_point, &frontier_head::timestamp>>
	>>;
	// clang-format on

	ordered_heads heads;

	// Round size for the head: the frozen adaptive target, or consideration_count when not
	// yet frozen (the latter preserves behavior when the index is driven without the strategy).
	std::size_t frontier_target (frontier_head const &) const;
};
}