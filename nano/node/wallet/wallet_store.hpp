#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/result.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/wallet/wallet_repository.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>
#include <nano/store/lmdb/wallet_value.hpp>
#include <nano/store/typed_iterator.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nano
{
/*
 * The fan spreads a key out over the heap to decrease the likelihood of it being recovered by memory inspection
 */
class fan final
{
public:
	fan (nano::raw_key const & key, std::size_t count);
	void value (nano::raw_key & result) const;
	void value_set (nano::raw_key const & value);
	std::vector<std::unique_ptr<nano::raw_key>> values;

private:
	mutable nano::mutex mutex;
	void value_get (nano::raw_key & result) const;
};

/*
 * Key derivation function using password hashing scheme (PHS) to derive encryption keys from passwords
 */
class kdf final
{
public:
	kdf (unsigned const & kdf_work) :
		kdf_work{ kdf_work }
	{
	}
	void phs (nano::raw_key & result, std::string const & password, nano::uint256_union const & salt);
	nano::mutex mutex;
	unsigned const & kdf_work;
};

enum class key_type
{
	not_a_type,
	unknown,
	adhoc,
	deterministic
};

class wallet_store final
{
public:
	using iterator = store::typed_iterator<nano::account, nano::wallet_value>;

public:
	wallet_store (nano::kdf & kdf, store::lmdb::env & env, unsigned fanout);

	void create (nano::store::write_transaction const & txn_wallet, nano::account representative, std::string const & wallet_path);
	void create_from_json (nano::store::write_transaction const & txn_wallet, std::string const & wallet_path, std::string const & json);
	void open (nano::store::write_transaction const & txn_wallet, std::string const & wallet_path);

	std::vector<nano::account> accounts (nano::store::transaction const & txn_wallet) const;
	nano::uint256_union check (nano::store::transaction const & txn_wallet) const;
	bool rekey (nano::store::write_transaction const & txn_wallet, std::string const & password_text);
	bool valid_password (nano::store::transaction const & txn_wallet) const;
	bool valid_public_key (nano::public_key const &) const;
	bool attempt_password (nano::store::transaction const & txn_wallet, std::string const & password_text);
	void wallet_key (nano::raw_key & result, nano::store::transaction const & txn_wallet) const;
	nano::raw_key seed (nano::store::transaction const & txn_wallet) const;
	void seed_set (nano::store::write_transaction const & txn_wallet, nano::raw_key const & seed);
	nano::key_type key_type (nano::store::transaction const & txn_wallet, nano::account const & account) const;
	nano::key_type key_type (nano::wallet_value const & value) const;
	nano::public_key deterministic_insert (nano::store::write_transaction const & txn_wallet);
	nano::public_key deterministic_insert (nano::store::write_transaction const & txn_wallet, uint32_t index);
	nano::raw_key deterministic_key (nano::store::transaction const & txn_wallet, uint32_t index) const;
	uint32_t deterministic_index_get (nano::store::transaction const & txn_wallet) const;
	void deterministic_index_set (nano::store::write_transaction const & txn_wallet, uint32_t index);
	void deterministic_clear (nano::store::write_transaction const & txn_wallet);
	nano::uint256_union salt (nano::store::transaction const & txn_wallet) const;
	bool is_representative (nano::store::transaction const & txn_wallet) const;
	nano::account representative (nano::store::transaction const & txn_wallet) const;
	void representative_set (nano::store::write_transaction const & txn_wallet, nano::account const & representative);
	nano::public_key insert_adhoc (nano::store::write_transaction const & txn_wallet, nano::raw_key const & prv);
	bool insert_watch (nano::store::write_transaction const & txn_wallet, nano::account const & account);
	void erase (nano::store::write_transaction const & txn_wallet, nano::account const & account);
	nano::result<nano::raw_key> fetch (nano::store::transaction const & txn_wallet, nano::account const & account) const;
	bool exists (nano::store::transaction const & txn_wallet, nano::account const & account) const;
	void destroy (nano::store::write_transaction const & txn_wallet);
	iterator find (nano::store::transaction const & txn_wallet, nano::account const & key) const;
	iterator begin (nano::store::transaction const & txn_wallet, nano::account const & key) const;
	iterator begin (nano::store::transaction const & txn_wallet) const;
	iterator end (nano::store::transaction const & txn_wallet) const;
	void derive_key (nano::raw_key & result, nano::store::transaction const & txn_wallet, std::string const & password_text) const;
	std::string serialize_json (nano::store::transaction const & txn_wallet) const;
	void write_backup (nano::store::transaction const & txn_wallet, std::filesystem::path const & path) const;
	bool move (nano::store::write_transaction const & txn_wallet, nano::wallet_store & source, std::vector<nano::public_key> const & keys);
	bool import (nano::store::write_transaction const & txn_wallet, nano::wallet_store & source);
	bool is_open () const;
	std::optional<uint64_t> work_get (nano::store::transaction const & txn_wallet, nano::public_key const & account) const;
	void work_put (nano::store::write_transaction const & txn_wallet, nano::public_key const & account, uint64_t work);
	unsigned version (nano::store::transaction const & txn_wallet) const;
	void version_put (nano::store::write_transaction const & txn_wallet, unsigned version);

public:
	nano::fan password;
	nano::fan wallet_key_mem;
	nano::kdf & kdf;
	mutable std::recursive_mutex mutex;

private:
	nano::wallet_repository repository;

	void ensure_required_entries (nano::store::transaction const & txn_wallet) const;

public:
	static unsigned constexpr version_1 = 1;
	static unsigned constexpr version_2 = 2;
	static unsigned constexpr version_3 = 3;
	static unsigned constexpr version_4 = 4;
	static unsigned constexpr version_current = version_4;
	static nano::account const version_special;
	static nano::account const wallet_key_special;
	static nano::account const salt_special;
	static nano::account const check_special;
	static nano::account const representative_special;
	static nano::account const seed_special;
	static nano::account const deterministic_index_special;
	static std::size_t const check_iv_index;
	static std::size_t const seed_iv_index;
	static int const special_count;
	static std::string const default_password;
};
}
