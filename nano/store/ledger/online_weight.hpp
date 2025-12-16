#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/reverse_iterator.hpp>
#include <nano/store/reverse_iterator_templ.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

namespace nano::store::ledger
{
class online_weight_view
{
public:
	using iterator = store::typed_iterator<uint64_t, nano::amount>;
	using reverse_iterator = store::reverse_iterator<iterator>;

public:
	explicit online_weight_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, uint64_t time, nano::amount const &);
	void del (nano::store::write_transaction const &, uint64_t time);
	iterator begin (nano::store::transaction const &) const;
	reverse_iterator rbegin (nano::store::transaction const &) const;
	reverse_iterator rend (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	size_t count (nano::store::transaction const &) const;
	void clear (nano::store::write_transaction const &);

private:
	nano::store::backend & backend;
};
}
