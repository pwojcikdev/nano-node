#include <nano/store/ledger/online_weight.hpp>

namespace nano::store::ledger
{
online_weight::online_weight (store::backend & backend_a) :
	backend{ backend_a }
{
}

void online_weight::put (store::write_transaction const & transaction, uint64_t time, nano::amount const & amount)
{
	auto status = backend.put (transaction, tables::online_weight, time, amount);
	backend.release_assert_success (status);
}

void online_weight::del (store::write_transaction const & transaction, uint64_t time)
{
	auto status = backend.del (transaction, tables::online_weight, time);
	backend.release_assert_success (status);
}

auto online_weight::begin (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::online_weight) };
}

auto online_weight::end (store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::online_weight) };
}

size_t online_weight::count (store::transaction const & transaction) const
{
	return backend.count (transaction, tables::online_weight);
}

void online_weight::clear (store::write_transaction const & transaction)
{
	auto status = backend.drop (transaction, tables::online_weight);
	backend.release_assert_success (status);
}
}