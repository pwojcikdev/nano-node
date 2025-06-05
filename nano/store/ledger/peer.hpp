#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

namespace nano::store::ledger
{
class peer
{
public:
	using iterator = store::typed_iterator<nano::endpoint_key, nano::millis_t>;

public:
	explicit peer (store::backend &);

	void put (store::write_transaction const & tx, nano::endpoint_key const & endpoint, nano::millis_t timestamp);
	nano::millis_t get (store::transaction const & tx, nano::endpoint_key const & endpoint) const;
	void del (store::write_transaction const & tx, nano::endpoint_key const & endpoint);
	bool exists (store::transaction const & tx, nano::endpoint_key const & endpoint) const;
	size_t count (store::transaction const & tx) const;
	void clear (store::write_transaction const & tx);
	iterator begin (store::transaction const & tx) const;
	iterator end (store::transaction const & tx) const;

private:
	store::backend & backend;
};
}
