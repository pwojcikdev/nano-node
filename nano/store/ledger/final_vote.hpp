#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <functional>

namespace nano::store::ledger
{
class final_vote_view
{
public:
	using iterator = store::typed_iterator<nano::qualified_root, nano::block_hash>;

public:
	explicit final_vote_view (nano::store::backend &);

	bool put (nano::store::write_transaction const &, nano::qualified_root const &, nano::block_hash const &);
	std::optional<nano::block_hash> get (nano::store::transaction const &, nano::qualified_root const &) const;
	void del (nano::store::write_transaction const &, nano::qualified_root const &);
	size_t count (nano::store::transaction const &) const;
	bool empty (nano::store::transaction const &) const;
	void clear (nano::store::write_transaction const &);
	iterator begin (nano::store::transaction const &, nano::qualified_root const &) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	void for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const;

private:
	nano::store::backend & backend;
};
}
