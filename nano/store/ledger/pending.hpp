#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/pending_info.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class pending_view
{
public:
	using iterator = store::typed_iterator<nano::pending_key, nano::pending_info>;

public:
	explicit pending_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, nano::pending_key const &, nano::pending_info const &);
	void del (nano::store::write_transaction const &, nano::pending_key const &);
	std::optional<nano::pending_info> get (nano::store::transaction const &, nano::pending_key const &) const;
	bool exists (nano::store::transaction const &, nano::pending_key const &) const;
	bool any (nano::store::transaction const &, nano::account const &) const;
	iterator begin (nano::store::transaction const &, nano::pending_key const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	nano::store::backend & backend;
};
}
