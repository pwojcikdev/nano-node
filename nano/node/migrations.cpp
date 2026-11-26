#include <nano/lib/config.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/logging.hpp>
#include <nano/node/make_store.hpp>
#include <nano/node/migrations.hpp>
#include <nano/store/ledger_store.hpp>
#include <nano/store/lmdb/backend_lmdb.hpp>
#include <nano/store/rocksdb/backend_rocksdb.hpp>

#include <boost/scope/scope_exit.hpp>

#include <array>

#include <rocksdb/env.h>

void nano::migrate_lmdb_to_rocksdb (
std::filesystem::path const & data_path,
nano::lmdb_config const & lmdb_config,
nano::rocksdb_config const & rocksdb_config)
{
	nano::default_logger ().info (nano::log::type::migration, "Migrating LMDB database to RocksDB. This will take a while...");

	auto lmdb_path = nano::database_path_for_backend (data_path, nano::database_backend::lmdb);
	auto rocksdb_path = nano::database_path_for_backend (data_path, nano::database_backend::rocksdb);

	// Check source exists
	if (!std::filesystem::exists (lmdb_path))
	{
		throw std::runtime_error ("LMDB database not found: " + lmdb_path.string ());
	}

	// Check destination doesn't already exist
	if (std::filesystem::exists (rocksdb_path))
	{
		nano::default_logger ().error (nano::log::type::migration, "Existing RocksDB folder found in '{}'. Please remove it and try again.", rocksdb_path.string ());
		throw std::runtime_error ("RocksDB folder already exists: " + rocksdb_path.string ());
	}

	// Set secure permissions
	boost::system::error_code error_chmod;
	nano::set_secure_perm_directory (data_path, error_chmod);

	// Copy the source format verbatim. The first writable RocksDB open performs any ledger upgrades.
	auto lmdb_backend = std::make_unique<nano::store::lmdb::backend_lmdb> (lmdb_path, lmdb_config, nano::default_logger ());
	auto const meta = lmdb_backend->fetch_meta ();
	using ledger_store = nano::store::ledger_store;
	static constexpr std::array schemas{
		&ledger_store::schema_v21, &ledger_store::schema_v22, &ledger_store::schema_v23, &ledger_store::schema_v24,
		&ledger_store::schema_v25, &ledger_store::schema_v26, &ledger_store::schema_current
	};
	static_assert (schemas.size () == ledger_store::version_current - ledger_store::version_minimum + 1);
	if (!meta || meta->version < ledger_store::version_minimum || meta->version > ledger_store::version_current)
	{
		throw std::runtime_error ("Cannot migrate unsupported LMDB ledger version " + (meta ? std::to_string (meta->version) : "unknown"));
	}
	auto const & schema = *schemas[meta->version - ledger_store::version_minimum];
	lmdb_backend->open (schema, nano::store::open_mode::read_only);

	auto progress_cb = [] (nano::store::copy_progress const & p) {
		auto pct = p.total_entries > 0 ? p.entries_copied * 100 / p.total_entries : 100;

		nano::default_logger ().info (nano::log::type::migration,
		"Table {} of {}: {} - {:9} / {:9} ({}%)",
		p.current_table_index + 1, p.total_tables, p.table_name,
		p.entries_copied, p.total_entries, pct);
	};

	// The lock file marks this workspace as ours and stays in place across retries.
	auto const workspace = data_path / ".lmdb-to-rocksdb";
	std::filesystem::create_directory (workspace);
	if (!std::filesystem::is_directory (std::filesystem::symlink_status (workspace)))
	{
		throw std::runtime_error ("Invalid migration workspace: " + workspace.string ());
	}
	auto const marker = std::filesystem::canonical (workspace) / ".nano-migration-v1.lock";
	if (!std::filesystem::is_empty (workspace) && (!std::filesystem::is_regular_file (std::filesystem::symlink_status (marker)) || std::filesystem::file_size (marker) != 0))
	{
		throw std::runtime_error ("Refusing to use an unmarked migration workspace: " + workspace.string ());
	}
	auto env = ::rocksdb::Env::Default ();
	::rocksdb::FileLock * lock{};
	if (auto status = env->LockFile (marker.string (), &lock); !status.ok ())
	{
		throw std::runtime_error ("Could not lock migration workspace: " + status.ToString ());
	}
	boost::scope::scope_exit unlock{ [&] { env->UnlockFile (lock).PermitUncheckedError (); } };

	// Only a previous attempt holding this lock could have started a copy here.
	auto const staging_path = workspace / "ledger";
	if (auto status = std::filesystem::symlink_status (staging_path); std::filesystem::exists (status))
	{
		if (!std::filesystem::is_directory (status))
		{
			throw std::runtime_error ("Invalid migration staging directory: " + staging_path.string ());
		}
		nano::default_logger ().info (nano::log::type::migration, "Removing interrupted migration copy at {}", staging_path.string ());
		std::filesystem::remove_all (staging_path);
	}

	// Check disk space
	std::filesystem::space_info si = std::filesystem::space (data_path);
	auto file_size = std::filesystem::file_size (lmdb_path);
	auto const estimated_required_space = static_cast<uint64_t> (file_size * 0.65);

	if (si.available < estimated_required_space)
	{
		nano::default_logger ().warn (nano::log::type::migration, "You may not have enough available disk space. Estimated free space requirement is {} GB", estimated_required_space / 1024 / 1024 / 1024);
	}

	try
	{
		nano::store::rocksdb::backend_rocksdb destination{ staging_path, rocksdb_config, nano::default_logger () };
		destination.open (schema, nano::store::open_mode::read_write);
		lmdb_backend->copy_to (destination, progress_cb);
		destination.close ();

		if (std::filesystem::exists (rocksdb_path))
		{
			throw std::runtime_error ("RocksDB folder already exists: " + rocksdb_path.string ());
		}
		std::filesystem::rename (staging_path, rocksdb_path);
	}
	catch (...)
	{
		std::error_code ec;
		std::filesystem::remove_all (staging_path, ec);
		if (ec)
		{
			nano::default_logger ().warn (nano::log::type::migration, "Could not remove migration staging directory {}: {}", staging_path.string (), ec.message ());
		}
		throw;
	}

	nano::default_logger ().info (nano::log::type::migration, "Migration completed. Make sure to set `database_backend` under [node] to 'rocksdb' in config-node.toml");
	nano::default_logger ().info (nano::log::type::migration, "After confirming correct node operation, the data.ldb file can be deleted if no longer required");
}
