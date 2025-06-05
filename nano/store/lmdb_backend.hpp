#pragma once

#include <nano/lib/diagnosticsconfig.hpp>
#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/logging.hpp>
#include <nano/store/backend.hpp>
#include <nano/store/lmdb/db_val.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/store/lmdb/transaction_impl.hpp>

#include <lmdb/libraries/liblmdb/lmdb.h>

namespace nano::store
{
/**
 * LMDB implementation of the backend interface.
 * This contains only the minimal database-specific operations.
 */
class lmdb_backend : public backend
{
public:
	lmdb_backend (nano::logger & logger, std::filesystem::path const & path, nano::ledger_constants & constants, 
		nano::txn_tracking_config const & txn_tracking_config = nano::txn_tracking_config{}, 
		std::chrono::milliseconds block_processor_batch_max_time = std::chrono::milliseconds (5000),
		nano::lmdb_config const & lmdb_config = nano::lmdb_config{}, 
		bool backup_before_upgrade = false,
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
	write_transaction tx_begin_write ();
	read_transaction tx_begin_read () const;

	// Database management
	bool init_error () const;
	bool copy_db (std::filesystem::path const & destination);
	void rebuild_db (write_transaction const & tx);
	std::string vendor_get () const;
	std::filesystem::path get_database_path () const;
	nano::store::open_mode get_mode () const;

	// Memory/stats
	void serialize_memory_stats (boost::property_tree::ptree & json);
	void serialize_mdb_tracker (boost::property_tree::ptree & json, std::chrono::milliseconds min_time, std::chrono::milliseconds max_time);

public:
	nano::store::lmdb::env env;

private:
	MDB_dbi table_to_dbi (tables table) const;

	bool error{ false };
	std::filesystem::path const database_path;
	nano::store::open_mode const mode;
	nano::logger & logger;

	// Table handles
	MDB_dbi accounts_handle{ 0 };
	MDB_dbi blocks_handle{ 0 };
	MDB_dbi confirmation_height_handle{ 0 };
	MDB_dbi final_votes_handle{ 0 };
	MDB_dbi meta_handle{ 0 };
	MDB_dbi online_weight_handle{ 0 };
	MDB_dbi peers_handle{ 0 };
	MDB_dbi pending_handle{ 0 };
	MDB_dbi pruned_handle{ 0 };
	MDB_dbi vote_handle{ 0 };
	MDB_dbi rep_weights_handle{ 0 };
};
} // namespace nano::store