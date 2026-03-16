#pragma once

#include <cstdint>

namespace nano
{
enum class node_capabilities : uint64_t
{
	none = 0,
	topo_index = 1ULL << 0,
	vote_storage = 1ULL << 1,
};

inline node_capabilities operator| (node_capabilities a, node_capabilities b)
{
	return static_cast<node_capabilities> (static_cast<uint64_t> (a) | static_cast<uint64_t> (b));
}
inline node_capabilities operator& (node_capabilities a, node_capabilities b)
{
	return static_cast<node_capabilities> (static_cast<uint64_t> (a) & static_cast<uint64_t> (b));
}
inline node_capabilities operator~ (node_capabilities a)
{
	return static_cast<node_capabilities> (~static_cast<uint64_t> (a));
}
}
