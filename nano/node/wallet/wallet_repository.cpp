#include <nano/node/wallet.hpp>
#include <nano/node/wallet/wallet_repository.hpp>
#include <nano/store/lmdb/common.hpp>
#include <nano/store/lmdb/db_val.hpp>
#include <nano/store/lmdb/iterator.hpp>
#include <nano/store/lmdb/utility.hpp>
#include <nano/store/typed_iterator_templ.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <cstring>
#include <sstream>
#include <stdexcept>

nano::wallet_repository::wallet_repository (nano::store::lmdb::env & env) :
	env{ env }
{
}

void nano::wallet_repository::open (nano::store::write_transaction const & wallet_txn, std::string const & wallet_path)
{
	debug_assert (strlen (wallet_path.c_str ()) == wallet_path.size ());

	MDB_dbi handle_l{};
	auto error = mdb_dbi_open (env.tx (wallet_txn), wallet_path.c_str (), MDB_CREATE, &handle_l);
	if (error != 0)
	{
		throw std::runtime_error ("Failed to open wallet database '" + wallet_path + "': " + nano::store::lmdb::error_string (error));
	}

	handle = handle_l;
}

void nano::wallet_repository::destroy (nano::store::write_transaction const & wallet_txn)
{
	release_assert (handle != 0);

	auto status = mdb_drop (env.tx (wallet_txn), handle, /* delete from database */ 1);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));

	handle = 0;
}

bool nano::wallet_repository::is_open () const
{
	return handle != 0;
}

void nano::wallet_repository::erase (nano::store::write_transaction const & wallet_txn, nano::account const & account)
{
	release_assert (handle != 0);

	nano::store::lmdb::db_val account_key{ account };
	auto mdb_account_key = nano::store::lmdb::to_mdb_val (account_key);
	auto status = mdb_del (env.tx (wallet_txn), handle, &mdb_account_key, nullptr);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

bool nano::wallet_repository::exists (nano::store::transaction const & wallet_txn, nano::account const & key) const
{
	release_assert (handle != 0);

	nano::store::lmdb::db_val db_key{ key };
	auto mdb_key = nano::store::lmdb::to_mdb_val (db_key);
	MDB_val value{};
	return mdb_get (env.tx (wallet_txn), handle, &mdb_key, &value) == 0;
}

nano::wallet_value nano::wallet_repository::get_raw (nano::store::transaction const & wallet_txn, nano::account const & account) const
{
	release_assert (handle != 0);

	nano::wallet_value result{};
	nano::store::lmdb::db_val value{};
	nano::store::lmdb::db_val account_key{ account };
	auto mdb_account_key = nano::store::lmdb::to_mdb_val (account_key);
	MDB_val mdb_value{};
	auto status = mdb_get (env.tx (wallet_txn), handle, &mdb_account_key, &mdb_value);
	if (status == 0)
	{
		value = nano::store::lmdb::from_mdb_val (mdb_value);
		result = nano::wallet_value{ value };
	}
	else
	{
		result.key.clear ();
		result.work = 0;
	}
	return result;
}

void nano::wallet_repository::put_raw (nano::store::write_transaction const & wallet_txn, nano::account const & account, nano::wallet_value const & entry)
{
	release_assert (handle != 0);

	nano::store::lmdb::db_val account_key{ account };
	nano::store::lmdb::db_val entry_val{ sizeof (entry), &entry };
	auto mdb_account_key = nano::store::lmdb::to_mdb_val (account_key);
	auto mdb_entry_val = nano::store::lmdb::to_mdb_val (entry_val);
	auto status = mdb_put (env.tx (wallet_txn), handle, &mdb_account_key, &mdb_entry_val, 0);
	release_assert (nano::store::lmdb::success (status), nano::store::lmdb::error_string (status));
}

auto nano::wallet_repository::find (nano::store::transaction const & wallet_txn, nano::account const & key) const -> iterator
{
	release_assert (handle != 0);

	auto it = begin (wallet_txn, key);
	auto end_it = end (wallet_txn);
	if (it == end_it)
	{
		return end_it;
	}
	if (it->first == key)
	{
		return it;
	}
	return end_it;
}

auto nano::wallet_repository::begin (nano::store::transaction const & wallet_txn) const -> iterator
{
	release_assert (handle != 0);

	nano::account account{ static_cast<uint64_t> (nano::wallet_store::special_count) };
	return iterator{ nano::store::iterator{ wallet_txn, nano::store::lmdb::iterator::lower_bound (env.tx (wallet_txn), handle, nano::store::lmdb::to_mdb_val (account)) } };
}

auto nano::wallet_repository::begin (nano::store::transaction const & wallet_txn, nano::account const & key) const -> iterator
{
	release_assert (handle != 0);

	nano::account account{ key };
	return iterator{ nano::store::iterator{ wallet_txn, nano::store::lmdb::iterator::lower_bound (env.tx (wallet_txn), handle, nano::store::lmdb::to_mdb_val (account)) } };
}

auto nano::wallet_repository::end (nano::store::transaction const & wallet_txn) const -> iterator
{
	release_assert (handle != 0);

	return iterator{ nano::store::iterator{ wallet_txn, nano::store::lmdb::iterator::end (env.tx (wallet_txn), handle) } };
}

std::string nano::wallet_repository::serialize_json (nano::store::transaction const & wallet_txn) const
{
	boost::property_tree::ptree tree;
	for (raw_iterator i{ nano::store::iterator{ wallet_txn, nano::store::lmdb::iterator::begin (env.tx (wallet_txn), handle) } }, n{ nano::store::iterator{ wallet_txn, nano::store::lmdb::iterator::end (env.tx (wallet_txn), handle) } }; i != n; ++i)
	{
		tree.put (i->first.to_string (), i->second.key.to_string ());
	}
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, tree);
	return ostream.str ();
}
