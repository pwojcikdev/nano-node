#pragma once

#include <nano/lib/rocksdbconfig.hpp>
#include <nano/store/backend.hpp>

#include <functional>
#include <map>
#include <unordered_map>

#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <rocksdb/utilities/transaction_db.h>

namespace nano::store::rocksdb
{
/**
 * RocksDB implementation of the backend interface.
 * Provides low-level database operations using RocksDB.
 */
class backend_rocksdb : public nano::store::backend
{
public:
	backend_rocksdb (std::filesystem::path const & path, nano::rocksdb_config const & config);
	~backend_rocksdb () override;

	// CRUD operations
	int get (store::transaction const & tx, tables table, db_val const & key, db_val & value) const override;
	int put (store::write_transaction const & tx, tables table, db_val const & key, db_val const & value) override;
	int del (store::write_transaction const & tx, tables table, db_val const & key) override;
	bool exists (store::transaction const & tx, tables table, db_val const & key) const override;

	// Table operations
	uint64_t count (store::transaction const & tx, tables table) const override;
	int clear (store::write_transaction const & tx, tables table) override;
	int drop_table (store::write_transaction const & tx, tables table) override;
	bool table_exists (std::string const & name) const override;

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
	std::string get_database_path () const override;
	nano::store::open_mode get_mode () const override;

protected:
	void open_impl (column_schema schema, nano::store::open_mode mode) override;
	void close_impl () override;

private:
	std::filesystem::path database_path;
	nano::rocksdb_config config;

	std::unique_ptr<::rocksdb::DB> db;
	::rocksdb::TransactionDB * transaction_db{ nullptr };
	std::vector<std::unique_ptr<::rocksdb::ColumnFamilyHandle>> handles;

	std::map<tables, ::rocksdb::ColumnFamilyHandle *> table_handles;
	nano::store::open_mode current_mode{ nano::store::open_mode::read_write };

	// Tombstone management
	class tombstone_info
	{
	public:
		tombstone_info (uint64_t num, uint64_t max_a);
		std::atomic<uint64_t> num_since_last_flush;
		uint64_t const max;
	};
	std::unordered_map<tables, tombstone_info> tombstone_map;
	std::unordered_map<char const *, tables> cf_name_table_map;

	::rocksdb::ColumnFamilyHandle * table_to_column_family (tables table) const;
	::rocksdb::ColumnFamilyHandle * get_column_family (char const * name) const;
	bool column_family_exists (char const * name) const;

	::rocksdb::Options get_db_options ();
	::rocksdb::BlockBasedTableOptions get_table_options () const;
	::rocksdb::ColumnFamilyOptions get_cf_options (std::string const & cf_name) const;

	void open_db (std::filesystem::path const & path, bool read_only, ::rocksdb::Options const & options, std::vector<::rocksdb::ColumnFamilyDescriptor> column_families);
	std::vector<::rocksdb::ColumnFamilyDescriptor> create_column_families (column_schema const & schema);

	void generate_tombstone_map ();
	void flush_tombstones_check (tables table);
	void flush_table (tables table);
	void on_flush (::rocksdb::FlushJobInfo const & flush_info);

	std::unordered_map<char const *, tables> create_cf_name_table_map () const;
};
}
