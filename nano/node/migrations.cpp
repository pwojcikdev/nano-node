#include <nano/lib/config.hpp>
#include <nano/lib/files.hpp>
#include <nano/lib/logging.hpp>
#include <nano/node/make_store.hpp>
#include <nano/node/migrations.hpp>
#include <nano/store/ledger_store.hpp>
#include <nano/store/lmdb/backend_lmdb.hpp>
#include <nano/store/rocksdb/backend_rocksdb.hpp>

#include <array>

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

	// Check disk space
	std::filesystem::space_info si = std::filesystem::space (data_path);
	auto file_size = std::filesystem::file_size (lmdb_path);
	auto const estimated_required_space = static_cast<uint64_t> (file_size * 0.65);

	if (si.available < estimated_required_space)
	{
		nano::default_logger ().warn (nano::log::type::migration, "You may not have enough available disk space. Estimated free space requirement is {} GB", estimated_required_space / 1024 / 1024 / 1024);
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

	// Create and open destination RocksDB backend (read-write)
	auto rocksdb_backend = std::make_unique<nano::store::rocksdb::backend_rocksdb> (rocksdb_path, rocksdb_config, nano::default_logger ());
	rocksdb_backend->open (schema, nano::store::open_mode::read_write);

	auto progress_cb = [] (nano::store::copy_progress const & p) {
		auto pct = p.total_entries > 0 ? p.entries_copied * 100 / p.total_entries : 100;

		nano::default_logger ().info (nano::log::type::migration,
		"Table {} of {}: {} - {:9} / {:9} ({}%)",
		p.current_table_index + 1, p.total_tables, p.table_name,
		p.entries_copied, p.total_entries, pct);
	};

	// Perform the migration
	lmdb_backend->copy_to (*rocksdb_backend, progress_cb);

	nano::default_logger ().info (nano::log::type::migration, "Migration completed. Make sure to set `database_backend` under [node] to 'rocksdb' in config-node.toml");
	nano::default_logger ().info (nano::log::type::migration, "After confirming correct node operation, the data.ldb file can be deleted if no longer required");
}
