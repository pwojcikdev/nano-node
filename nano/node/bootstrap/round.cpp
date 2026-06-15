#include <nano/lib/assert.hpp>
#include <nano/node/bootstrap/round.hpp>

#include <algorithm>

namespace nano::bootstrap
{
std::span<nano::account const> round::used () const
{
	return { used_m.data (), used_m.size () };
}

std::span<nano::account const> round::exclude () const
{
	return used ();
}

size_t round::launched () const
{
	return launched_m;
}

size_t round::inflight () const
{
	return tag_ids_m.size ();
}

std::chrono::steady_clock::time_point round::last_launch () const
{
	return last_launch_m;
}

void round::conclude (id_t tag_id, nano::bootstrap::conclusion result)
{
	(void)tag_id;
	(void)result;
}

bool round::owns (id_t tag_id) const
{
	return std::find (tag_ids_m.begin (), tag_ids_m.end (), tag_id) != tag_ids_m.end ();
}

bool round::erase (id_t tag_id)
{
	auto const it = std::find (tag_ids_m.begin (), tag_ids_m.end (), tag_id);
	if (it == tag_ids_m.end ())
	{
		return false;
	}
	tag_ids_m.erase (it);
	return true;
}

void round::reserve (nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now)
{
	debug_assert (!owns (tag_id));
	debug_assert (std::find (used_m.begin (), used_m.end (), node_id) == used_m.end ());

	used_m.push_back (node_id);
	tag_ids_m.push_back (tag_id);
	++launched_m;
	last_launch_m = now;
}
}
