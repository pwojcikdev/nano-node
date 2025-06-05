#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/reverse_iterator.hpp>
#include <nano/store/typed_iterator.hpp>

namespace nano::store
{
class online_weight
{
public:
	using iterator = store::typed_iterator<uint64_t, nano::amount>;
	using reverse_iterator = store::reverse_iterator<iterator>;

public:
	explicit online_weight (store::backend &);

	void put (store::write_transaction const & tx, uint64_t time, nano::amount const & weight);
	void del (store::write_transaction const & tx, uint64_t time);
	iterator begin (store::transaction const & tx) const;
	reverse_iterator rbegin (store::transaction const & tx) const;
	reverse_iterator rend (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;
	size_t count (store::transaction const & tx) const;
	void clear (store::write_transaction const & tx);

private:
	store::backend & backend;
};
}
