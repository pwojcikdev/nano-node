#include <nano/lib/container_info.hpp>
#include <nano/lib/enum_util.hpp>
#include <nano/node/bootstrap/lookup_index.hpp>

#include <algorithm>
#include <tuple>

using namespace std::chrono_literals;

namespace
{
// Bound on the wait between lookups, far past any useful setting, that keeps the time arithmetic in range
constexpr std::chrono::milliseconds max_retry_delay = 1h;
}

namespace nano::bootstrap
{
lookup_index::lookup_index (nano::lookup_config const & config_a) :
	config{ config_a }
{
}

auto lookup_index::insert (nano::block_hash const & hash, std::chrono::steady_clock::time_point now) -> insert_result
{
	if (entries.get<tag_hash> ().contains (hash))
	{
		return insert_result::duplicate;
	}

	if (entries.size () >= config.max_blocks)
	{
		return insert_result::overfill;
	}

	entries.insert (entry{ .hash = hash, .due = now });

	return insert_result::inserted;
}

auto lookup_index::next (std::chrono::steady_clock::time_point now) const -> std::optional<request>
{
	// Waiting blocks are ordered by their due time, so the first one has been due for longest
	auto const & by_schedule = entries.get<tag_schedule> ();
	auto const [first, last] = by_schedule.equal_range (std::make_tuple (entry_state::waiting));
	if (first == last)
	{
		return std::nullopt;
	}

	if (first->due > now)
	{
		return std::nullopt;
	}

	return request{ first->hash, { first->tried.begin (), first->tried.end () } };
}

bool lookup_index::dispatch (nano::block_hash const & hash, id_t id, nano::account const & peer)
{
	auto & by_hash = entries.get<tag_hash> ();
	auto const existing = by_hash.find (hash);
	if (existing == by_hash.end ())
	{
		return false;
	}

	if (existing->state != entry_state::waiting)
	{
		return false;
	}

	by_hash.modify (existing, [id, &peer] (entry & item) {
		item.state = entry_state::requested;
		item.id = id;
		item.attempts += 1;
		item.tried.push_back (peer);
	});

	return true;
}

void lookup_index::reset_peers (nano::block_hash const & hash)
{
	auto & by_hash = entries.get<tag_hash> ();
	if (auto existing = by_hash.find (hash); existing != by_hash.end ())
	{
		by_hash.modify (existing, [] (entry & item) {
			item.tried.clear ();
		});
	}
}

auto lookup_index::fail (nano::block_hash const & hash, id_t id, std::chrono::steady_clock::time_point now) -> fail_result
{
	auto & by_hash = entries.get<tag_hash> ();
	auto const existing = by_hash.find (hash);
	if (existing == by_hash.end ())
	{
		return fail_result::stale;
	}

	if (existing->state != entry_state::requested)
	{
		return fail_result::stale;
	}

	// An answer to an earlier lookup of the same block, which is no longer waited on
	if (existing->id != id)
	{
		return fail_result::stale;
	}

	// The first lookup is not a retry, so a block gets one more lookup than it has retries
	if (existing->attempts > config.max_retries)
	{
		by_hash.erase (existing);
		return fail_result::exhausted;
	}

	auto const delay = retry_delay (existing->attempts);
	by_hash.modify (existing, [now, delay] (entry & item) {
		item.state = entry_state::waiting;
		item.due = now + delay;
		item.id = 0;
	});

	return fail_result::retry;
}

bool lookup_index::erase (nano::block_hash const & hash)
{
	return entries.get<tag_hash> ().erase (hash) > 0;
}

void lookup_index::clear ()
{
	entries.clear ();
}

bool lookup_index::contains (nano::block_hash const & hash) const
{
	return entries.get<tag_hash> ().contains (hash);
}

std::size_t lookup_index::size () const
{
	return entries.size ();
}

auto lookup_index::retry_delay (unsigned failures) const -> std::chrono::milliseconds
{
	auto delay = std::clamp (config.retry_delay, std::chrono::milliseconds{ 0 }, max_retry_delay);

	// A factor below two keeps the wait as it is
	if (config.retry_backoff < 2)
	{
		return delay;
	}

	// A zero wait stays zero however often it is multiplied
	if (delay == std::chrono::milliseconds{ 0 })
	{
		return delay;
	}

	// Every failure past the first multiplies the wait, up to the bound
	for (unsigned n = 1; n < failures; ++n)
	{
		if (delay > max_retry_delay / config.retry_backoff)
		{
			return max_retry_delay;
		}

		delay *= config.retry_backoff;
	}

	return delay;
}

nano::container_info lookup_index::container_info () const
{
	auto const & by_schedule = entries.get<tag_schedule> ();

	nano::container_info info;
	info.put ("total", entries.size ());
	info.put ("waiting", by_schedule.count (std::make_tuple (entry_state::waiting)));
	info.put ("requested", by_schedule.count (std::make_tuple (entry_state::requested)));
	return info;
}

/*
 *
 */

nano::stat::detail to_stat_detail (lookup_index::insert_result result)
{
	return nano::enum_convert<nano::stat::detail> (result);
}

nano::stat::detail to_stat_detail (lookup_index::fail_result result)
{
	return nano::enum_convert<nano::stat::detail> (result);
}
}
