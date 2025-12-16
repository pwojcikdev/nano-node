#include <nano/store/ledger/online_weight.hpp>

namespace nano::store::ledger
{
online_weight::online_weight (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void online_weight::put (nano::store::write_transaction const & txn, uint64_t time, nano::amount const & amount)
{
	auto status = backend.put (txn, tables::online_weight, time, amount);
	backend.release_assert_success (status);
}

void online_weight::del (nano::store::write_transaction const & txn, uint64_t time)
{
	auto status = backend.del (txn, tables::online_weight, time);
	backend.release_assert_success (status);
}

auto online_weight::begin (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.begin (txn, tables::online_weight) };
}

auto online_weight::end (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.end (txn, tables::online_weight) };
}

auto online_weight::rbegin (nano::store::transaction const & txn) const -> reverse_iterator
{
	return reverse_iterator{ std::prev (end (txn)) };
}

auto online_weight::rend (nano::store::transaction const & txn) const -> reverse_iterator
{
	return reverse_iterator{ end (txn) };
}

size_t online_weight::count (nano::store::transaction const & txn) const
{
	return backend.count (txn, tables::online_weight);
}

void online_weight::clear (nano::store::write_transaction const & txn)
{
	auto status = backend.clear (txn, tables::online_weight);
	backend.release_assert_success (status);
}
}
