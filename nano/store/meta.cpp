#include <nano/store/backend.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/db_val_templ.hpp>
#include <nano/store/meta.hpp>

namespace nano::store
{
meta_view::meta_view (store::backend & backend_a) :
	backend{ backend_a }
{
}

void meta_view::put_version (store::write_transaction const & transaction, uint64_t version)
{
	nano::uint256_union db_key{ version_key };
	nano::uint256_union db_value{ version };
	auto status = backend.put (transaction, tables::meta, db_key, db_value);
	backend.release_assert_success (status);
}

auto meta_view::get_version (store::transaction const & transaction) const -> uint64_t
{
	nano::uint256_union db_key{ version_key };
	db_val data;
	auto status = backend.get (transaction, tables::meta, db_key, data);
	uint64_t result = 0; // Default minimum version
	if (backend.success (status))
	{
		nano::uint256_union db_value{ data };
		debug_assert (db_value.qwords[2] == 0 && db_value.qwords[1] == 0 && db_value.qwords[0] == 0);
		result = db_value.number ().convert_to<uint64_t> ();
	}
	return result;
}

bool meta_view::version_exists (store::transaction const & transaction) const
{
	nano::uint256_union db_key{ version_key };
	return backend.exists (transaction, tables::meta, db_key);
}
}