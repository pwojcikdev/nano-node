#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>

#include <functional>

namespace nano::store::ledger
{
class rep_weight
{
public:
	using iterator = store::typed_iterator<nano::account, nano::uint128_union>;

public:
	explicit rep_weight (store::backend &);

	uint64_t count (store::transaction const & tx) const;
	nano::uint128_t get (store::transaction const & tx, nano::account const & representative) const;
	void put (store::write_transaction const & tx, nano::account const & representative, nano::uint128_t const & weight);
	void del (store::write_transaction const & tx, nano::account const & representative);
	iterator begin (store::transaction const & tx, nano::account const & representative) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
