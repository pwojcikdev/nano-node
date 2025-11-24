#include <nano/store/backend.hpp>

namespace nano::store
{
nano::store::column_schema const backend::schema_meta{ { tables::meta, "meta" } };
}

namespace nano::store
{
auto backend::open_meta () -> backend_meta
{
	// Attempt to open just the meta table which should always exist
	open (schema_meta, store::open_mode::read_only);

	backend_meta meta;
	{
		version_store version{ *this };
		auto transaction = tx_begin_read ();
		meta.version = version.get_version (transaction);
	}

	close ();

	return meta;
}
}

namespace nano::store
{
void version_store::put_version (store::write_transaction const & transaction, uint64_t version)
{
	nano::uint256_union db_key{ version_key };
	nano::uint256_union db_value{ version };
	auto status = backend.put (transaction, tables::meta, db_key, db_value);
	backend.release_assert_success (status);
}

auto version_store::get_version (store::transaction const & transaction) const -> uint64_t
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
}