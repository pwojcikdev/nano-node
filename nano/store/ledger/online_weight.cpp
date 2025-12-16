#include <nano/store/ledger/online_weight.hpp>

namespace nano::store::ledger
{
online_weight_view::online_weight_view (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void online_weight_view::put (nano::store::write_transaction const & txn, uint64_t time, nano::amount const & amount)
{
	auto status = backend.put (txn, tables::online_weight, time, amount);
	backend.release_assert_success (status);
}

void online_weight_view::del (nano::store::write_transaction const & txn, uint64_t time)
{
	auto status = backend.del (txn, tables::online_weight, time);
	backend.release_assert_success (status);
}

auto online_weight_view::begin (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.begin (txn, tables::online_weight) };
}

auto online_weight_view::end (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.end (txn, tables::online_weight) };
}

auto online_weight_view::rbegin (nano::store::transaction const & txn) const -> reverse_iterator
{
	return reverse_iterator{ std::prev (end (txn)) };
}

auto online_weight_view::rend (nano::store::transaction const & txn) const -> reverse_iterator
{
	return reverse_iterator{ end (txn) };
}

size_t online_weight_view::count (nano::store::transaction const & txn) const
{
	return backend.count (txn, tables::online_weight);
}

void online_weight_view::clear (nano::store::write_transaction const & txn)
{
	auto status = backend.clear (txn, tables::online_weight);
	backend.release_assert_success (status);
}
}
