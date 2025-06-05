#pragma once

#include <nano/boost/outcome.hpp>
#include <nano/store/common.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/iterator.hpp>
#include <nano/store/tables.hpp>
#include <nano/store/transaction.hpp>

#include <map>
#include <memory>
#include <string>

namespace nano::store
{
class backend_table
{
};

enum class backend_status
{
	success,
	not_found,
	failure
};

struct backend_error
{
	backend_status status;


};

struct backend_meta
{
	uint64_t version;
};

/**
 * Polymorphic backend interface for key-value database operations.
 * This provides the minimal interface that database backends (LMDB, RocksDB) must implement.
 * All business logic should be in common implementation classes that use this interface.
 */
class backend
{
public:
	using column_definitions = std::map<tables, std::string>;

public:
	virtual ~backend () = default;

	virtual outcome::result<backend_meta, backend_error> meta () = 0;
	virtual backend_status open (nano::store::open_mode mode, column_definitions) = 0;

	// Basic CRUD operations
	virtual int get (store::transaction const & tx, tables table, db_val const & key, db_val & value) const = 0;
	virtual int put (store::write_transaction const & tx, tables table, db_val const & key, db_val const & value) = 0;
	virtual int del (store::write_transaction const & tx, tables table, db_val const & key) = 0;
	virtual bool exists (store::transaction const & tx, tables table, db_val const & key) const = 0;

	// Table operations
	virtual uint64_t count (store::transaction const & tx, tables table) const = 0;
	virtual int drop (store::write_transaction const & tx, tables table) = 0;

	// Iterator operations
	virtual store::iterator begin (store::transaction const & tx, tables table) const = 0;
	virtual store::iterator begin (store::transaction const & tx, tables table, db_val const & key) const = 0;
	virtual store::iterator end (store::transaction const & tx, tables table) const = 0;

	// Status checking
	virtual bool success (int status) const = 0;
	virtual bool not_found (int status) const = 0;
	virtual std::string error_string (int status) const = 0;

	// Transaction management
	virtual store::read_transaction tx_begin_read () const = 0;
	virtual store::write_transaction tx_begin_write () = 0;

	// Helper methods
	void release_assert_success (int status) const;

	std::string vendor_get () const;
	std::filesystem::path get_database_path () const;
	nano::store::open_mode get_mode () const;
};

inline void backend::release_assert_success (int status) const
{
	if (!success (status))
	{
		release_assert (false, error_string (status));
	}
}
}