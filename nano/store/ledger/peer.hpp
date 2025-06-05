#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/store/typed_iterator_templ.hpp>

namespace nano::store::ledger
{
class peer_view
{
public:
	using iterator = store::typed_iterator<nano::endpoint_key, nano::millis_t>;

public:
	explicit peer_view (nano::store::backend &);

	void put (nano::store::write_transaction const &, nano::endpoint_key const &, nano::millis_t timestamp);
	nano::millis_t get (nano::store::transaction const &, nano::endpoint_key const &) const;
	void del (nano::store::write_transaction const &, nano::endpoint_key const &);
	bool exists (nano::store::transaction const &, nano::endpoint_key const &) const;
	size_t count (nano::store::transaction const &) const;
	void clear (nano::store::write_transaction const &);
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;

private:
	nano::store::backend & backend;
};
}
