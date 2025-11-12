#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/component.hpp>
#include <nano/store/typed_iterator.hpp>

#include <deque>
#include <functional>
#include <memory>

namespace nano
{
class block_hash;
class vote;
}

namespace nano::store
{
/**
 * Manages vote storage for rebroadcasting
 */
class vote_storage
{
public:
	using iterator = typed_iterator<nano::vote_storage_key, std::shared_ptr<nano::vote>>;

public:
	/**
	 * Store a vote in the database
	 * @return The number of votes actually inserted (0 if duplicate or replaced by final vote)
	 */
	virtual std::size_t put (store::write_transaction const & transaction_a, std::shared_ptr<nano::vote> const & vote_a) = 0;

	/**
	 * Get all votes for a specific block hash
	 */
	virtual std::deque<std::shared_ptr<nano::vote>> get (store::transaction const & transaction_a, nano::block_hash const & hash_a) = 0;

	/**
	 * Get vote count
	 */
	virtual size_t count (store::transaction const & transaction_a) const = 0;

	/**
	 * Clear all votes
	 */
	virtual void clear (store::write_transaction const &) = 0;

	/**
	 * Iterate votes starting from a specific key
	 */
	virtual iterator begin (store::transaction const & transaction_a, nano::vote_storage_key const & key_a) const = 0;

	/**
	 * Iterate all votes from the beginning
	 */
	virtual iterator begin (store::transaction const & transaction_a) const = 0;

	/**
	 * End iterator
	 */
	virtual iterator end (store::transaction const & transaction_a) const = 0;
};
} // namespace nano::store
