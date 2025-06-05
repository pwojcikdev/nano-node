#pragma once

#include <nano/store/db_val.hpp>
#include <nano/store/iterator.hpp>
#include <nano/store/tables.hpp>
#include <nano/store/transaction.hpp>

#include <memory>

namespace nano::store
{
/**
 * Polymorphic backend interface for key-value database operations.
 * This provides the minimal interface that database backends (LMDB, RocksDB) must implement.
 * All business logic should be in common implementation classes that use this interface.
 */
class backend
{
public:
	virtual ~backend () = default;

	// Basic CRUD operations
	virtual int get (transaction const & tx, tables table, db_val const & key, db_val & value) const = 0;
	virtual int put (write_transaction const & tx, tables table, db_val const & key, db_val const & value) = 0;
	virtual int del (write_transaction const & tx, tables table, db_val const & key) = 0;
	virtual bool exists (transaction const & tx, tables table, db_val const & key) const = 0;

	// Table operations
	virtual uint64_t count (transaction const & tx, tables table) const = 0;
	virtual int drop (write_transaction const & tx, tables table) = 0;

	// Iterator operations
	virtual store::iterator begin (transaction const & tx, tables table) const = 0;
	virtual store::iterator begin (transaction const & tx, tables table, db_val const & key) const = 0;
	virtual store::iterator end (transaction const & tx, tables table) const = 0;

	// Status checking
	virtual bool success (int status) const = 0;
	virtual bool not_found (int status) const = 0;
	virtual std::string error_string (int status) const = 0;

	// Transaction management
	virtual read_transaction tx_begin_read () const = 0;
	virtual write_transaction tx_begin_write () = 0;

	// Helper methods
	void release_assert_success (int status) const;
};

inline void backend::release_assert_success (int status) const
{
	if (!success (status))
	{
		release_assert (false, error_string (status));
	}
}
}