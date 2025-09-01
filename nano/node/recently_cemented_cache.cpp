#include <nano/lib/blocks.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/recently_cemented_cache.hpp>

#include <ranges>

nano::recently_cemented_cache::recently_cemented_cache (std::size_t max_size_a) :
	max_size{ max_size_a }
{
}

void nano::recently_cemented_cache::put (const nano::election_status & status)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	entries.emplace_back (entry{ status.winner->qualified_root (), status.winner->hash (), status });
	if (entries.size () > max_size)
	{
		entries.pop_front (); // Remove oldest
	}
}

void nano::recently_cemented_cache::erase (const nano::block_hash & hash)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	entries.get<tag_hash> ().erase (hash);
}

void nano::recently_cemented_cache::clear ()
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	entries.clear ();
}

auto nano::recently_cemented_cache::list (size_t max_count) const -> std::deque<nano::election_status>
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	std::deque<nano::election_status> result;
	auto it = entries.rbegin ();
	for (size_t i = 0; i < max_count && it != entries.rend (); ++i, ++it)
	{
		result.push_back (it->status);
	}
	return result;
}

std::size_t nano::recently_cemented_cache::size () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return entries.size ();
}

bool nano::recently_cemented_cache::contains (const nano::qualified_root & root) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return entries.get<tag_root> ().contains (root);
}

bool nano::recently_cemented_cache::contains (const nano::block_hash & hash) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return entries.get<tag_hash> ().contains (hash);
}

nano::container_info nano::recently_cemented_cache::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("entries", entries);
	return info;
}
