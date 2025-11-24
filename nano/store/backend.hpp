#pragma once

#include <nano/lib/errors.hpp>
#include <nano/lib/result.hpp>
#include <nano/store/common.hpp>
#include <nano/store/db_val.hpp>
#include <nano/store/iterator.hpp>
#include <nano/store/tables.hpp>
#include <nano/store/transaction.hpp>

#include <map>
#include <memory>
#include <set>
#include <string>

namespace nano
{
enum class error_backend
{
	generic = 1,
	db_not_found,
	table_not_found,
	failure,
};
}
REGISTER_ERROR_CODES (nano, error_backend)

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

using version_t = uint64_t;

struct backend_meta
{
	version_t version;
};

using column_definition = std::pair<tables, std::string>;
using column_schema = std::set<column_definition>;

/**
 * Polymorphic backend interface for key-value database operations.
 * This provides the minimal interface that database backends (LMDB, RocksDB) must implement.
 * All business logic should be in common implementation classes that use this interface.
 */
class backend
{
public:
	virtual ~backend () = default;

	backend_meta open_meta ();

	void open (column_schema, nano::store::open_mode mode);
	void create (column_schema);
	void close ();

	virtual void backup () = 0;
	virtual bool copy_db (std::filesystem::path const & destination) = 0;

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

	virtual std::string vendor_get () const;
	virtual std::filesystem::path get_database_path () const;
	virtual nano::store::open_mode get_mode () const;

	backend_meta get_meta () const;

	nano::store::version_t get_version (store::transaction const &) const;
	void set_version (store::write_transaction const &, nano::store::version_t version);

protected:
	virtual void open_impl (column_schema, nano::store::open_mode) = 0;
	virtual void close_impl () = 0;

private:
	void load_meta ();

private:
	bool is_open{ false };
	std::optional<backend_meta> current_meta;

public:
	static nano::store::column_schema const schema_meta;
};

inline void backend::release_assert_success (int status) const
{
	if (!success (status))
	{
		release_assert (false, error_string (status));
	}
}
}

namespace nano::store
{
class version_store
{
public:
	explicit version_store (store::backend &);

	void put_version (store::write_transaction const & transaction, uint64_t version);
	uint64_t get_version (store::transaction const & transaction) const;

private:
	store::backend & backend;

private:
	static uint64_t constexpr version_key{ 1 };
};
}