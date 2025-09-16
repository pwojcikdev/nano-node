#include <nano/lib/blocks.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/unchecked_map.hpp>

nano::unchecked_map::unchecked_map (unsigned max_size, nano::stats & stats, bool disable_delete) :
	max_size{ max_size },
	stats{ stats },
	disable_delete{ disable_delete }
{
}

nano::unchecked_map::~unchecked_map ()
{
	debug_assert (!thread.joinable ());
}

void nano::unchecked_map::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread ([this] () {
		nano::thread_role::set (nano::thread_role::name::unchecked);
		run ();
	});
}

void nano::unchecked_map::stop ()
{
	{
		std::lock_guard lock{ mutex };
		stopped = true;
	}
	condition.notify_all ();

	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::unchecked_map::put (nano::hash_or_account const & dependency, nano::unchecked_info const & info)
{
	std::unique_lock lock{ mutex };

	stats.inc (nano::stat::type::unchecked, nano::stat::detail::put);
	nano::unchecked_key key{ dependency, info.block->hash () };
	entries.get<tag_root> ().insert ({ key, info });
	if (entries.size () > max_size)
	{
		entries.get<tag_sequenced> ().pop_front ();
	}

	// Check if this dependency was previously triggered
	bool trigger = false;
	if (recently_triggered.get<tag_hash> ().contains (dependency.hash) > 0)
	{
		// This block was triggered before it was inserted, so trigger it now
		triggered.emplace_back (dependency);
		trigger = true;
	}

	lock.unlock ();

	if (trigger)
	{
		condition.notify_all ();
	}
}

void nano::unchecked_map::put_many (std::deque<std::pair<nano::hash_or_account, nano::unchecked_info>> const & batch)
{
	std::unique_lock lock{ mutex };

	stats.add (nano::stat::type::unchecked, nano::stat::detail::put, batch.size ());
	for (auto const & [dependency, info] : batch)
	{
		nano::unchecked_key key{ dependency, info.block->hash () };
		entries.get<tag_root> ().insert ({ key, info });
		if (entries.size () > max_size)
		{
			entries.get<tag_sequenced> ().pop_front ();
		}

		if (recently_triggered.get<tag_hash> ().contains (dependency.hash) > 0)
		{
			triggered.emplace_back (dependency);
		}
	}

	lock.unlock ();
	condition.notify_all ();
}

void nano::unchecked_map::for_each (std::function<void (nano::unchecked_key const &, nano::unchecked_info const &)> action, std::function<bool ()> predicate)
{
	std::lock_guard lock{ mutex };
	for (auto i = entries.begin (), n = entries.end (); predicate () && i != n; ++i)
	{
		action (i->key, i->info);
	}
}

void nano::unchecked_map::for_each (nano::hash_or_account const & dependency, std::function<void (nano::unchecked_key const &, nano::unchecked_info const &)> action, std::function<bool ()> predicate)
{
	std::lock_guard lock{ mutex };
	for (auto i = entries.template get<tag_root> ().lower_bound (nano::unchecked_key{ dependency, 0 }), n = entries.template get<tag_root> ().end (); predicate () && i != n && i->key.key () == dependency.as_block_hash (); ++i)
	{
		action (i->key, i->info);
	}
}

std::vector<nano::unchecked_info> nano::unchecked_map::get (nano::block_hash const & hash)
{
	std::vector<nano::unchecked_info> result;
	for_each (hash, [&result] (nano::unchecked_key const & key, nano::unchecked_info const & info) {
		result.push_back (info);
	});
	return result;
}

bool nano::unchecked_map::exists (nano::unchecked_key const & key) const
{
	std::lock_guard lock{ mutex };
	return entries.get<tag_root> ().count (key) != 0;
}

void nano::unchecked_map::del (nano::unchecked_key const & key)
{
	std::lock_guard lock{ mutex };
	auto erased = entries.get<tag_root> ().erase (key);
	debug_assert (erased);
}

void nano::unchecked_map::clear ()
{
	std::lock_guard lock{ mutex };
	entries.clear ();
}

size_t nano::unchecked_map::entries_size () const
{
	std::lock_guard lock{ mutex };
	return entries.size ();
}

size_t nano::unchecked_map::triggered_size () const
{
	std::lock_guard lock{ mutex };
	return triggered.size ();
}

size_t nano::unchecked_map::count () const
{
	return entries_size ();
}

void nano::unchecked_map::trigger (nano::hash_or_account const & dependency)
{
	std::lock_guard lock{ mutex };

	// Add to triggered cache
	if (recently_triggered.size () >= max_triggered_entries)
	{
		recently_triggered.pop_front ();
	}
	recently_triggered.emplace_back (dependency.hash);

	// Add to triggered queue
	triggered.emplace_back (dependency);

	stats.inc (nano::stat::type::unchecked, nano::stat::detail::trigger);
	condition.notify_all ();
}

void nano::unchecked_map::trigger_many (std::deque<nano::hash_or_account> const & batch)
{
	std::lock_guard lock{ mutex };

	for (auto const & dependency : batch)
	{
		// Add to triggered cache
		if (recently_triggered.size () >= max_triggered_entries)
		{
			recently_triggered.pop_front ();
		}
		recently_triggered.emplace_back (dependency.hash);

		// Add to triggered queue
		triggered.emplace_back (dependency);
	}

	stats.add (nano::stat::type::unchecked, nano::stat::detail::trigger, batch.size ());
	condition.notify_all ();
}

void nano::unchecked_map::run ()
{
	std::unique_lock lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] {
			return stopped || !triggered.empty ();
		});
		while (!triggered.empty () && !stopped)
		{
			auto item = triggered.front ();
			triggered.pop_front ();

			lock.unlock ();
			trigger_impl (item.hash);
			lock.lock ();
		}
	}
}

void nano::unchecked_map::trigger_impl (nano::block_hash const & hash)
{
	std::deque<std::pair<nano::unchecked_key, nano::unchecked_info>> to_notify;
	{
		std::lock_guard lock{ mutex };
		for_each (hash, [&to_notify] (nano::unchecked_key const & key, nano::unchecked_info const & info) {
			to_notify.emplace_back (key, info);
		});
	}

	if (!to_notify.empty ())
	{
		// Remove from triggered cache since we found entries
		// {
		// 	std::lock_guard lock{ mutex };
		// 	recently_triggered.get<tag_hash> ().erase (hash);
		// }

		// Notify outside of locks
		for (auto const & [key, info] : to_notify)
		{
			stats.inc (nano::stat::type::unchecked, nano::stat::detail::satisfied);
			satisfied.notify (info);
		}

		// Delete entries if needed
		// if (!disable_delete)
		// {
		// 	nano::lock_guard<std::recursive_mutex> lock{ entries_mutex };
		// 	for (auto const & [key, info] : to_notify)
		// 	{
		// 		del (key);
		// 	}
		// }
	}
	else
	{
		// Check if in triggered cache - if yes, requeue
		// std::lock_guard lock{ mutex };
		// if (recently_triggered.get<tag_hash> ().count (hash) > 0)
		// {
		// 	// Re-queue for later processing
		// 	triggered.emplace_back (hash);
		// }
	}
}

nano::container_info nano::unchecked_map::container_info () const
{
	std::lock_guard lock{ mutex };

	nano::container_info info;
	info.put ("entries", entries.size ());
	info.put ("triggered", triggered.size ());
	info.put ("recently_triggered", recently_triggered.size ());
	return info;
}
