#pragma once

#include <functional>

namespace nano
{
// TODO: Move to store::tables
// Keep this in alphabetical order
enum class tables
{
	accounts,
	blocks,
	confirmation_height,
	default_unused, // RocksDB only
	final_votes,
	meta,
	online_weight,
	peers,
	pending,
	pruned,
	vote,
	rep_weights,
	unchecked, // dropped in v22
	frontiers, // dropped in v24
};
}

namespace std
{
template <>
struct hash<::nano::tables>
{
	size_t operator() (::nano::tables const & table_a) const
	{
		return static_cast<size_t> (table_a);
	}
}; // struct hash
}
