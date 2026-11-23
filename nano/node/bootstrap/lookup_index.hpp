#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>

#include <boost/container/small_vector.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <vector>

namespace mi = boost::multi_index;

namespace nano::bootstrap
{
/*
 * Blocks known to exist but missing from the ledger, while the account that contains each of them is looked up so that the account can be pulled.
 * A block is due for a lookup as soon as it is added. After a lookup that found nothing it waits a delay that grows with every failure,
 * and goes to a peer that has not been asked yet, until its retries run out.
 * Pure state, guarded externally by the bootstrap_context mutex.
 */
class lookup_index
{
public:
	explicit lookup_index (nano::lookup_config const &);

	enum class insert_result
	{
		inserted, // Newly tracked, due for a lookup right away
		duplicate, // Already tracked
		overfill, // Refused, the index is full
	};

	enum class fail_result
	{
		retry, // Due for another lookup once its delay has passed
		exhausted, // Out of retries, no longer tracked
		stale, // The block was not waiting on that lookup
	};

	struct request
	{
		nano::block_hash hash;
		std::vector<nano::account> exclude; // Peers that already failed to find the block
	};

	// Starts tracking a block, due for a lookup right away
	insert_result insert (nano::block_hash const &, std::chrono::steady_clock::time_point now);

	// The block that has been due for a lookup the longest
	std::optional<request> next (std::chrono::steady_clock::time_point now) const;

	// Records a lookup sent to a peer, false if the block no longer waits for one
	bool dispatch (nano::block_hash const &, id_t id, nano::account const & peer);

	// Lets the block be looked up at peers that already failed to find it, for when no other peer is left
	void reset_peers (nano::block_hash const &);

	// The lookup came back without an account or got no answer in time
	fail_result fail (nano::block_hash const &, id_t id, std::chrono::steady_clock::time_point now);

	// Stops tracking a block, false if it was not tracked
	bool erase (nano::block_hash const &);

	void clear ();
	bool contains (nano::block_hash const &) const;
	std::size_t size () const;

	nano::container_info container_info () const;

private: // Dependencies
	nano::lookup_config const & config;

private:
	enum class entry_state
	{
		waiting, // Due for a lookup at `due`
		requested, // Waiting for the answer to lookup `id`
	};

	struct entry
	{
		nano::block_hash hash;
		entry_state state{ entry_state::waiting };
		std::chrono::steady_clock::time_point due; // When the next lookup is due, while waiting
		id_t id{ 0 }; // Lookup the block waits on, while requested
		unsigned attempts{ 0 }; // Lookups sent so far

		// Peers asked so far
		// Most blocks are found by the first peer, so the list rarely grows past the entry itself
		boost::container::small_vector<nano::account, 1> tried;
	};

	// Wait before the next lookup of a block whose lookups failed this many times
	std::chrono::milliseconds retry_delay (unsigned failures) const;

	// clang-format off
	class tag_hash {};
	class tag_schedule {};

	using ordered_entries = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_hash>,
			mi::member<entry, nano::block_hash, &entry::hash>>,
		mi::ordered_non_unique<mi::tag<tag_schedule>,
			mi::composite_key<entry,
				mi::member<entry, entry_state, &entry::state>,
				mi::member<entry, std::chrono::steady_clock::time_point, &entry::due>>>
	>>;
	// clang-format on

	ordered_entries entries;
};

nano::stat::detail to_stat_detail (lookup_index::insert_result);
nano::stat::detail to_stat_detail (lookup_index::fail_result);
}
