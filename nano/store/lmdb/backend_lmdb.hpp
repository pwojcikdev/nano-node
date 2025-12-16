#pragma once

#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/logging.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/store/lmdb/transaction_lmdb.hpp>

#include <unordered_map>

namespace nano::store::lmdb
{
/**
 * LMDB implementation of the backend interface.
 * Provides low-level database operations using LMDB.
 */
class backend_lmdb : public nano::store::backend
{
public:
	backend_lmdb (std::filesystem::path const & path, nano::logger & logger, nano::lmdb_config const & config, nano::txn_tracking_config const & txn_tracking_config = {}, std::chrono::milliseconds block_processor_batch_max_time = std::chrono::milliseconds{ 5000 });
	~backend_lmdb () override;

	int get (nano::store::transaction const &, tables, nano::store::db_val const & key, nano::store::db_val & value) const override;
	int put (nano::store::write_transaction const &, tables, nano::store::db_val const & key, nano::store::db_val const & value) override;
	int del (nano::store::write_transaction const &, tables, nano::store::db_val const & key) override;
	bool exists (nano::store::transaction const &, tables, nano::store::db_val const & key) const override;

	uint64_t count (nano::store::transaction const &, tables) const override;
	int clear (nano::store::write_transaction const &, tables) override;
	bool drop_table (nano::store::write_transaction const &, std::string const & name) override;
	bool table_exists (nano::store::transaction const &, std::string const & name) const override;

	nano::store::iterator begin (nano::store::transaction const &, tables) const override;
	nano::store::iterator begin (nano::store::transaction const &, tables, nano::store::db_val const & key) const override;
	nano::store::iterator end (nano::store::transaction const &, tables) const override;

	bool success (int status) const override;
	bool not_found (int status) const override;
	std::string error_string (int status) const override;

	nano::store::read_transaction tx_begin_read () const override;
	nano::store::write_transaction tx_begin_write () override;

	void copy_with_compaction (std::filesystem::path const & destination) override;
	void backup () override;

	std::string vendor_get () const override;
	std::string get_database_path () const override;

protected:
	void open_impl (column_schema schema, nano::store::open_mode mode) override;
	void close_impl () override;

private:
	std::filesystem::path const database_path;
	nano::lmdb_config config;
	nano::txn_tracking_config txn_tracking_config;
	std::chrono::milliseconds block_processor_batch_max_time;

	std::unique_ptr<nano::store::lmdb::env> env;
	std::unordered_map<tables, nano::store::lmdb::env::table_handle> table_handles;

	mutable nano::mdb_txn_tracker mdb_txn_tracker;
	bool txn_tracking_enabled{ false };

	nano::store::lmdb::env::table_handle table_to_dbi (tables table) const;
	void open_table (MDB_txn * mdb_txn, tables table, char const * name, unsigned flags);

	nano::store::lmdb::txn_callbacks create_txn_callbacks () const;
};
}
