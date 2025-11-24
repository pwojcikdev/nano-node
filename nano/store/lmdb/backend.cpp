#include <nano/store/lmdb/backend.hpp>
#include <nano/store/lmdb/iterator.hpp>
#include <nano/store/lmdb/utility.hpp>

#include <boost/format.hpp>

namespace nano::store::lmdb
{
backend::backend (std::filesystem::path const & path, nano::lmdb_config const & config_a, nano::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a) :
	database_path{ path },
	config{ config_a },
	txn_tracking_config{ txn_tracking_config_a },
	block_processor_batch_max_time{ block_processor_batch_max_time_a },
	mdb_txn_tracker{ nano::logger::get (), txn_tracking_config_a, block_processor_batch_max_time_a },
	txn_tracking_enabled{ txn_tracking_config_a.enable }
{
}

void backend::open_impl (column_schema schema, nano::store::open_mode mode)
{
	current_mode = mode;

	// Create environment with appropriate options
	auto options = nano::store::lmdb::env::options::make ()
					   .set_config (config)
					   .set_use_no_mem_init (true)
					   .set_read_only (mode == nano::store::open_mode::read_only);

	env_impl = std::make_unique<nano::store::lmdb::env> (database_path, options);
	env = env_impl.get ();

	// Determine flags for opening tables
	unsigned flags = (mode == nano::store::open_mode::read_only) ? 0 : MDB_CREATE;

	// Open all tables specified in schema
	auto transaction = tx_begin_write ();
	for (auto const & [table, name] : schema)
	{
		open_table (transaction, table, name.c_str (), flags);
	}
	transaction.commit ();
}

void backend::close_impl ()
{
	// Close all table handles
	if (env)
	{
		for (auto const & [table, dbi] : table_handles)
		{
			mdb_dbi_close (*env, dbi);
		}
	}
	table_handles.clear ();

	// Release environment
	env = nullptr;
	env_impl.reset ();
}

void backend::open_table (store::transaction const & transaction, tables table, char const * name, unsigned flags)
{
	MDB_dbi handle;
	auto status = mdb_dbi_open (env->tx (transaction), name, flags, &handle);
	if (status != 0)
	{
		throw std::runtime_error ("Failed to open " + std::string (name) + " database: " + error_string (status));
	}
	table_handles[table] = handle;
}

MDB_dbi backend::table_to_dbi (tables table) const
{
	auto it = table_handles.find (table);
	release_assert (it != table_handles.end (), "table not found");
	return it->second;
}

int backend::get (store::transaction const & tx, tables table, db_val const & key, db_val & value) const
{
	auto mdb_key = to_mdb_val (key);
	MDB_val mdb_value{};

	auto result = mdb_get (env->tx (tx), table_to_dbi (table), &mdb_key, &mdb_value);
	if (result == MDB_SUCCESS)
	{
		value = from_mdb_val (mdb_value);
	}
	return result;
}

int backend::put (store::write_transaction const & tx, tables table, db_val const & key, db_val const & value)
{
	auto mdb_key = to_mdb_val (key);
	auto mdb_value = to_mdb_val (value);
	return mdb_put (env->tx (tx), table_to_dbi (table), &mdb_key, &mdb_value, 0);
}

int backend::del (store::write_transaction const & tx, tables table, db_val const & key)
{
	auto mdb_key = to_mdb_val (key);
	return mdb_del (env->tx (tx), table_to_dbi (table), &mdb_key, nullptr);
}

bool backend::exists (store::transaction const & tx, tables table, db_val const & key) const
{
	db_val junk;
	auto status = get (tx, table, key, junk);
	release_assert (success (status) || not_found (status), error_string (status));
	return status == MDB_SUCCESS;
}

uint64_t backend::count (store::transaction const & tx, tables table) const
{
	MDB_stat stats;
	auto status = mdb_stat (env->tx (tx), table_to_dbi (table), &stats);
	release_assert (success (status), error_string (status));
	return stats.ms_entries;
}

int backend::drop (store::write_transaction const & tx, tables table)
{
	return mdb_drop (env->tx (tx), table_to_dbi (table), 0);
}

store::iterator backend::begin (store::transaction const & tx, tables table) const
{
	return store::iterator{ iterator::begin (env->tx (tx), table_to_dbi (table)) };
}

store::iterator backend::begin (store::transaction const & tx, tables table, db_val const & key) const
{
	auto mdb_key = to_mdb_val (key);
	return store::iterator{ iterator::lower_bound (env->tx (tx), table_to_dbi (table), mdb_key) };
}

store::iterator backend::end (store::transaction const & tx, tables table) const
{
	return store::iterator{ iterator::end (env->tx (tx), table_to_dbi (table)) };
}

bool backend::success (int status) const
{
	return MDB_SUCCESS == status;
}

bool backend::not_found (int status) const
{
	return MDB_NOTFOUND == status;
}

std::string backend::error_string (int status) const
{
	return "status: " + std::to_string (status) + " (" + mdb_strerror (status) + ")";
}

store::read_transaction backend::tx_begin_read () const
{
	return env->tx_begin_read (create_txn_callbacks ());
}

store::write_transaction backend::tx_begin_write ()
{
	return env->tx_begin_write (create_txn_callbacks ());
}

nano::store::lmdb::txn_callbacks backend::create_txn_callbacks () const
{
	nano::store::lmdb::txn_callbacks callbacks;
	if (txn_tracking_enabled)
	{
		callbacks.txn_start = [&tracker = mdb_txn_tracker] (store::transaction_impl const * transaction_impl) {
			tracker.add (transaction_impl);
		};
		callbacks.txn_end = [&tracker = mdb_txn_tracker] (store::transaction_impl const * transaction_impl) {
			tracker.erase (transaction_impl);
		};
	}
	return callbacks;
}

void backend::backup ()
{
	auto extension = database_path.extension ();
	auto filename_without_extension = database_path.filename ().replace_extension ("");
	auto backup_path = database_path.parent_path ();
	auto backup_filename = filename_without_extension;
	backup_filename += "_backup_";
	backup_filename += std::to_string (std::chrono::system_clock::now ().time_since_epoch ().count ());
	backup_filename += extension;
	auto backup_filepath = backup_path / backup_filename;

	auto error = mdb_env_copy (*env, backup_filepath.string ().c_str ());
	if (error)
	{
		throw std::runtime_error ("Database backup failed: " + error_string (error));
	}
}

bool backend::copy_db (std::filesystem::path const & destination)
{
	return !mdb_env_copy2 (*env, destination.string ().c_str (), MDB_CP_COMPACT);
}

std::string backend::vendor_get () const
{
	return boost::str (boost::format ("LMDB %1%.%2%.%3%") % MDB_VERSION_MAJOR % MDB_VERSION_MINOR % MDB_VERSION_PATCH);
}

std::filesystem::path backend::get_database_path () const
{
	return database_path;
}

nano::store::open_mode backend::get_mode () const
{
	return current_mode;
}
} // namespace nano::store::lmdb
