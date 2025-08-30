#include <nano/node/active_elections_index.hpp>
#include <nano/node/election.hpp>

#include <ranges>

void nano::active_elections_index::insert (std::shared_ptr<nano::election> const & election, nano::election_behavior behavior, nano::bucket_index bucket, nano::priority_timestamp priority)
{
	debug_assert (!entries.get<tag_ptr> ().contains (election));
	debug_assert (!entries.get<tag_root> ().contains (election->qualified_root));

	auto [it, inserted] = entries.emplace_back (entry{ election, election->qualified_root, behavior, bucket, priority });
	debug_assert (inserted);

	// Update cached size
	size_by_behavior[{ behavior }]++;
	size_by_bucket[{ behavior, bucket }]++;
}

bool nano::active_elections_index::erase (std::shared_ptr<nano::election> const & election)
{
	auto maybe_entry = info (election);
	if (!maybe_entry)
	{
		return false; // Not found
	}
	auto entry = *maybe_entry;

	auto & index = entries.get<tag_ptr> ();
	auto erased = index.erase (election);
	release_assert (erased == 1);

	// Update cached size
	size_by_behavior[{ entry.behavior }]--;
	size_by_bucket[{ entry.behavior, entry.bucket }]--;

	return true; // Erased
}

void nano::active_elections_index::update (std::shared_ptr<nano::election> const & election, nano::election_behavior behavior)
{
	auto & index = entries.get<tag_ptr> ();
	auto existing = index.find (election);
	if (existing != index.end ())
	{
		auto old_behavior = existing->behavior;
		auto bucket = existing->bucket;

		if (old_behavior != behavior)
		{
			// Update cached sizes
			size_by_behavior[{ old_behavior }]--;
			size_by_bucket[{ old_behavior, bucket }]--;
			size_by_behavior[{ behavior }]++;
			size_by_bucket[{ behavior, bucket }]++;

			// Update the entry
			index.modify (existing, [behavior] (entry & e) {
				e.behavior = behavior;
			});
		}
	}
	else
	{
		debug_assert (false, "election not found in index");
	}
}

bool nano::active_elections_index::exists (nano::qualified_root const & root) const
{
	return entries.get<tag_root> ().contains (root);
}

bool nano::active_elections_index::exists (std::shared_ptr<nano::election> const & election) const
{
	return entries.get<tag_ptr> ().contains (election);
}

std::shared_ptr<nano::election> nano::active_elections_index::election (nano::qualified_root const & root) const
{
	if (auto existing = entries.get<tag_root> ().find (root); existing != entries.get<tag_root> ().end ())
	{
		return existing->election;
	}
	return nullptr;
}

auto nano::active_elections_index::info (std::shared_ptr<nano::election> const & election) const -> std::optional<entry>
{
	if (auto existing = entries.get<tag_ptr> ().find (election); existing != entries.get<tag_ptr> ().end ())
	{
		return *existing;
	}
	return std::nullopt;
}

auto nano::active_elections_index::list () const -> std::deque<entry>
{
	auto r = entries.get<tag_sequenced> () | std::views::transform ([] (auto const & entry) { return entry; });
	return { r.begin (), r.end () };
}

size_t nano::active_elections_index::size () const
{
	return entries.size ();
}

size_t nano::active_elections_index::size (nano::election_behavior behavior) const
{
	if (auto existing = size_by_behavior.find ({ behavior }); existing != size_by_behavior.end ())
	{
		return existing->second;
	}
	return 0;
}

size_t nano::active_elections_index::size (nano::election_behavior behavior, nano::bucket_index bucket) const
{
	if (auto existing = size_by_bucket.find ({ behavior, bucket }); existing != size_by_bucket.end ())
	{
		return existing->second;
	}
	return 0;
}

auto nano::active_elections_index::highest (nano::election_behavior behavior, nano::bucket_index bucket) const -> priority_result
{
	auto & index = entries.get<tag_key> ();

	// Returns an iterator pointing to the first element with key greater than x
	auto existing = index.upper_bound (key{ behavior, bucket, std::numeric_limits<nano::priority_timestamp>::max () });
	if (existing != index.begin ())
	{
		--existing;
		if (existing->behavior == behavior && existing->bucket == bucket)
		{
			return { existing->election, existing->priority };
		}
	}

	return { nullptr, std::numeric_limits<nano::priority_timestamp>::max () };
}

auto nano::active_elections_index::list (std::chrono::steady_clock::time_point cutoff, std::chrono::steady_clock::time_point now) -> std::deque<std::shared_ptr<nano::election>>
{
	auto & index = entries.get<tag_timestamp> ();

	// Collect entries to process first to avoid iterator invalidation issues
	std::deque<decltype (index.begin ())> to_process;
	auto end = index.upper_bound (cutoff);
	for (auto it = index.begin (); it != end; ++it)
	{
		to_process.push_back (it);
	}

	// Process and update timestamps
	for (auto it : to_process)
	{
		// Update timestamp to 'now' for processed entries
		index.modify (it, [now] (entry & e) {
			e.timestamp = now;
		});
	}

	auto r = to_process | std::views::transform ([] (auto const & it) { return it->election; });
	return { r.begin (), r.end () };
}

bool nano::active_elections_index::trigger (std::shared_ptr<nano::election> const & election)
{
	auto & index = entries.get<tag_ptr> ();
	if (auto existing = index.find (election); existing != index.end ())
	{
		index.modify (existing, [] (entry & e) {
			e.timestamp = {};
		});
		return true;
	}
	return false; // Not found
}

bool nano::active_elections_index::any (std::chrono::steady_clock::time_point cutoff) const
{
	auto & index = entries.get<tag_timestamp> ();
	auto it = index.begin ();
	return it != index.end () && it->timestamp <= cutoff;
}

void nano::active_elections_index::clear ()
{
	entries.clear ();
	size_by_behavior.clear ();
	size_by_bucket.clear ();
}