#include <nano/store/ledger/version.hpp>

namespace nano::store::ledger
{
version::version (nano::store::backend & backend_a) :
	backend{ backend_a }
{
}

void version::put (nano::store::write_transaction const & transaction, uint64_t version)
{
	nano::uint256_union version_key{ 1 };
	nano::uint256_union version_value{ version };
	auto status = backend.put (transaction, tables::meta, version_key, version_value);
	backend.release_assert_success (status);
}

uint64_t version::get (nano::store::transaction const & transaction) const
{
	nano::uint256_union version_key{ 1 };
	db_val data;
	auto status = backend.get (transaction, tables::meta, version_key, data);
	uint64_t result = 0; // Default minimum version
	if (backend.success (status))
	{
		nano::uint256_union version_value{ data };
		debug_assert (version_value.qwords[2] == 0 && version_value.qwords[1] == 0 && version_value.qwords[0] == 0);
		result = version_value.number ().convert_to<int> ();
	}
	return result;
}
}