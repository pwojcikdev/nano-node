#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/account_info.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/reverse_iterator.hpp>
#include <nano/store/reverse_iterator_templ.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class account
{
public:
	using iterator = store::typed_iterator<nano::account, nano::account_info>;
	using reverse_iterator = store::reverse_iterator<iterator>;

public:
	explicit account (store::backend &);

	void put (store::write_transaction const & tx, nano::account const & account, nano::account_info const & info);
	bool get (store::transaction const & tx, nano::account const & account, nano::account_info & info) const;
	std::optional<nano::account_info> get (store::transaction const & tx, nano::account const & account) const;
	void del (store::write_transaction const & tx, nano::account const & account);
	bool exists (store::transaction const & tx, nano::account const & account) const;
	size_t count (store::transaction const & tx) const;
	iterator begin (store::transaction const & tx, nano::account const & account) const;
	iterator begin (store::transaction const & tx) const;
	reverse_iterator rbegin (store::transaction const & tx) const;
	reverse_iterator rend (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
