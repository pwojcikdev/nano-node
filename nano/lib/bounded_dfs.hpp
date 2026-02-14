#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <type_traits>

namespace nano
{
struct bounded_dfs_result
{
	size_t resolved{ 0 }; // Number of nodes newly resolved in this call
	bool overflow{ false }; // True if stack exceeded max_depth (partial progress, needs more passes)
};

/**
 * Bounded iterative DFS that resolves a node and all its transitive dependencies
 *
 * Starting from `start`, walks dependencies depth-first. For each node:
 *   1. Call get_dependencies(node) to obtain the node's deps (called once per stack entry)
 *   2. Push all unresolved dependencies and go deeper
 *   3. When all deps are resolved, call resolve(node) to process the node
 *
 * Returns a result with the count of resolved nodes and whether the stack overflowed.
 * On overflow, partial progress is preserved -- the caller's outer loop picks up remaining work.
 *
 * Precondition: The dependency graph must be acyclic
 *
 * Callbacks:
 *   is_resolved(Node const &) -> bool
 *   get_dependencies(Node const &) -> iterable of Node
 *   resolve(Node const &) -> bool
 *     -- OR --
 *   resolve(Node const &, deps_type const &) -> bool  (receives cached deps from get_dependencies)
 */
template <typename Node, typename IsResolved, typename GetDeps, typename Resolve>
bounded_dfs_result bounded_dfs (
Node const & start,
size_t max_depth,
IsResolved && is_resolved,
GetDeps && get_dependencies,
Resolve && resolve)
{
	using deps_type = std::decay_t<decltype (get_dependencies (start))>;

	struct entry
	{
		Node node;
		deps_type deps{};
		bool loaded{ false };
	};

	bounded_dfs_result result{};

	std::deque<entry> stack;
	stack.push_back ({ start });

	while (!stack.empty ())
	{
		auto current = stack.back ();

		if (!current.loaded)
		{
			current.loaded = true;
			current.deps = get_dependencies (current.node);
			stack.back () = current; // Persist loaded state back to stack
		}

		// Push all unresolved dependencies
		bool pushed = false;
		for (auto const & dep : current.deps)
		{
			if (!is_resolved (dep))
			{
				if (stack.size () >= max_depth)
				{
					result.overflow = true;
					stack.pop_front ();
				}
				stack.push_back ({ dep });
				pushed = true;
			}
		}

		if (!pushed)
		{
			// All dependencies resolved, process the current node
			if (!is_resolved (current.node))
			{
				bool keep_going;
				if constexpr (std::is_invocable_r_v<bool, Resolve, Node const &, deps_type const &>)
				{
					keep_going = resolve (current.node, current.deps);
				}
				else
				{
					keep_going = resolve (current.node);
				}
				++result.resolved;
				if (!keep_going)
				{
					// Early return requested (needs more passes to finish)
					result.overflow = true;
					stack.pop_back ();
					break;
				}
			}
			stack.pop_back ();
		}
	}

	return result;
}
}
