#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class confirmation_height
{
public:
	using iterator = store::typed_iterator<nano::account, nano::confirmation_height_info>;

public:
	explicit confirmation_height (store::backend &);

	void put (store::write_transaction const & tx, nano::account const & account, nano::confirmation_height_info const & info);
	bool get (store::transaction const & tx, nano::account const & account, nano::confirmation_height_info & info) const;
	std::optional<nano::confirmation_height_info> get (store::transaction const & tx, nano::account const & account);
	bool exists (store::transaction const & tx, nano::account const & account) const;
	void del (store::write_transaction const & tx, nano::account const & account);
	uint64_t count (store::transaction const & tx) const;
	void clear (store::write_transaction const & tx, nano::account const & account);
	void clear (store::write_transaction const & tx);
	iterator begin (store::transaction const & tx, nano::account const & account) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
