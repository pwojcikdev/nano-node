#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <memory>
#include <optional>
#include <unordered_map>

namespace mi = boost::multi_index;

namespace nano::scheduler
{
/**
 * Tracks active priority elections per bucket
 */
class election_tracker final
{
public:
	election_tracker () = default;

	struct entry
	{
		std::shared_ptr<nano::election> election;
		nano::qualified_root root;
		nano::bucket_index bucket;
		nano::priority_timestamp priority;
	};

	// Insert a new election
	bool insert (std::shared_ptr<nano::election> const & election, nano::bucket_index bucket, nano::priority_timestamp priority);

	// Remove an election by its pointer (more efficient)
	bool erase (std::shared_ptr<nano::election> const & election);

	// Check if an election is tracked
	bool contains (std::shared_ptr<nano::election> const & election) const;

	// Get the lowest priority (highest value) election for a bucket
	std::optional<entry> worst (nano::bucket_index bucket) const;

	// Get count of elections for a bucket
	std::unordered_map<nano::bucket_index, size_t> sizes () const;
	size_t size () const;
	size_t size (nano::bucket_index bucket) const;
	bool empty () const;
	bool empty (nano::bucket_index bucket) const;

	void clear ();

	nano::container_info container_info () const;

private:
	// clang-format off
	class tag_root {};
	class tag_ptr {};
	class tag_bucket_priority {};

	using ordered_elections = boost::multi_index_container<entry,
	mi::indexed_by<
		mi::hashed_unique<mi::tag<tag_root>,
			mi::member<entry, nano::qualified_root, &entry::root>>,
		mi::hashed_unique<mi::tag<tag_ptr>,
			mi::member<entry, std::shared_ptr<nano::election>, &entry::election>>,
		mi::ordered_non_unique<mi::tag<tag_bucket_priority>,
			mi::composite_key<entry,
				mi::member<entry, nano::bucket_index, &entry::bucket>,
				mi::member<entry, nano::priority_timestamp, &entry::priority>,
				mi::member<entry, nano::qualified_root, &entry::root>
			>>
	>>;
	// clang-format on

	ordered_elections elections;

	// Track election counts per bucket for efficient lookups
	std::unordered_map<nano::bucket_index, size_t> bucket_sizes;
};
}