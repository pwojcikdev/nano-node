#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/pending_info.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>

#include <functional>

namespace nano::store::ledger
{
class pending
{
public:
	using iterator = store::typed_iterator<nano::pending_key, nano::pending_info>;

public:
	explicit pending (store::backend &);

	void put (store::write_transaction const & tx, nano::pending_key const & key, nano::pending_info const & info);
	void del (store::write_transaction const & tx, nano::pending_key const & key);
	std::optional<nano::pending_info> get (store::transaction const & tx, nano::pending_key const & key) const;
	bool exists (store::transaction const & tx, nano::pending_key const & key) const;
	bool any (store::transaction const & tx, nano::account const & account) const;
	iterator begin (store::transaction const & tx, nano::pending_key const & key) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
