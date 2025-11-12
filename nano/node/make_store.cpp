#include <nano/lib/logging.hpp>
#include <nano/node/make_store.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/store/lmdb/lmdb.hpp>
#include <nano/store/rocksdb/rocksdb.hpp>

std::unique_ptr<nano::store::component> nano::make_store (nano::logger & logger, std::filesystem::path const & path, nano::ledger_constants & constants, bool read_only, bool add_db_postfix, nano::node_config node_config)
{
	auto decide_backend = [&] () -> nano::database_backend {
		if (node_config.rocksdb_config.enable && node_config.database_backend == nano::database_backend::lmdb)
		{
			logger.warn (nano::log::type::config, "Use of deprecated `[node.rocksdb].enable` setting detected in config file, defaulting to RocksDB backend.\nPlease edit config-node.toml and use the new '[node].database_backend' for future compatibility.");
			return nano::database_backend::rocksdb;
		}
		return node_config.database_backend;
	};

	nano::store::open_mode const mode = read_only ? nano::store::open_mode::read_only : nano::store::open_mode::read_write;

	auto backend = decide_backend ();
	switch (backend)
	{
		case nano::database_backend::lmdb:
		{
			return std::make_unique<nano::store::lmdb::component> (logger, add_db_postfix ? path / "data.ldb" : path, constants, node_config.diagnostics_config.txn_tracking, node_config.block_processor_batch_max_time, node_config.lmdb_config, node_config.backup_before_upgrade, mode);
		}
		case nano::database_backend::rocksdb:
		{
			return std::make_unique<nano::store::rocksdb::component> (logger, add_db_postfix ? path / "rocksdb" : path, constants, node_config.rocksdb_config, mode);
		}
	}

	release_assert (false); // Must be handled above
}

std::unique_ptr<nano::store::component> nano::make_vote_store (nano::logger & logger, std::filesystem::path const & path, nano::ledger_constants & constants, nano::node_config node_config)
{
	// Vote store is always read-write
	nano::store::open_mode const mode = nano::store::open_mode::read_write;

	// Use the same backend as the main store
	switch (node_config.database_backend)
	{
		case nano::database_backend::lmdb:
		{
			// Create separate vote storage database with nosync_unsafe for performance
			auto lmdb_config = node_config.lmdb_config;
			lmdb_config.sync = nano::lmdb_config::sync_strategy::nosync_unsafe;

			return std::make_unique<nano::store::lmdb::component> (
				logger,
				path / "votes.ldb",
				constants,
				node_config.diagnostics_config.txn_tracking,
				node_config.block_processor_batch_max_time,
				lmdb_config,
				false, // backup_before_upgrade
				mode);
		}
		case nano::database_backend::rocksdb:
		{
			return std::make_unique<nano::store::rocksdb::component> (
				logger,
				path / "votes_rocksdb",
				constants,
				node_config.rocksdb_config,
				mode);
		}
	}

	release_assert (false); // Must be handled above
}
