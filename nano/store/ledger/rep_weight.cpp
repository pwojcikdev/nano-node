#include <nano/lib/numbers.hpp>
#include <nano/secure/parallel_traversal.hpp>
#include <nano/store/ledger/rep_weight.hpp>

#include <iostream>
#include <stdexcept>

namespace nano::store::ledger
{
rep_weight::rep_weight (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

uint64_t rep_weight::count (nano::store::transaction const & transaction) const
{
	return backend.count (transaction, tables::rep_weights);
}

nano::uint128_t rep_weight::get (nano::store::transaction const & transaction, nano::account const & representative) const
{
	nano::store::db_val value;
	auto status = backend.get (transaction, tables::rep_weights, representative, value);
	release_assert (backend.success (status) || backend.not_found (status), backend.error_string (status));
	nano::uint128_t weight{ 0 };
	if (backend.success (status))
	{
		nano::uint128_union weight_union{ value };
		weight = weight_union.number ();
	}
	return weight;
}

void rep_weight::put (nano::store::write_transaction const & transaction, nano::account const & representative, nano::uint128_t const & weight)
{
	nano::uint128_union weight_union{ weight };
	auto status = backend.put (transaction, tables::rep_weights, representative, weight_union);
	backend.release_assert_success (status);
}

void rep_weight::del (nano::store::write_transaction const & transaction, nano::account const & representative)
{
	auto status = backend.del (transaction, tables::rep_weights, representative);
	backend.release_assert_success (status);
}

auto rep_weight::begin (nano::store::transaction const & transaction, nano::account const & representative) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::rep_weights, representative) };
}

auto rep_weight::begin (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.begin (transaction, tables::rep_weights) };
}

auto rep_weight::end (nano::store::transaction const & transaction) const -> iterator
{
	return iterator{ backend.end (transaction, tables::rep_weights) };
}

void rep_weight::for_each_par (std::function<void (nano::store::read_transaction const &, iterator, iterator)> const & action) const
{
	parallel_traversal<nano::uint256_t> (
	[&action, this] (nano::uint256_t const & start, nano::uint256_t const & end, bool const is_last) {
		auto transaction = this->backend.tx_begin_read ();
		action (transaction, this->begin (transaction, start), !is_last ? this->begin (transaction, end) : this->end (transaction));
	});
}
}