#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class pruned_view
{
public:
	using iterator = store::typed_iterator<nano::block_hash, std::nullptr_t>;

public:
	explicit pruned_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, nano::block_hash const &);
	void del (nano::store::write_transaction const &, nano::block_hash const &);
	bool exists (nano::store::transaction const &, nano::block_hash const &) const;
	nano::block_hash random (nano::store::transaction const &) const;
	size_t count (nano::store::transaction const &) const;
	void clear (nano::store::write_transaction const &);
	iterator begin (nano::store::transaction const &, nano::block_hash const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	nano::store::backend & backend;
};
}
