#include <nano/store/db_val_templ.hpp>
#include <nano/store/ledger/version.hpp>

namespace nano::store::ledger
{
version_view::version_view (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void version_view::put (nano::store::write_transaction const & txn, uint64_t version)
{
	nano::uint256_union version_key{ 1 };
	nano::uint256_union version_value{ version };
	auto status = backend.put (txn, nano::store::table::meta, version_key, version_value);
	backend.release_assert_success (status);
}

uint64_t version_view::get (nano::store::transaction const & txn) const
{
	nano::uint256_union version_key{ 1 };
	nano::store::db_val data;
	auto status = backend.get (txn, nano::store::table::meta, version_key, data);
	uint64_t result = 0; // Default minimum version
	if (backend.success (status))
	{
		nano::uint256_union version_value{ data };
		debug_assert (version_value.qwords[2] == 0 && version_value.qwords[1] == 0 && version_value.qwords[0] == 0);
		result = version_value.number ().convert_to<int> ();
	}
	return result;
}

void version_view::put_topo_enabled (nano::store::write_transaction const & txn, bool enabled)
{
	nano::uint256_union topo_key{ 2 };
	nano::uint256_union topo_value{ enabled ? uint64_t{ 1 } : uint64_t{ 0 } };
	auto status = backend.put (txn, nano::store::table::meta, topo_key, topo_value);
	backend.release_assert_success (status);
}

bool version_view::get_topo_enabled (nano::store::transaction const & txn) const
{
	nano::uint256_union topo_key{ 2 };
	nano::store::db_val data;
	auto status = backend.get (txn, nano::store::table::meta, topo_key, data);
	if (backend.success (status))
	{
		nano::uint256_union topo_value{ data };
		return topo_value.number () != 0;
	}
	return false;
}
}
