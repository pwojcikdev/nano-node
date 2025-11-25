#pragma once

#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/logging.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/store/lmdb/transaction_impl.hpp>

#include <map>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::store::lmdb
{
/**
 * LMDB implementation of the backend interface.
 * Provides low-level database operations using LMDB.
 */
class backend : public nano::store::backend
{
public:
	backend (std::filesystem::path const & path, nano::logger & logger, nano::lmdb_config const & config, nano::txn_tracking_config const & txn_tracking_config = {}, std::chrono::milliseconds block_processor_batch_max_time = std::chrono::milliseconds{ 5000 });

	// CRUD operations
	int get (store::transaction const & tx, tables table, db_val const & key, db_val & value) const override;
	int put (store::write_transaction const & tx, tables table, db_val const & key, db_val const & value) override;
	int del (store::write_transaction const & tx, tables table, db_val const & key) override;
	bool exists (store::transaction const & tx, tables table, db_val const & key) const override;

	// Table operations
	uint64_t count (store::transaction const & tx, tables table) const override;
	int drop (store::write_transaction const & tx, tables table) override;

	// Iterator operations
	store::iterator begin (store::transaction const & tx, tables table) const override;
	store::iterator begin (store::transaction const & tx, tables table, db_val const & key) const override;
	store::iterator end (store::transaction const & tx, tables table) const override;

	// Status checking
	bool success (int status) const override;
	bool not_found (int status) const override;
	std::string error_string (int status) const override;

	// Transaction management
	store::read_transaction tx_begin_read () const override;
	store::write_transaction tx_begin_write () override;

	// Database lifecycle
	void backup () override;
	bool copy_db (std::filesystem::path const & destination) override;

	// Additional methods
	std::string vendor_get () const override;
	std::filesystem::path get_database_path () const override;
	nano::store::open_mode get_mode () const override;

protected:
	void open_impl (column_schema schema, nano::store::open_mode mode) override;
	void close_impl () override;

private:
	std::filesystem::path database_path;
	nano::lmdb_config config;
	nano::txn_tracking_config txn_tracking_config;
	std::chrono::milliseconds block_processor_batch_max_time;

	std::unique_ptr<nano::store::lmdb::env> env_impl;
	nano::store::lmdb::env * env{ nullptr };

	std::map<tables, MDB_dbi> table_handles;
	nano::store::open_mode current_mode{ nano::store::open_mode::read_write };

	mutable nano::mdb_txn_tracker mdb_txn_tracker;
	bool txn_tracking_enabled{ false };

	MDB_dbi table_to_dbi (tables table) const;
	void open_table (store::transaction const & tx, tables table, char const * name, unsigned flags);

	nano::store::lmdb::txn_callbacks create_txn_callbacks () const;
};
} // namespace nano::store::lmdb
