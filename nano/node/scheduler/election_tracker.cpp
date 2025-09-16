#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/scheduler/election_tracker.hpp>

namespace nano::scheduler
{
bool election_tracker::insert (std::shared_ptr<nano::election> const & election, nano::bucket_index bucket, nano::priority_timestamp priority)
{
	auto root = election->qualified_root;
	entry new_entry{ election, root, bucket, priority };

	auto [it, inserted] = elections.insert (new_entry);
	if (inserted)
	{
		bucket_sizes[bucket]++;
	}

	return inserted;
}

bool election_tracker::erase (std::shared_ptr<nano::election> const & election)
{
	auto & ptr_index = elections.get<tag_ptr> ();

	if (auto it = ptr_index.find (election); it != ptr_index.end ())
	{
		auto bucket = it->bucket;
		ptr_index.erase (it);
		bucket_sizes[bucket]--;
		return true;
	}

	return false;
}

auto election_tracker::worst (nano::bucket_index bucket) const -> std::optional<entry>
{
	auto & bucket_priority_index = elections.get<tag_bucket_priority> ();
	auto range = bucket_priority_index.equal_range (bucket);

	if (range.first != range.second)
	{
		// The last element in the range has the highest priority value (lowest priority)
		auto it = --range.second;
		return *it;
	}

	return std::nullopt;
}

bool election_tracker::contains (std::shared_ptr<nano::election> const & election) const
{
	auto const & ptr_index = elections.get<tag_ptr> ();
	return ptr_index.contains (election);
}

size_t election_tracker::size (nano::bucket_index bucket) const
{
	auto it = bucket_sizes.find (bucket);
	return it != bucket_sizes.end () ? it->second : 0;
}

size_t election_tracker::size () const
{
	return elections.size ();
}

auto election_tracker::sizes () const -> std::unordered_map<nano::bucket_index, size_t>
{
	return bucket_sizes;
}

bool election_tracker::empty () const
{
	return elections.empty ();
}

bool election_tracker::empty (nano::bucket_index bucket) const
{
	return size (bucket) == 0;
}

void election_tracker::clear ()
{
	elections.clear ();
	bucket_sizes.clear ();
}

nano::container_info election_tracker::container_info () const
{
	nano::container_info info;
	info.put ("elections", elections.size ());
	info.put ("buckets", bucket_sizes.size ());

	nano::container_info bucket_info;
	for (auto const & [bucket, size] : bucket_sizes)
	{
		bucket_info.put (std::to_string (bucket), size);
	}
	info.add ("bucket_sizes", bucket_info);

	return info;
}
}