#include <nano/lib/container_info.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/peer_pool.hpp>
#include <nano/node/transport/channel.hpp>

#include <algorithm>

namespace nano::bootstrap
{
peer_pool::peer_pool (nano::bootstrap_config const & config_a) :
	config{ config_a }
{
}

auto peer_pool::acquire (nano::node_capabilities_flags required, std::span<nano::account const> exclude, nano::transport::traffic_type traffic) -> peer_pool::acquire_result
{
	bool capable_present = false; // Some connected peer satisfies the capability requirement
	bool candidate_present = false; // Some capable peer is not excluded, though it may be at capacity

	auto excluded = [&exclude] (entry const & entry) {
		return std::find (exclude.begin (), exclude.end (), entry.node_id) != exclude.end ();
	};

	auto best = entries.end ();
	for (auto it = entries.begin (); it != entries.end (); ++it)
	{
		auto const & entry = it->second;
		if (!entry.capable (required))
		{
			continue;
		}
		capable_present = true;
		if (excluded (entry))
		{
			continue; // Already used by the requesting round
		}
		candidate_present = true;
		if (config.channel_limit != 0 && entry.outstanding >= config.channel_limit)
		{
			continue; // At capacity, may free up later
		}
		// Check the send queue only when the entry improves on the current best, keeping the scan cheap
		if (best == entries.end () || entry.outstanding < best->second.outstanding)
		{
			if (entry.channel->max (traffic))
			{
				continue; // Send queue full, may free up later
			}
			best = it;
		}
	}

	if (best != entries.end ())
	{
		best->second.outstanding += 1;
		return { best->second.channel, peer_acquire_status::acquired, best->second.node_id };
	}
	if (candidate_present)
	{
		return { nullptr, peer_acquire_status::busy };
	}
	if (capable_present)
	{
		return { nullptr, peer_acquire_status::exhausted };
	}
	return { nullptr, peer_acquire_status::no_peers };
}

void peer_pool::release (std::shared_ptr<nano::transport::channel> const & channel)
{
	if (auto it = entries.find (channel.get ()); it != entries.end ())
	{
		auto & entry = it->second;
		entry.outstanding -= entry.outstanding > 0 ? 1 : 0;
	}
}

bool peer_pool::has_candidate (nano::node_capabilities_flags required, std::span<nano::account const> exclude) const
{
	return std::any_of (entries.begin (), entries.end (), [&] (auto const & item) {
		auto const & entry = item.second;
		return entry.capable (required) && std::find (exclude.begin (), exclude.end (), entry.node_id) == exclude.end ();
	});
}

peer_probe_status peer_pool::probe (nano::node_capabilities_flags required, std::span<nano::account const> exclude) const
{
	bool candidate_present = false;

	for (auto const & [_, entry] : entries)
	{
		if (!entry.capable (required))
		{
			continue;
		}
		if (std::find (exclude.begin (), exclude.end (), entry.node_id) != exclude.end ())
		{
			continue;
		}
		candidate_present = true;
		if (config.channel_limit == 0 || entry.outstanding < config.channel_limit)
		{
			return peer_probe_status::available;
		}
	}

	return candidate_present ? peer_probe_status::busy : peer_probe_status::none;
}

void peer_pool::update (std::deque<std::shared_ptr<nano::transport::channel>> const & channels)
{
	// Track new channels and refresh the cached identity of known ones
	for (auto const & channel : channels)
	{
		if (auto it = entries.find (channel.get ()); it != entries.end ())
		{
			it->second.node_id = channel->get_node_id ();
			it->second.capabilities = channel->get_flags ();
		}
		else
		{
			entries.emplace (channel.get (), entry{ channel, channel->get_node_id (), channel->get_flags () });
		}
	}

	// Drop closed channels
	for (auto it = entries.begin (); it != entries.end ();)
	{
		if (!it->second.channel->alive ())
		{
			it = entries.erase (it);
		}
		else
		{
			++it;
		}
	}
}

void peer_pool::decay ()
{
	for (auto & [channel, entry] : entries)
	{
		entry.outstanding -= entry.outstanding > 0 ? 1 : 0;
	}
}

void peer_pool::reset ()
{
	entries.clear ();
}

std::size_t peer_pool::size () const
{
	return entries.size ();
}

std::size_t peer_pool::available () const
{
	return std::count_if (entries.begin (), entries.end (), [this] (auto const & item) {
		auto const & entry = item.second;
		return config.channel_limit == 0 || entry.outstanding < config.channel_limit;
	});
}

nano::container_info peer_pool::container_info () const
{
	nano::container_info info;
	info.put ("tracked", size ());
	info.put ("available", available ());
	return info;
}
}
