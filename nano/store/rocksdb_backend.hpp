#pragma once

#include <nano/lib/config.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/rocksdbconfig.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/rocksdb/db_val.hpp>
#include <nano/store/rocksdb/transaction_impl.hpp>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>

namespace nano::store
{
/**
 * RocksDB implementation of the backend interface.
 * This contains only the minimal database-specific operations.
 */
class rocksdb_backend : public backend
{
public:
	rocksdb_backend (nano::logger & logger, std::filesystem::path const & path, nano::ledger_constants & constants,
		nano::rocksdb_config const & rocksdb_config = nano::rocksdb_config{},
		nano::store::open_mode mode = nano::store::open_mode::read_write);

	// Backend interface
	int get (transaction const & tx, tables table, db_val const & key, db_val & value) const override;
	int put (write_transaction const & tx, tables table, db_val const & key, db_val const & value) override;
	int del (write_transaction const & tx, tables table, db_val const & key) override;
	bool exists (transaction const & tx, tables table, db_val const & key) const override;

	uint64_t count (transaction const & tx, tables table) const override;
	int drop (write_transaction const & tx, tables table) override;

	std::unique_ptr<store::iterator> begin (transaction const & tx, tables table) const override;
	std::unique_ptr<store::iterator> begin (transaction const & tx, tables table, db_val const & key) const override;
	std::unique_ptr<store::iterator> end (transaction const & tx, tables table) const override;

	bool success (int status) const override;
	bool not_found (int status) const override;
	std::string error_string (int status) const override;

	// Transaction management
	write_transaction tx_begin_write () override;
	read_transaction tx_begin_read () const override;

	// Database management
	bool init_error () const;
	bool copy_db (std::filesystem::path const & destination);
	void rebuild_db (write_transaction const & tx);
	std::string vendor_get () const;
	std::filesystem::path get_database_path () const;
	nano::store::open_mode get_mode () const;

	// Memory/stats
	void serialize_memory_stats (boost::property_tree::ptree & json);

public:
	std::unique_ptr<::rocksdb::TransactionDB> db;
	std::vector<std::unique_ptr<::rocksdb::ColumnFamilyHandle>> handles;

private:
	::rocksdb::ColumnFamilyHandle * table_to_column_family (tables table) const;

	bool error{ false };
	std::filesystem::path const database_path;
	nano::store::open_mode const mode;
	nano::logger & logger;

	// Column family handles for fast lookup
	::rocksdb::ColumnFamilyHandle * default_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * accounts_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * blocks_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * confirmation_height_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * final_votes_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * meta_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * online_weight_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * peers_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * pending_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * pruned_handle{ nullptr };
	::rocksdb::ColumnFamilyHandle * rep_weights_handle{ nullptr };
};
} // namespace nano::store