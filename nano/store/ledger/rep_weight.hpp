#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class rep_weight_view
{
public:
	using iterator = store::typed_iterator<nano::account, nano::uint128_union>;

public:
	explicit rep_weight_view (nano::store::backend &);

	uint64_t count (nano::store::transaction const &) const;
	nano::uint128_t get (nano::store::transaction const &, nano::account const &) const;
	void put (nano::store::write_transaction const &, nano::account const &, nano::uint128_t const &);
	void del (nano::store::write_transaction const &, nano::account const &);
	iterator begin (nano::store::transaction const &, nano::account const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	nano::store::backend & backend;
};
}
