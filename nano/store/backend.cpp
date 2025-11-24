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

	load_meta ();
	debug_assert (current_meta.has_value ());

	close ();

	return current_meta.value ();
}

auto backend::open (column_schema schema, nano::store::open_mode mode) -> void
{
	release_assert (!is_open, "backend is already open");

	open_impl (schema, mode);

	load_meta ();
	debug_assert (current_meta.has_value ());

	is_open = true;
}

void backend::close ()
{
	close_impl ();

	current_meta.reset ();
	is_open = false;
}

void backend::load_meta ()
{
	backend_meta meta;
	{
		version_store version{ *this };
		auto transaction = tx_begin_read ();
		meta.version = version.get_version (transaction);
	}
	current_meta = meta;
}

auto backend::get_meta () const -> backend_meta
{
	release_assert (current_meta.has_value (), "meta information has not been loaded");
	return current_meta.value ();
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