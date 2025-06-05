#include <nano/store/lmdb_backend.hpp>
#include <nano/store/lmdb/iterator.hpp>

namespace nano::store
{
lmdb_backend::lmdb_backend (nano::logger & logger_a, std::filesystem::path const & path_a, nano::ledger_constants & constants, 
	nano::txn_tracking_config const & txn_tracking_config, std::chrono::milliseconds block_processor_batch_max_time,
	nano::lmdb_config const & lmdb_config, bool backup_before_upgrade, nano::store::open_mode mode_a) :
	database_path{ path_a },
	mode{ mode_a },
	logger{ logger_a },
	env (error, path_a, nano::store::lmdb::env::options::make ().set_config (lmdb_config).set_use_no_mem_init (true).set_read_only (mode_a == nano::store::open_mode::read_only))
{
	if (!error)
	{
		// Open all database tables
		auto transaction = tx_begin_write ();
		error |= mdb_dbi_open (env.tx (transaction), "accounts", MDB_CREATE, &accounts_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "blocks", MDB_CREATE, &blocks_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "confirmation_height", MDB_CREATE, &confirmation_height_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "final_votes", MDB_CREATE, &final_votes_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "meta", MDB_CREATE, &meta_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "online_weight", MDB_CREATE, &online_weight_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "peers", MDB_CREATE, &peers_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "pending", MDB_CREATE, &pending_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "pruned", MDB_CREATE, &pruned_handle) != 0;
		error |= mdb_dbi_open (env.tx (transaction), "rep_weights", MDB_CREATE, &rep_weights_handle) != 0;
	}
}

MDB_dbi lmdb_backend::table_to_dbi (tables table) const
{
	switch (table)
	{
		case tables::accounts:
			return accounts_handle;
		case tables::blocks:
			return blocks_handle;
		case tables::confirmation_height:
			return confirmation_height_handle;
		case tables::final_votes:
			return final_votes_handle;
		case tables::meta:
			return meta_handle;
		case tables::online_weight:
			return online_weight_handle;
		case tables::peers:
			return peers_handle;
		case tables::pending:
			return pending_handle;
		case tables::pruned:
			return pruned_handle;
		case tables::rep_weights:
			return rep_weights_handle;
		default:
			release_assert (false, "Invalid table");
			return 0;
	}
}

int lmdb_backend::get (transaction const & tx, tables table, db_val const & key, db_val & value) const
{
	auto & lmdb_key = static_cast<nano::store::lmdb::db_val const &> (key);
	auto & lmdb_value = static_cast<nano::store::lmdb::db_val &> (value);
	return mdb_get (env.tx (tx), table_to_dbi (table), lmdb_key, lmdb_value);
}

int lmdb_backend::put (write_transaction const & tx, tables table, db_val const & key, db_val const & value)
{
	auto & lmdb_key = static_cast<nano::store::lmdb::db_val const &> (key);
	auto & lmdb_value = static_cast<nano::store::lmdb::db_val const &> (value);
	return mdb_put (env.tx (tx), table_to_dbi (table), lmdb_key, lmdb_value, 0);
}

int lmdb_backend::del (write_transaction const & tx, tables table, db_val const & key)
{
	auto & lmdb_key = static_cast<nano::store::lmdb::db_val const &> (key);
	return mdb_del (env.tx (tx), table_to_dbi (table), lmdb_key, nullptr);
}

bool lmdb_backend::exists (transaction const & tx, tables table, db_val const & key) const
{
	nano::store::lmdb::db_val junk;
	auto status = get (tx, table, key, junk);
	release_assert (success (status) || not_found (status), error_string (status));
	return success (status);
}

uint64_t lmdb_backend::count (transaction const & tx, tables table) const
{
	MDB_stat stats;
	auto status = mdb_stat (env.tx (tx), table_to_dbi (table), &stats);
	release_assert_success (status);
	return stats.ms_entries;
}

int lmdb_backend::drop (write_transaction const & tx, tables table)
{
	return mdb_drop (env.tx (tx), table_to_dbi (table), 0);
}

std::unique_ptr<store::iterator> lmdb_backend::begin (transaction const & tx, tables table) const
{
	return std::make_unique<store::iterator> (lmdb::iterator::begin (env.tx (tx), table_to_dbi (table)));
}

std::unique_ptr<store::iterator> lmdb_backend::begin (transaction const & tx, tables table, db_val const & key) const
{
	auto & lmdb_key = static_cast<nano::store::lmdb::db_val const &> (key);
	return std::make_unique<store::iterator> (lmdb::iterator::lower_bound (env.tx (tx), table_to_dbi (table), lmdb_key));
}

std::unique_ptr<store::iterator> lmdb_backend::end (transaction const & tx, tables table) const
{
	return std::make_unique<store::iterator> (lmdb::iterator::end (env.tx (tx), table_to_dbi (table)));
}

bool lmdb_backend::success (int status) const
{
	return status == MDB_SUCCESS;
}

bool lmdb_backend::not_found (int status) const
{
	return status == MDB_NOTFOUND;
}

std::string lmdb_backend::error_string (int status) const
{
	return mdb_strerror (status);
}

write_transaction lmdb_backend::tx_begin_write ()
{
	return env.tx_begin_write ();
}

read_transaction lmdb_backend::tx_begin_read () const
{
	return env.tx_begin_read ();
}

bool lmdb_backend::init_error () const
{
	return error;
}

bool lmdb_backend::copy_db (std::filesystem::path const & destination)
{
	return env.copy_db (destination);
}

void lmdb_backend::rebuild_db (write_transaction const & tx)
{
	// LMDB doesn't need explicit rebuilding
}

std::string lmdb_backend::vendor_get () const
{
	return "LMDB";
}

std::filesystem::path lmdb_backend::get_database_path () const
{
	return database_path;
}

nano::store::open_mode lmdb_backend::get_mode () const
{
	return mode;
}

void lmdb_backend::serialize_memory_stats (boost::property_tree::ptree & json)
{
	env.serialize_memory_stats (json);
}

void lmdb_backend::serialize_mdb_tracker (boost::property_tree::ptree & json, std::chrono::milliseconds min_time, std::chrono::milliseconds max_time)
{
	// Implementation would go here if tracking is needed
}
} // namespace nano::store