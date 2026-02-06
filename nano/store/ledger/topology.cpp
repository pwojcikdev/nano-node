#include <nano/store/backend.hpp>
#include <nano/store/ledger/topology.hpp>

namespace nano::store::ledger
{
topology_view::topology_view (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void topology_view::put (nano::store::write_transaction const & txn, uint64_t index, nano::block_hash const & hash)
{
	auto status = backend.put (txn, nano::store::table::topology, nano::topology_key{ index, hash }, nullptr);
	backend.release_assert_success (status);
}

void topology_view::del (nano::store::write_transaction const & txn, uint64_t index, nano::block_hash const & hash)
{
	auto status = backend.del (txn, nano::store::table::topology, nano::topology_key{ index, hash });
	backend.release_assert_success (status);
}

bool topology_view::exists (nano::store::transaction const & txn, uint64_t index, nano::block_hash const & hash) const
{
	return backend.exists (txn, nano::store::table::topology, nano::topology_key{ index, hash });
}

std::optional<uint64_t> topology_view::latest (nano::store::transaction const & txn) const
{
	auto first = begin (txn);
	auto i = end (txn);
	if (i == first)
	{
		return std::nullopt;
	}
	--i;
	return i->first.index;
}

uint64_t topology_view::count (nano::store::transaction const & txn) const
{
	return backend.count (txn, nano::store::table::topology);
}

void topology_view::clear ()
{
	auto status = backend.clear (nano::store::table::topology);
	backend.release_assert_success (status);
}

auto topology_view::begin (nano::store::transaction const & txn, uint64_t index) const -> iterator
{
	return iterator{ backend.begin (txn, nano::store::table::topology, nano::topology_key{ index, 0 }) };
}

auto topology_view::begin (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.begin (txn, nano::store::table::topology) };
}

auto topology_view::end (nano::store::transaction const & txn) const -> iterator
{
	return iterator{ backend.end (txn, nano::store::table::topology) };
}
}
