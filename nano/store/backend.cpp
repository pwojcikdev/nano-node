#include <nano/store/backend.hpp>

namespace nano::store
{
nano::store::column_schema const backend::schema_meta{ { tables::meta, "meta" } };

backend::~backend () = default;

void backend::open (column_schema schema, nano::store::open_mode mode)
{
	if (is_open)
	{
		throw std::runtime_error ("Backend is already open: " + get_database_path ());
	}

	open_impl (schema, mode);

	is_open = true;
	current_mode = mode;
	current_schema = schema;

	load_meta ();
	debug_assert (current_meta.has_value ());
}

void backend::create (column_schema schema, nano::store::version_t version)
{
	if (is_open)
	{
		throw std::runtime_error ("Backend is already open: " + get_database_path ());
	}

	// Create and immediately close to initialize the database structure
	open (schema, nano::store::open_mode::read_write);

	// Ensure database doesn't already exist
	if (meta.version_exists (tx_begin_read ()))
	{
		throw std::runtime_error ("Attempting to create a database that already exists: " + get_database_path ());
	}

	// Set the version in the meta table
	meta.put_version (tx_begin_write (), version);

	close ();
}

void backend::close ()
{
	close_impl ();

	is_open = false;
	current_mode = {};
	current_meta.reset ();
	current_schema.clear ();
}

auto backend::fetch_meta () -> std::optional<backend_meta>
{
	// Attempt to open just the meta table to check if database exists
	try
	{
		open (schema_meta, store::open_mode::read_only);
	}
	catch (nano::error const & error)
	{
		if (error == nano::error_backend::db_not_found)
		{
			return std::nullopt;
		}
		throw;
	}

	load_meta ();
	debug_assert (current_meta.has_value ());
	auto result = current_meta.value ();

	close ();

	return result;
}

void backend::load_meta ()
{
	backend_meta info{};
	info.version = meta.get_version (tx_begin_read ());
	current_meta = info;
}

auto backend::get_meta () const -> backend_meta
{
	release_assert (current_meta.has_value (), "meta information has not been loaded");
	return current_meta.value ();
}

auto backend::get_schema () const -> column_schema
{
	return current_schema;
}

auto backend::get_mode () const -> std::optional<nano::store::open_mode>
{
	return is_open ? std::optional{ current_mode } : std::nullopt;
}

auto backend::get_version (store::transaction const & transaction) const -> nano::store::version_t
{
	return meta.get_version (transaction);
}

void backend::set_version (store::write_transaction const & transaction, nano::store::version_t version)
{
	meta.put_version (transaction, version);
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
