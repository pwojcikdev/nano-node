#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/store/lmdb/wallet_value.hpp>
#include <nano/store/typed_iterator.hpp>

#include <string>

namespace nano
{
class wallet_repository final
{
public:
	using iterator = store::typed_iterator<nano::account, nano::wallet_value>;
	using raw_iterator = nano::store::typed_iterator<nano::uint256_union, nano::wallet_value>;

public:
	explicit wallet_repository (nano::store::lmdb::env &);

	void open (nano::store::write_transaction const & wallet_txn, std::string const & wallet_path);
	void destroy (nano::store::write_transaction const & wallet_txn);
	void erase (nano::store::write_transaction const & wallet_txn, nano::account const & account);
	bool is_open () const;

	nano::wallet_value get_raw (nano::store::transaction const & wallet_txn, nano::account const & account) const;
	void put_raw (nano::store::write_transaction const & wallet_txn, nano::account const & account, nano::wallet_value const & entry);

	bool exists (nano::store::transaction const & wallet_txn, nano::account const & key) const;

	iterator find (nano::store::transaction const & wallet_txn, nano::account const & key) const;
	iterator begin (nano::store::transaction const & wallet_txn, nano::account const & key) const;
	iterator begin (nano::store::transaction const & wallet_txn) const;
	iterator end (nano::store::transaction const & wallet_txn) const;

	std::string serialize_json (nano::store::transaction const & wallet_txn) const;

private:
	nano::store::lmdb::env & env;
	MDB_dbi handle{ 0 };
};
}
