#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>

#include <functional>

namespace nano::store
{
class pruned
{
public:
	using iterator = store::typed_iterator<nano::block_hash, std::nullptr_t>;

public:
	explicit pruned (store::backend &);

	void put (store::write_transaction const & tx, nano::block_hash const & hash);
	void del (store::write_transaction const & tx, nano::block_hash const & hash);
	bool exists (store::transaction const & tx, nano::block_hash const & hash) const;
	nano::block_hash random (store::transaction const & tx);
	size_t count (store::transaction const & tx) const;
	void clear (store::write_transaction const & tx);
	iterator begin (store::transaction const & tx, nano::block_hash const & hash) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
