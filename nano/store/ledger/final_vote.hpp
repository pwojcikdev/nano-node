#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class final_vote
{
public:
	using iterator = store::typed_iterator<nano::qualified_root, nano::block_hash>;

public:
	explicit final_vote (store::backend &);

	bool put (store::write_transaction const & tx, nano::qualified_root const & root, nano::block_hash const & hash);
	std::optional<nano::block_hash> get (store::transaction const & tx, nano::qualified_root const & root) const;
	void del (store::write_transaction const & tx, nano::qualified_root const & root);
	size_t count (store::transaction const & tx) const;
	bool empty (store::transaction const & tx) const;
	void clear (store::write_transaction const & tx);
	iterator begin (store::transaction const & tx, nano::qualified_root const & root) const;
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	void for_each_par (std::function<void (store::read_transaction const & tx, iterator, iterator)> const & action) const;

private:
	store::backend & backend;
};
}
