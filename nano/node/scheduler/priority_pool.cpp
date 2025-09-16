#include <nano/lib/stats.hpp>
#include <nano/node/scheduler/priority_pool.hpp>

namespace nano::scheduler
{
priority_pool::priority_pool (size_t max_size_a, size_t bucket_reserved_a) :
	max_size{ max_size_a },
	bucket_reserved{ bucket_reserved_a }
{
}

bool priority_pool::push (std::shared_ptr<nano::block> const & block, nano::bucket_index bucket, nano::priority_timestamp priority)
{
	auto const hash = block->hash ();

	entry new_entry{ bucket, priority, hash, block };
	auto [it, inserted] = pool.insert (new_entry);

	if (!inserted)
	{
		// Block already exists
		return false;
	}

	// Update bucket size
	bucket_sizes[bucket]++;

	// Check if we need to evict
	if (pool.size () > max_size)
	{
		if (size (bucket) > bucket_reserved)
		{
			// Evict from the same bucket if it exceeds reserved size
			auto evicted = evict (bucket);
			return evicted != hash; // Added if not evicted
		}
		else
		{
			// Otherwise let the container overflow
		}
	}

	return true; // Added
}

nano::block_hash priority_pool::evict (nano::bucket_index bucket)
{
	auto & bucket_priority_index = pool.get<tag_bucket_priority> ();
	auto range = bucket_priority_index.equal_range (bucket);

	if (range.first != range.second)
	{
		// Evict the worst priority block (highest priority value)
		auto it = --range.second;
		auto hash = it->hash;

		bucket_priority_index.erase (it);
		bucket_sizes[bucket]--;

		return hash;
	}

	return nano::block_hash{ 0 }; // Nothing evicted
}

std::optional<priority_pool::priority_result> priority_pool::top (nano::bucket_index bucket) const
{
	auto & bucket_priority_index = pool.get<tag_bucket_priority> ();
	auto range = bucket_priority_index.equal_range (bucket);

	if (range.first != range.second)
	{
		auto const & entry = *range.first;
		return priority_result{ entry.block, entry.bucket, entry.priority };
	}

	return std::nullopt;
}

std::map<nano::bucket_index, priority_pool::priority_result> priority_pool::top_all () const
{
	std::map<nano::bucket_index, priority_result> result;

	for (auto const & [bucket, size] : bucket_sizes)
	{
		if (size > 0)
		{
			auto entry = top (bucket);
			if (entry)
			{
				result[bucket] = *entry;
			}
		}
	}

	return result;
}

std::optional<priority_pool::priority_result> priority_pool::pop (nano::bucket_index bucket)
{
	auto & bucket_priority_index = pool.get<tag_bucket_priority> ();
	auto range = bucket_priority_index.equal_range (bucket);

	if (range.first != range.second)
	{
		auto const & entry = *range.first;
		priority_result result{ entry.block, entry.bucket, entry.priority };

		bucket_priority_index.erase (range.first);
		bucket_sizes[bucket]--;

		return result;
	}

	return std::nullopt;
}

bool priority_pool::erase (nano::block_hash const & hash)
{
	auto & hash_index = pool.get<tag_hash> ();
	if (auto it = hash_index.find (hash); it != hash_index.end ())
	{
		auto bucket = it->bucket;
		hash_index.erase (it);
		bucket_sizes[bucket]--;
		return true;
	}
	return false;
}

bool priority_pool::contains (nano::block_hash const & hash) const
{
	auto const & hash_index = pool.get<tag_hash> ();
	return hash_index.find (hash) != hash_index.end ();
}

bool priority_pool::any (nano::bucket_index bucket) const
{
	auto it = bucket_sizes.find (bucket);
	return it != bucket_sizes.end () && it->second > 0;
}

size_t priority_pool::size () const
{
	return pool.size ();
}

size_t priority_pool::size (nano::bucket_index bucket) const
{
	auto it = bucket_sizes.find (bucket);
	return it != bucket_sizes.end () ? it->second : 0;
}

bool priority_pool::empty () const
{
	return pool.empty ();
}

bool priority_pool::empty (nano::bucket_index bucket) const
{
	return size (bucket) == 0;
}

void priority_pool::clear ()
{
	pool.clear ();
	bucket_sizes.clear ();
}

nano::container_info priority_pool::container_info () const
{
	nano::container_info info;
	info.put ("pool", pool.size ());
	info.put ("buckets", bucket_sizes.size ());

	nano::container_info bucket_info;
	for (auto const & [bucket, size] : bucket_sizes)
	{
		bucket_info.put (std::string ("bucket_") + std::to_string (bucket), size);
	}
	info.add ("buckets", bucket_info);

	return info;
}
}