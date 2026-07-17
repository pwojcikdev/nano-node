#include <nano/lib/container_info.hpp>
#include <nano/lib/saturate.hpp>
#include <nano/node/bootstrap/topo_skip_policy.hpp>

namespace nano::bootstrap
{
topo_skip_policy::topo_skip_policy (nano::topo_scan_config const & config_a) :
	config{ config_a }
{
	reset ();
}

bool topo_skip_policy::observe (bool needs_fetch)
{
	if (needs_fetch)
	{
		add_history (true);
		redundant_pages_m = 0;
		threshold_m = next_threshold ();
		return false;
	}

	if (config.redundant_skip_threshold == 0)
	{
		add_history (false);
		redundant_pages_m = 0;
		threshold_m = 0;
		return false;
	}

	// Snapshot the recent pressure before this streak starts. Subsequent redundant pages may age old samples
	// out of the history, but the evidence target for this streak remains stable.
	if (redundant_pages_m == 0)
	{
		threshold_m = next_threshold ();
	}

	add_history (false);
	++redundant_pages_m;

	if (redundant_pages_m >= threshold_m)
	{
		redundant_pages_m = 0;
		return true;
	}

	return false;
}

void topo_skip_policy::reset ()
{
	history.clear ();
	recent_need_fetch_pages_m = 0;
	redundant_pages_m = 0;
	threshold_m = config.redundant_skip_threshold;
}

std::size_t topo_skip_policy::threshold () const
{
	return threshold_m;
}

nano::container_info topo_skip_policy::container_info () const
{
	nano::container_info info;
	info.put ("history", history.size ());
	info.put ("need_fetch_pages", recent_need_fetch_pages_m);
	info.put ("redundant_pages", redundant_pages_m);
	info.put ("threshold", threshold_m);
	return info;
}

void topo_skip_policy::add_history (bool needs_fetch)
{
	if (config.redundant_skip_history_size == 0)
	{
		return;
	}

	if (history.size () >= config.redundant_skip_history_size)
	{
		if (history.front ())
		{
			--recent_need_fetch_pages_m;
		}
		history.pop_front ();
	}

	history.push_back (needs_fetch);
	if (needs_fetch)
	{
		++recent_need_fetch_pages_m;
	}
}

std::size_t topo_skip_policy::next_threshold () const
{
	if (config.redundant_skip_threshold == 0)
	{
		return 0;
	}
	return nano::add_sat (config.redundant_skip_threshold, recent_need_fetch_pages_m);
}
}
