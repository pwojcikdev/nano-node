#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/make_store.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/store/ledger_store.hpp>
#include <nano/store/lmdb/backend_lmdb.hpp>
#include <nano/store/rocksdb/backend_rocksdb.hpp>

std::filesystem::path nano::database_path_for_backend (std::filesystem::path const & base_path, database_backend backend_type)
{
	switch (backend_type)
	{
		case nano::database_backend::lmdb:
			return base_path / "data.ldb";
		case nano::database_backend::rocksdb:
			return base_path / "rocksdb";
	}
	release_assert (false, "unknown database backend");
}

std::unique_ptr<nano::store::backend> nano::make_backend (nano::logger & logger, std::filesystem::path const & path, bool add_db_postfix, nano::node_config const & node_config)
{
	auto const backend_type = node_config.get_database_backend ();
	if (backend_type != node_config.database_backend)
	{
		logger.warn (nano::log::type::config, "Use of deprecated `[node.rocksdb].enable` setting detected in config file, defaulting to RocksDB backend.\nPlease edit config-node.toml and use the new '[node].database_backend' for future compatibility.");
	}
	std::unique_ptr<nano::store::backend> backend;

	switch (backend_type)
	{
		case nano::database_backend::lmdb:
		{
			auto db_path = add_db_postfix ? database_path_for_backend (path, backend_type) : path;
			backend = std::make_unique<nano::store::lmdb::backend_lmdb> (db_path, *node_config.lmdb_config, logger, *node_config.txn_tracking);
			break;
		}
		case nano::database_backend::rocksdb:
		{
			auto db_path = add_db_postfix ? database_path_for_backend (path, backend_type) : path;
			backend = std::make_unique<nano::store::rocksdb::backend_rocksdb> (db_path, *node_config.rocksdb_config, logger, *node_config.txn_tracking);
			break;
		}
	}

	release_assert (backend != nullptr);

	return backend;
}

std::unique_ptr<nano::store::ledger_store> nano::make_store (nano::logger & logger, nano::stats & stats, std::filesystem::path const & path, nano::ledger_constants & constants, bool read_only, bool add_db_postfix, nano::node_config const & node_config)
{
	return make_store (logger, stats, path, constants, read_only, add_db_postfix, node_config, false);
}

std::unique_ptr<nano::store::ledger_store> nano::make_store (nano::logger & logger, nano::stats & stats, std::filesystem::path const & path, nano::ledger_constants & constants, bool read_only, bool add_db_postfix, nano::node_config const & node_config, bool for_backup)
{
	auto backend = nano::make_backend (logger, path, add_db_postfix, node_config);

	auto mode = nano::store::open_mode::read_write;
	if (read_only)
	{
		mode = for_backup ? nano::store::open_mode::read_only_backup : nano::store::open_mode::read_only;
	}

	nano::store::ledger_store_params params;
	params.backup_before_upgrade = node_config.backup_before_upgrade;
	params.require_current_version = for_backup;

	return std::make_unique<nano::store::ledger_store> (std::move (backend), mode, stats, logger, params);
}
