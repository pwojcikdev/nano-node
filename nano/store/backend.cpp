#include <nano/store/backend.hpp>

namespace nano::store
{
nano::store::column_schema const backend::schema_meta{ { tables::meta, "meta" } };

backend::~backend () = default;

auto backend::meta () -> backend_meta
{
	// Attempt to open just the meta table which should always exist
	open (schema_meta, store::open_mode::read_only);

	load_meta ();
	debug_assert (current_meta.has_value ());
	auto meta = current_meta.value ();

	close ();

	return meta;
}

auto backend::open (column_schema schema, nano::store::open_mode mode) -> void
{
	release_assert (!is_open, "backend is already open");

	open_impl (schema, mode);

	load_meta ();
	debug_assert (current_meta.has_value ());

	current_schema = schema;
	is_open = true;
}

void backend::create (column_schema schema, nano::store::version_t version)
{
	release_assert (!is_open, "backend is already open");

	// Create and immediately close to initialize the database structure
	open_impl (schema, nano::store::open_mode::create);
	is_open = true;

	// Set the version in the meta table
	set_version (tx_begin_write (), version);

	close ();
}

void backend::close ()
{
	close_impl ();

	current_meta.reset ();
	current_schema.clear ();
	is_open = false;
}

auto backend::get_schema () const -> column_schema
{
	return current_schema;
}

void backend::load_meta ()
{
	backend_meta meta{};
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
version_store::version_store (store::backend & backend_a) :
	backend{ backend_a }
{
}

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

auto backend::get_version (store::transaction const & transaction) const -> nano::store::version_t
{
	version_store version_store_impl{ const_cast<backend &> (*this) };
	return version_store_impl.get_version (transaction);
}

void backend::set_version (store::write_transaction const & transaction, nano::store::version_t version)
{
	version_store version_store_impl{ *this };
	version_store_impl.put_version (transaction, version);
}
}

namespace nano
{
std::string error_backend_messages::message (int ev) const
{
	switch (static_cast<nano::error_backend> (ev))
	{
		case nano::error_backend::generic:
			return "Generic backend error";
		case nano::error_backend::db_not_found:
			return "Database not found";
		case nano::error_backend::table_not_found:
			return "Table not found";
		case nano::error_backend::failure:
			return "Backend operation failed";
	}
	return "Invalid error code";
}
}
