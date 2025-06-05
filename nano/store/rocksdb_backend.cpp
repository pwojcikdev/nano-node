#include <nano/store/rocksdb_backend.hpp>
#include <nano/store/rocksdb/iterator.hpp>

#include <rocksdb/merge_operator.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/utilities/backup_engine.h>
#include <rocksdb/utilities/transaction.h>

namespace nano::store
{
rocksdb_backend::rocksdb_backend (nano::logger & logger_a, std::filesystem::path const & path_a, nano::ledger_constants & constants,
	nano::rocksdb_config const & rocksdb_config, nano::store::open_mode mode_a) :
	database_path{ path_a },
	mode{ mode_a },
	logger{ logger_a }
{
	// Create column family descriptors
	std::vector<::rocksdb::ColumnFamilyDescriptor> column_families;
	
	// Default column family (required by RocksDB)
	column_families.emplace_back (::rocksdb::kDefaultColumnFamilyName, ::rocksdb::ColumnFamilyOptions{});
	
	// Create column families for all tables in the same order we'll store handles
	column_families.emplace_back ("accounts", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("blocks", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("confirmation_height", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("final_votes", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("meta", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("online_weight", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("peers", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("pending", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("pruned", ::rocksdb::ColumnFamilyOptions{});
	column_families.emplace_back ("rep_weights", ::rocksdb::ColumnFamilyOptions{});
	
	// Set up RocksDB options
	::rocksdb::Options options;
	options.create_if_missing = true;
	options.create_missing_column_families = true;
	
	// Open database
	std::vector<::rocksdb::ColumnFamilyHandle *> handles_l;
	::rocksdb::TransactionDB * transaction_db = nullptr;
	
	auto status = ::rocksdb::TransactionDB::Open (options, ::rocksdb::TransactionDBOptions{}, path_a.string (), column_families, &handles_l, &transaction_db);
	
	if (status.ok () && transaction_db)
	{
		db.reset (transaction_db);
		
		// Store column family handles in specific order for fast lookup
		debug_assert (handles_l.size () == 11); // default + 10 tables
		
		// Verify that column family names match expected order
		debug_assert (handles_l[0]->GetName () == ::rocksdb::kDefaultColumnFamilyName);
		debug_assert (handles_l[1]->GetName () == "accounts");
		debug_assert (handles_l[2]->GetName () == "blocks");
		debug_assert (handles_l[3]->GetName () == "confirmation_height");
		debug_assert (handles_l[4]->GetName () == "final_votes");
		debug_assert (handles_l[5]->GetName () == "meta");
		debug_assert (handles_l[6]->GetName () == "online_weight");
		debug_assert (handles_l[7]->GetName () == "peers");
		debug_assert (handles_l[8]->GetName () == "pending");
		debug_assert (handles_l[9]->GetName () == "pruned");
		debug_assert (handles_l[10]->GetName () == "rep_weights");
		
		default_handle = handles_l[0];
		accounts_handle = handles_l[1];
		blocks_handle = handles_l[2];
		confirmation_height_handle = handles_l[3];
		final_votes_handle = handles_l[4];
		meta_handle = handles_l[5];
		online_weight_handle = handles_l[6];
		peers_handle = handles_l[7];
		pending_handle = handles_l[8];
		pruned_handle = handles_l[9];
		rep_weights_handle = handles_l[10];
		
		// Store all handles for cleanup
		handles.resize (handles_l.size ());
		for (size_t i = 0; i < handles_l.size (); ++i)
		{
			handles[i].reset (handles_l[i]);
		}
	}
	else
	{
		error = true;
		logger.error (nano::log::type::rocksdb, "Failed to open RocksDB: {}", status.ToString ());
	}
}

::rocksdb::ColumnFamilyHandle * rocksdb_backend::table_to_column_family (tables table) const
{
	switch (table)
	{
		case tables::accounts:
			return accounts_handle;
		case tables::blocks:
			return blocks_handle;
		case tables::pending:
			return pending_handle;
		case tables::online_weight:
			return online_weight_handle;
		case tables::meta:
			return meta_handle;
		case tables::peers:
			return peers_handle;
		case tables::pruned:
			return pruned_handle;
		case tables::confirmation_height:
			return confirmation_height_handle;
		case tables::final_votes:
			return final_votes_handle;
		case tables::rep_weights:
			return rep_weights_handle;
		default:
			release_assert (false, "Invalid table");
			return nullptr;
	}
}

int rocksdb_backend::get (transaction const & tx, tables table, db_val const & key, db_val & value) const
{
	auto & rocksdb_key = static_cast<nano::store::rocksdb::db_val const &> (key);
	auto & rocksdb_value = static_cast<nano::store::rocksdb::db_val &> (value);
	
	::rocksdb::PinnableSlice slice;
	auto internals = rocksdb::tx (tx);
	auto status = std::visit ([&] (auto && ptr) {
		using V = std::remove_cvref_t<decltype (ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			::rocksdb::ReadOptions options;
			options.fill_cache = false;
			return ptr->Get (options, table_to_column_family (table), rocksdb_key, &slice);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			return db->Get (*ptr, table_to_column_family (table), rocksdb_key, &slice);
		}
		else
		{
			static_assert (nano::always_false<V>, "Unhandled transaction type");
		}
	}, internals);
	
	if (status.ok ())
	{
		rocksdb_value = slice;
	}
	
	return status.code ();
}

int rocksdb_backend::put (write_transaction const & tx, tables table, db_val const & key, db_val const & value)
{
	auto & rocksdb_key = static_cast<nano::store::rocksdb::db_val const &> (key);
	auto & rocksdb_value = static_cast<nano::store::rocksdb::db_val const &> (value);
	
	auto transaction = std::get<::rocksdb::Transaction *> (rocksdb::tx (tx));
	return transaction->Put (table_to_column_family (table), rocksdb_key, rocksdb_value).code ();
}

int rocksdb_backend::del (write_transaction const & tx, tables table, db_val const & key)
{
	auto & rocksdb_key = static_cast<nano::store::rocksdb::db_val const &> (key);
	
	auto transaction = std::get<::rocksdb::Transaction *> (rocksdb::tx (tx));
	return transaction->Delete (table_to_column_family (table), rocksdb_key).code ();
}

bool rocksdb_backend::exists (transaction const & tx, tables table, db_val const & key) const
{
	::rocksdb::PinnableSlice slice;
	auto & rocksdb_key = static_cast<nano::store::rocksdb::db_val const &> (key);
	auto internals = rocksdb::tx (tx);
	auto status = std::visit ([&] (auto && ptr) {
		using V = std::remove_cvref_t<decltype (ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			::rocksdb::ReadOptions options;
			options.fill_cache = false;
			return ptr->Get (options, table_to_column_family (table), rocksdb_key, &slice);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			return db->Get (*ptr, table_to_column_family (table), rocksdb_key, &slice);
		}
		else
		{
			static_assert (nano::always_false<V>, "Unhandled transaction type");
		}
	}, internals);
	
	return status.ok ();
}

uint64_t rocksdb_backend::count (transaction const & tx, tables table) const
{
	uint64_t sum = 0;
	auto internals = rocksdb::tx (tx);
	std::visit ([&] (auto && ptr) {
		using V = std::remove_cvref_t<decltype (ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			db->GetIntProperty (table_to_column_family (table), "rocksdb.estimate-num-keys", &sum);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			db->GetIntProperty (table_to_column_family (table), "rocksdb.estimate-num-keys", &sum);
		}
		else
		{
			static_assert (nano::always_false<V>, "Unhandled transaction type");
		}
	}, internals);
	
	return sum;
}

int rocksdb_backend::drop (write_transaction const & tx, tables table)
{
	auto col = table_to_column_family (table);
	auto status = db->DropColumnFamily (col);
	if (status.ok ())
	{
		// Recreate the column family
		::rocksdb::ColumnFamilyHandle * handle;
		status = db->CreateColumnFamily (::rocksdb::ColumnFamilyOptions{}, col->GetName (), &handle);
		if (status.ok ())
		{
			// Replace the handle in our handles vector and specific handle
			for (auto & stored_handle : handles)
			{
				if (stored_handle.get () == col)
				{
					stored_handle.reset (handle);
					break;
				}
			}
			
			// Update the specific handle pointer
			switch (table)
			{
				case tables::accounts:
					accounts_handle = handle;
					break;
				case tables::blocks:
					blocks_handle = handle;
					break;
				case tables::pending:
					pending_handle = handle;
					break;
				case tables::online_weight:
					online_weight_handle = handle;
					break;
				case tables::meta:
					meta_handle = handle;
					break;
				case tables::peers:
					peers_handle = handle;
					break;
				case tables::pruned:
					pruned_handle = handle;
					break;
				case tables::confirmation_height:
					confirmation_height_handle = handle;
					break;
				case tables::final_votes:
					final_votes_handle = handle;
					break;
				case tables::rep_weights:
					rep_weights_handle = handle;
					break;
				default:
					release_assert (false, "Invalid table");
			}
		}
	}
	return status.code ();
}

std::unique_ptr<store::iterator> rocksdb_backend::begin (transaction const & tx, tables table) const
{
	return std::make_unique<store::iterator> (rocksdb::iterator::begin (db.get (), rocksdb::tx (tx), table_to_column_family (table)));
}

std::unique_ptr<store::iterator> rocksdb_backend::begin (transaction const & tx, tables table, db_val const & key) const
{
	auto & rocksdb_key = static_cast<nano::store::rocksdb::db_val const &> (key);
	return std::make_unique<store::iterator> (rocksdb::iterator::lower_bound (db.get (), rocksdb::tx (tx), table_to_column_family (table), rocksdb_key));
}

std::unique_ptr<store::iterator> rocksdb_backend::end (transaction const & tx, tables table) const
{
	return std::make_unique<store::iterator> (rocksdb::iterator::end (db.get (), rocksdb::tx (tx), table_to_column_family (table)));
}

bool rocksdb_backend::success (int status) const
{
	return status == ::rocksdb::Status::kOk;
}

bool rocksdb_backend::not_found (int status) const
{
	return status == ::rocksdb::Status::kNotFound;
}

std::string rocksdb_backend::error_string (int status) const
{
	return ::rocksdb::Status::CodeToString (static_cast<::rocksdb::Status::Code> (status));
}

write_transaction rocksdb_backend::tx_begin_write ()
{
	return write_transaction{ std::make_unique<rocksdb::write_transaction_impl> (db.get ()) };
}

read_transaction rocksdb_backend::tx_begin_read () const
{
	return read_transaction{ std::make_unique<rocksdb::read_transaction_impl> (db.get ()) };
}

bool rocksdb_backend::init_error () const
{
	return error;
}

bool rocksdb_backend::copy_db (std::filesystem::path const & destination)
{
	// RocksDB backup implementation would go here
	return false; // Not implemented for simplicity
}

void rocksdb_backend::rebuild_db (write_transaction const & tx)
{
	// RocksDB doesn't need explicit rebuilding like LMDB
}

std::string rocksdb_backend::vendor_get () const
{
	return "RocksDB";
}

std::filesystem::path rocksdb_backend::get_database_path () const
{
	return database_path;
}

nano::store::open_mode rocksdb_backend::get_mode () const
{
	return mode;
}

void rocksdb_backend::serialize_memory_stats (boost::property_tree::ptree & json)
{
	uint64_t val;
	if (db->GetIntProperty ("rocksdb.estimate-table-readers-mem", &val))
	{
		json.put ("table_readers_mem", val);
	}
	if (db->GetIntProperty ("rocksdb.cur-size-all-mem-tables", &val))
	{
		json.put ("all_memtables_mem", val);
	}
}
} // namespace nano::store