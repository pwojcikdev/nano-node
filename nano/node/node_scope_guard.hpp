#pragma once

#include <nano/node/node.hpp>

#include <memory>

namespace nano
{
/**
 * RAII guard to ensure node is properly stopped before shared_ptr destruction.
 * This prevents bad_weak_ptr exceptions when stop() is called from the destructor
 * (where shared_from_this() can no longer work because refcount is 0).
 */
class node_scope_guard
{
public:
	explicit node_scope_guard (std::shared_ptr<nano::node> node_a) :
		node{ std::move (node_a) }
	{
	}

	~node_scope_guard ()
	{
		if (node)
		{
			node->stop ();
		}
	}

	// Non-copyable
	node_scope_guard (node_scope_guard const &) = delete;
	node_scope_guard & operator= (node_scope_guard const &) = delete;

	// Movable
	node_scope_guard (node_scope_guard && other) noexcept :
		node{ std::move (other.node) }
	{
	}

	node_scope_guard & operator= (node_scope_guard && other) noexcept
	{
		if (this != &other)
		{
			node = std::move (other.node);
		}
		return *this;
	}

	nano::node & operator* () const
	{
		return *node;
	}

	nano::node * operator->() const
	{
		return node.get ();
	}

	nano::node * get () const
	{
		return node.get ();
	}

	std::shared_ptr<nano::node> const & shared () const
	{
		return node;
	}

private:
	std::shared_ptr<nano::node> node;
};
}
