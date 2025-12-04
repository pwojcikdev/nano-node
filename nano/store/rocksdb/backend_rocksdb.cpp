#include <nano/lib/files.hpp>
#include <nano/store/rocksdb/backend_rocksdb.hpp>
#include <nano/store/rocksdb/iterator.hpp>
#include <nano/store/rocksdb/transaction_rocksdb.hpp>
#include <nano/store/rocksdb/utility.hpp>

#include <boost/format.hpp>

#include <set>

#include <rocksdb/filter_policy.h>
#include <rocksdb/slice.h>
#include <rocksdb/utilities/backup_engine.h>
#include <rocksdb/utilities/transaction.h>

namespace
{
class event_listener : public ::rocksdb::EventListener
{
public:
	event_listener (std::function<void (::rocksdb::FlushJobInfo const &)> const & flush_completed_cb_a) :
		flush_completed_cb (flush_completed_cb_a)
	{
	}

	void OnFlushCompleted (::rocksdb::DB * /* db_a */, ::rocksdb::FlushJobInfo const & flush_info_a) override
	{
		flush_completed_cb (flush_info_a);
	}

private:
	std::function<void (::rocksdb::FlushJobInfo const &)> flush_completed_cb;
};

// Checks if status indicates database/path doesn't exist
bool is_not_found (::rocksdb::Status const & status)
{
	if (status.IsNotFound ())
	{
		return true;
	}
	if (status.IsIOError () && status.subcode () == ::rocksdb::Status::kPathNotFound)
	{
		return true;
	}
	return false;
}
}

namespace nano::store::rocksdb
{
backend_rocksdb::backend_rocksdb (std::filesystem::path const & path, nano::rocksdb_config const & config_a) :
	database_path{ path },
	config{ config_a },
	cf_name_table_map{ create_cf_name_table_map () }
{
	boost::system::error_code error_mkdir, error_chmod;
	std::filesystem::create_directories (path, error_mkdir);
	nano::set_secure_perm_directory (path, error_chmod);

	if (error_mkdir)
	{
		throw std::runtime_error ("Failed to create database directory: " + path.string ());
	}

	generate_tombstone_map ();
}

backend_rocksdb::~backend_rocksdb ()
{
	close ();
}

void backend_rocksdb::open_impl (column_schema schema, nano::store::open_mode mode)
{
	current_mode = mode;

	auto options = get_db_options ();

	// Get existing column families from the database (if it exists)
	std::vector<std::string> existing_cf_names;
	auto list_status = ::rocksdb::DB::ListColumnFamilies (options, database_path.string (), &existing_cf_names);

	// If database doesn't exist or listing failed, use empty list (all column families will be created)
	if (!list_status.ok ())
	{
		if (is_not_found (list_status))
		{
			// Database doesn't exist yet, will be created
			existing_cf_names.clear ();
		}
		else
		{
			throw std::runtime_error ("Failed to list existing column families: " + list_status.ToString ());
		}
	}

	// Build column family descriptors - open ALL existing column families
	std::vector<::rocksdb::ColumnFamilyDescriptor> column_families;
	for (auto const & cf_name : existing_cf_names)
	{
		column_families.emplace_back (cf_name, get_cf_options (cf_name));
	}

	// If no existing column families, at least add default
	if (column_families.empty ())
	{
		column_families.emplace_back (::rocksdb::kDefaultColumnFamilyName, ::rocksdb::ColumnFamilyOptions{});
	}

	// Track which schema tables need to be created
	std::set<std::string> existing_cf_set (existing_cf_names.begin (), existing_cf_names.end ());
	std::vector<std::pair<tables, std::string>> missing_column_families;
	for (auto const & [table, name] : schema)
	{
		if (!existing_cf_set.contains (name))
		{
			missing_column_families.emplace_back (table, name);
		}
	}

	open_db (database_path, mode == nano::store::open_mode::read_only, options, column_families);

	// Create missing column families (only in write modes)
	if (mode != nano::store::open_mode::read_only)
	{
		for (auto const & [table, name] : missing_column_families)
		{
			::rocksdb::ColumnFamilyHandle * handle;
			auto status = db->CreateColumnFamily (get_cf_options (name), name, &handle);
			if (!status.ok ())
			{
				throw std::runtime_error ("Failed to create column family " + name + ": " + status.ToString ());
			}
			handles.emplace_back (handle);
		}
	}

	// Build table_handles map for schema tables
	for (auto const & [table, name] : schema)
	{
		table_handles[table] = get_column_family (name.c_str ());
	}
}

void backend_rocksdb::close_impl ()
{
	table_handles.clear ();
	handles.clear ();
	db.reset ();
	transaction_db = nullptr;
}

void backend_rocksdb::open_db (std::filesystem::path const & path, bool read_only, ::rocksdb::Options const & options, std::vector<::rocksdb::ColumnFamilyDescriptor> column_families)
{
	::rocksdb::Status s;

	std::vector<::rocksdb::ColumnFamilyHandle *> handles_l;
	if (read_only)
	{
		::rocksdb::DB * db_l;
		s = ::rocksdb::DB::OpenForReadOnly (options, path.string (), column_families, &handles_l, &db_l);
		db.reset (db_l);
	}
	else
	{
		s = ::rocksdb::TransactionDB::Open (options, ::rocksdb::TransactionDBOptions{}, path.string (), column_families, &handles_l, &transaction_db);
		if (transaction_db)
		{
			db.reset (transaction_db);
		}
	}

	handles.resize (handles_l.size ());
	for (size_t i = 0; i < handles_l.size (); ++i)
	{
		handles[i].reset (handles_l[i]);
	}

	if (!s.ok ())
	{
		if (is_not_found (s))
		{
			throw nano::error (nano::error_backend::db_not_found);
		}
		throw std::runtime_error ("Failed to open RocksDB database: " + s.ToString ());
	}
}

std::vector<::rocksdb::ColumnFamilyDescriptor> backend_rocksdb::create_column_families (column_schema const & schema)
{
	std::vector<::rocksdb::ColumnFamilyDescriptor> column_families;

	// Always include default column family
	column_families.emplace_back (::rocksdb::kDefaultColumnFamilyName, ::rocksdb::ColumnFamilyOptions{});

	for (auto const & [table, name] : schema)
	{
		column_families.emplace_back (name, get_cf_options (name));
	}
	return column_families;
}

std::unordered_map<char const *, tables> backend_rocksdb::create_cf_name_table_map () const
{
	std::unordered_map<char const *, tables> map{
		{ "accounts", tables::accounts },
		{ "blocks", tables::blocks },
		{ "pending", tables::pending },
		{ "online_weight", tables::online_weight },
		{ "meta", tables::meta },
		{ "peers", tables::peers },
		{ "confirmation_height", tables::confirmation_height },
		{ "pruned", tables::pruned },
		{ "final_votes", tables::final_votes },
		{ "rep_weights", tables::rep_weights }
	};
	return map;
}

::rocksdb::ColumnFamilyHandle * backend_rocksdb::table_to_column_family (tables table) const
{
	auto it = table_handles.find (table);
	release_assert (it != table_handles.end (), "table not found");
	return it->second;
}

::rocksdb::ColumnFamilyHandle * backend_rocksdb::get_column_family (char const * name) const
{
	auto iter = std::find_if (handles.begin (), handles.end (), [name] (auto & handle) {
		return handle->GetName () == name;
	});
	release_assert (iter != handles.end ());
	return iter->get ();
}

bool backend_rocksdb::column_family_exists (char const * name) const
{
	auto iter = std::find_if (handles.begin (), handles.end (), [name] (auto & handle) {
		return handle->GetName () == name;
	});
	return iter != handles.end ();
}

int backend_rocksdb::get (store::transaction const & tx, tables table, db_val const & key, db_val & value) const
{
	::rocksdb::ReadOptions options;
	::rocksdb::PinnableSlice slice;
	auto key_slice = to_slice (key);
	auto handle = table_to_column_family (table);
	auto internals = rocksdb::tx (tx);

	auto status = std::visit ([&] (auto && ptr) {
		using V = std::remove_cvref_t<decltype (ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			return ptr->Get (options, handle, key_slice, &slice);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			return db->Get (*ptr, handle, key_slice, &slice);
		}
		else
		{
			static_assert (sizeof (V) == 0, "Missing variant handler for type V");
		}
	},
	internals);

	if (status.ok ())
	{
		value = from_slice (slice);
	}
	return status.code ();
}

int backend_rocksdb::put (store::write_transaction const & tx, tables table, db_val const & key, db_val const & value)
{
	auto key_slice = to_slice (key);
	auto value_slice = to_slice (value);
	return std::get<::rocksdb::Transaction *> (rocksdb::tx (tx))->Put (table_to_column_family (table), key_slice, value_slice).code ();
}

int backend_rocksdb::del (store::write_transaction const & tx, tables table, db_val const & key)
{
	// RocksDB does not report not_found status, it is a pre-condition that the key exists
	debug_assert (exists (tx, table, key));
	flush_tombstones_check (table);
	auto key_slice = to_slice (key);
	return std::get<::rocksdb::Transaction *> (rocksdb::tx (tx))->Delete (table_to_column_family (table), key_slice).code ();
}

bool backend_rocksdb::exists (store::transaction const & tx, tables table, db_val const & key) const
{
	::rocksdb::PinnableSlice slice;
	auto key_slice = to_slice (key);
	auto internals = rocksdb::tx (tx);

	auto status = std::visit ([&] (auto && ptr) {
		using V = std::remove_cvref_t<decltype (ptr)>;
		if constexpr (std::is_same_v<V, ::rocksdb::Transaction *>)
		{
			::rocksdb::ReadOptions options;
			options.fill_cache = false;
			return ptr->Get (options, table_to_column_family (table), key_slice, &slice);
		}
		else if constexpr (std::is_same_v<V, ::rocksdb::ReadOptions *>)
		{
			return db->Get (*ptr, table_to_column_family (table), key_slice, &slice);
		}
		else
		{
			static_assert (sizeof (V) == 0, "Missing variant handler for type V");
		}
	},
	internals);

	return status.ok ();
}

uint64_t backend_rocksdb::count (store::transaction const & tx, tables table) const
{
	uint64_t sum = 0;

	// TODO: This should be configurable somewhere else, hardcoding this like that is not ideal
	// For small tables, iterate to get accurate counts
	if (table == tables::peers || table == tables::online_weight)
	{
		for (auto i = begin (tx, table), n = end (tx, table); i != n; ++i)
		{
			++sum;
		}
	}
	// Use estimate for pruned and final_votes
	else if (table == tables::pruned || table == tables::final_votes)
	{
		db->GetIntProperty (table_to_column_family (table), "rocksdb.estimate-num-keys", &sum);
	}
	// For other tables, iterate for accuracy (may be slow)
	else
	{
		for (auto i = begin (tx, table), n = end (tx, table); i != n; ++i)
		{
			++sum;
		}
	}

	return sum;
}

// TODO: Temporary impl, optimize this with DeleteRange
int backend_rocksdb::clear (store::write_transaction const & tx, tables table)
{
	auto col = table_to_column_family (table);

	::rocksdb::ReadOptions read_options;
	::rocksdb::WriteOptions write_options;
	::rocksdb::WriteBatch write_batch;
	std::unique_ptr<::rocksdb::Iterator> it (db->NewIterator (read_options, col));

	for (it->SeekToFirst (); it->Valid (); it->Next ())
	{
		write_batch.Delete (col, it->key ());
	}

	::rocksdb::Status status = db->Write (write_options, &write_batch);
	release_assert (status.ok (), error_string (status.code ()));

	return status.code ();
}

bool backend_rocksdb::drop_table (store::write_transaction const & tx, std::string const & name)
{
	if (!column_family_exists (name.c_str ()))
	{
		return false; // Table doesn't exist
	}

	auto const handle = get_column_family (name.c_str ());

	auto status1 = db->DropColumnFamily (handle);
	release_assert (success (status1.code ()), error_string (status1.code ()));

	auto status2 = db->DestroyColumnFamilyHandle (handle);
	release_assert (success (status2.code ()), error_string (status2.code ()));

	// Remove from handles vector
	std::erase_if (handles, [handle] (auto & h) {
		if (h.get () == handle)
		{
			// The handle resource is deleted by RocksDB
			[[maybe_unused]] auto ptr = h.release ();
			return true;
		}
		return false;
	});

	// Remove from table_handles if it was tracked
	std::erase_if (table_handles, [handle] (auto const & pair) {
		return pair.second == handle;
	});

	return true;
}

bool backend_rocksdb::table_exists (store::transaction const & tx, std::string const & name) const
{
	return column_family_exists (name.c_str ());
}

store::iterator backend_rocksdb::begin (store::transaction const & tx, tables table) const
{
	return store::iterator{ iterator::begin (db.get (), rocksdb::tx (tx), table_to_column_family (table)) };
}

store::iterator backend_rocksdb::begin (store::transaction const & tx, tables table, db_val const & key) const
{
	auto key_slice = to_slice (key);
	return store::iterator{ iterator::lower_bound (db.get (), rocksdb::tx (tx), table_to_column_family (table), key_slice) };
}

store::iterator backend_rocksdb::end (store::transaction const & tx, tables table) const
{
	return store::iterator{ iterator::end (db.get (), rocksdb::tx (tx), table_to_column_family (table)) };
}

bool backend_rocksdb::success (int status) const
{
	return static_cast<int> (::rocksdb::Status::Code::kOk) == status;
}

bool backend_rocksdb::not_found (int status) const
{
	return static_cast<int> (::rocksdb::Status::Code::kNotFound) == status;
}

std::string backend_rocksdb::error_string (int status) const
{
	return "status: " + std::to_string (status);
}

store::read_transaction backend_rocksdb::tx_begin_read () const
{
	return store::read_transaction{ std::make_unique<nano::store::rocksdb::read_transaction_impl> (db.get ()) };
}

store::write_transaction backend_rocksdb::tx_begin_write ()
{
	release_assert (transaction_db != nullptr);
	return store::write_transaction{ std::make_unique<nano::store::rocksdb::write_transaction_impl> (transaction_db) };
}

void backend_rocksdb::backup ()
{
	std::unique_ptr<::rocksdb::BackupEngine> backup_engine;
	::rocksdb::BackupEngine * backup_engine_raw;
	auto backup_path = database_path.parent_path () / "backup";
	::rocksdb::BackupEngineOptions backup_options (backup_path.string ());
	backup_options.share_table_files = true;
	backup_options.max_background_operations = std::thread::hardware_concurrency ();

	auto status = ::rocksdb::BackupEngine::Open (::rocksdb::Env::Default (), backup_options, &backup_engine_raw);
	backup_engine.reset (backup_engine_raw);
	if (!status.ok ())
	{
		throw std::runtime_error ("Failed to open backup engine: " + status.ToString ());
	}

	status = backup_engine->CreateNewBackup (db.get ());
	if (!status.ok ())
	{
		throw std::runtime_error ("Failed to create backup: " + status.ToString ());
	}
}

bool backend_rocksdb::copy_db (std::filesystem::path const & destination_path)
{
	std::unique_ptr<::rocksdb::BackupEngine> backup_engine;
	{
		::rocksdb::BackupEngine * backup_engine_raw;
		::rocksdb::BackupEngineOptions backup_options (destination_path.string ());
		backup_options.share_table_files = true;
		backup_options.max_background_operations = std::thread::hardware_concurrency ();
		auto status = ::rocksdb::BackupEngine::Open (::rocksdb::Env::Default (), backup_options, &backup_engine_raw);
		backup_engine.reset (backup_engine_raw);
		if (!status.ok ())
		{
			return false;
		}
	}

	auto status = backup_engine->CreateNewBackup (db.get ());
	if (!status.ok ())
	{
		return false;
	}

	std::vector<::rocksdb::BackupInfo> backup_infos;
	backup_engine->GetBackupInfo (&backup_infos);

	for (auto const & backup_info : backup_infos)
	{
		status = backup_engine->VerifyBackup (backup_info.backup_id);
		if (!status.ok ())
		{
			return false;
		}
	}

	{
		std::unique_ptr<::rocksdb::BackupEngineReadOnly> backup_engine_read;
		{
			::rocksdb::BackupEngineReadOnly * backup_engine_read_raw;
			status = ::rocksdb::BackupEngineReadOnly::Open (::rocksdb::Env::Default (), ::rocksdb::BackupEngineOptions (destination_path.string ()), &backup_engine_read_raw);
		}
		if (!status.ok ())
		{
			return false;
		}

		// First remove all files (not directories) in the destination
		for (auto const & path : std::filesystem::directory_iterator (destination_path))
		{
			if (std::filesystem::is_regular_file (path))
			{
				std::filesystem::remove (path);
			}
		}

		// Now generate the relevant files from the backup
		status = backup_engine->RestoreDBFromLatestBackup (destination_path.string (), destination_path.string ());
	}

	// Open it so that it flushes all WAL files
	if (status.ok ())
	{
		try
		{
			backend_rocksdb temp_backend (destination_path, config);
			// Opening a database causes WAL to be flushed
			return true;
		}
		catch (std::exception const &)
		{
			return false;
		}
	}
	return false;
}

std::string backend_rocksdb::vendor_get () const
{
	return boost::str (boost::format ("RocksDB %1%.%2%.%3%") % ROCKSDB_MAJOR % ROCKSDB_MINOR % ROCKSDB_PATCH);
}

std::string backend_rocksdb::get_database_path () const
{
	return database_path.string ();
}

nano::store::open_mode backend_rocksdb::get_mode () const
{
	return current_mode;
}

::rocksdb::Options backend_rocksdb::get_db_options ()
{
	::rocksdb::Options db_options;
	db_options.create_if_missing = true;
	db_options.create_missing_column_families = true;

	db_options.OptimizeLevelStyleCompaction ();
	db_options.IncreaseParallelism (config.io_threads);
	db_options.compression = ::rocksdb::kNoCompression;

	auto event_listener_l = new event_listener ([this] (::rocksdb::FlushJobInfo const & flush_job_info) {
		this->on_flush (flush_job_info);
	});
	db_options.listeners.emplace_back (event_listener_l);

	return db_options;
}

::rocksdb::BlockBasedTableOptions backend_rocksdb::get_table_options () const
{
	::rocksdb::BlockBasedTableOptions table_options;

	table_options.data_block_index_type = ::rocksdb::BlockBasedTableOptions::DataBlockIndexType::kDataBlockBinaryAndHash;
	table_options.format_version = 5;
	table_options.block_cache = ::rocksdb::NewLRUCache (config.read_cache * 1024 * 1024);
	table_options.filter_policy.reset (::rocksdb::NewBloomFilterPolicy (10, false));

	return table_options;
}

::rocksdb::ColumnFamilyOptions backend_rocksdb::get_cf_options (std::string const & cf_name) const
{
	::rocksdb::ColumnFamilyOptions cf_options;
	if (cf_name != ::rocksdb::kDefaultColumnFamilyName)
	{
		std::shared_ptr<::rocksdb::TableFactory> table_factory (::rocksdb::NewBlockBasedTableFactory (get_table_options ()));
		cf_options.table_factory = table_factory;
		cf_options.write_buffer_size = config.write_cache * 1024 * 1024;
	}
	return cf_options;
}

void backend_rocksdb::generate_tombstone_map ()
{
	tombstone_map.emplace (std::piecewise_construct, std::forward_as_tuple (tables::blocks), std::forward_as_tuple (0, 25000));
	tombstone_map.emplace (std::piecewise_construct, std::forward_as_tuple (tables::accounts), std::forward_as_tuple (0, 25000));
	tombstone_map.emplace (std::piecewise_construct, std::forward_as_tuple (tables::pending), std::forward_as_tuple (0, 25000));
}

void backend_rocksdb::flush_tombstones_check (tables table)
{
	if (auto it = tombstone_map.find (table); it != tombstone_map.end ())
	{
		auto & tombstone_info = it->second;
		if (++tombstone_info.num_since_last_flush > tombstone_info.max)
		{
			tombstone_info.num_since_last_flush = 0;
			flush_table (table);
		}
	}
}

void backend_rocksdb::flush_table (tables table)
{
	db->Flush (::rocksdb::FlushOptions{}, table_to_column_family (table));
}

void backend_rocksdb::on_flush (::rocksdb::FlushJobInfo const & flush_info)
{
	if (auto it = cf_name_table_map.find (flush_info.cf_name.c_str ()); it != cf_name_table_map.end ())
	{
		if (auto tomb_it = tombstone_map.find (it->second); tomb_it != tombstone_map.end ())
		{
			tomb_it->second.num_since_last_flush = 0;
		}
	}
}

backend_rocksdb::tombstone_info::tombstone_info (uint64_t num, uint64_t max_a) :
	num_since_last_flush (num),
	max (max_a)
{
}
}
