#include <nano/store/ledger/peer.hpp>

namespace nano::store::ledger
{
peer::peer (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void peer::put (nano::store::write_transaction const & transaction, nano::endpoint_key const & endpoint, nano::millis_t timestamp)
{
	auto status = backend.put (transaction, tables::peers, endpoint, timestamp);
	backend.release_assert_success (status);
}

nano::millis_t peer::get (nano::store::transaction const & transaction, nano::endpoint_key const & endpoint) const
{
	nano::millis_t result{ 0 };
	db_val value;
	auto status = backend.get (transaction, tables::peers, endpoint, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	if (backend.success (status) && value.size () > 0)
	{
		result = static_cast<nano::millis_t> (value);
	}
	return result;
}

void peer::del (nano::store::write_transaction const & transaction, nano::endpoint_key const & endpoint)
{
	auto status = backend.del (transaction, tables::peers, endpoint);
	backend.release_assert_success (status);
}

bool peer::exists (nano::store::transaction const & transaction, nano::endpoint_key const & endpoint) const
{
	return backend.exists (transaction, tables::peers, endpoint);
}

size_t peer::count (nano::store::transaction const & transaction) const
{
	return backend.count (transaction, tables::peers);
}

void peer::clear (nano::store::write_transaction const & transaction)
{
	auto status = backend.clear (transaction, tables::peers);
	backend.release_assert_success (status);
}

auto peer::begin (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::peers) };
}

auto peer::end (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::peers) };
}
}