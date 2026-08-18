#pragma once

#include <nano/lib/fan.hpp>
#include <nano/lib/kdf.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/result.hpp>
#include <nano/node/fwd.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/wallet/wallet_value.hpp>
#include <nano/wallet/wallets_backend.hpp>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nano::wallet
{
enum class key_type
{
	not_a_type,
	unknown,
	adhoc,
	deterministic
};

class wallet_store;

/**
 * Validated handle for wallet-key crypto. The only way to obtain one is via wallet_store::unlock(), which has therefore already validated the password.
 * The cipher exposes encrypt/decrypt but never the underlying wallet key, so it is impossible to encrypt or decrypt with an unvalidated key.
 */
class wallet_cipher final
{
public:
	nano::raw_key encrypt (nano::raw_key const & plaintext, nano::uint128_union const & iv) const;
	nano::raw_key decrypt (nano::uint256_union const & ciphertext, nano::uint128_union const & iv) const;

	// Re-encrypt the wallet key under a new password key. Used by rekey.
	nano::raw_key reseal (nano::raw_key const & new_password_key, nano::uint128_union const & iv) const;

private:
	friend class wallet_store;
	wallet_cipher (wallet_store const & issuer, nano::raw_key wallet_key);

	// Issuing store, a cipher must never be used with a different wallet
	wallet_store const * issuer;
	nano::raw_key wallet_key;
};

class wallet_store final
{
public:
	using iterator = store::typed_iterator<nano::account, nano::wallet::wallet_value>;

public:
	wallet_store (nano::kdf &, nano::store::write_transaction &, nano::wallet::wallets_backend &, nano::account representative, unsigned fanout, std::string const & wallet_path);
	wallet_store (nano::kdf &, nano::store::write_transaction &, nano::wallet::wallets_backend &, unsigned fanout, std::string const & wallet_path, std::string const & json);

	// Returns a cipher if the password decrypts the wallet key correctly.
	// The cipher is the only way to encrypt/decrypt account data.
	std::optional<nano::wallet::wallet_cipher> unlock (nano::store::transaction const &) const;

	std::vector<nano::account> accounts (nano::store::transaction const &) const;
	bool rekey (nano::store::write_transaction const &, std::string const & password);
	bool valid_password (nano::store::transaction const &) const;
	bool valid_public_key (nano::public_key const &) const;
	bool attempt_password (nano::store::transaction const &, std::string const & password);
	// Atomically clears the password, locking the wallet; serialized against rekey and attempt_password
	void password_clear ();
	nano::raw_key seed (nano::store::transaction const &, nano::wallet::wallet_cipher const &) const;
	void seed_set (nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, nano::raw_key const & seed);
	nano::wallet::key_type key_type (nano::wallet::wallet_value const &) const;
	// Allocates the next account: inserts at the stored index and advances it, skipping indexes whose accounts already exist
	nano::public_key deterministic_insert (nano::store::write_transaction const &, nano::wallet::wallet_cipher const &);
	// Materializes one account at an explicit index, leaving the stored index alone so sequential allocation still fills the indexes below it
	nano::public_key deterministic_insert (nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, uint32_t index);
	nano::raw_key deterministic_key (nano::store::transaction const &, nano::wallet::wallet_cipher const &, uint32_t index) const;
	uint32_t deterministic_index_get (nano::store::transaction const &) const;
	void deterministic_index_set (nano::store::write_transaction const &, uint32_t index);
	void deterministic_clear (nano::store::write_transaction const &);
	nano::uint256_union salt_get (nano::store::transaction const &) const;
	nano::uint256_union check_value_get (nano::store::transaction const &) const;
	bool is_representative (nano::store::transaction const &) const;
	nano::account representative (nano::store::transaction const &) const;
	void representative_set (nano::store::write_transaction const &, nano::account const & rep);
	nano::public_key insert_adhoc (nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, nano::raw_key const & prv);
	bool insert_watch (nano::store::write_transaction const &, nano::account const & pub);
	void erase (nano::store::write_transaction const &, nano::account const & pub);
	nano::wallet::wallet_value entry_get_raw (nano::store::transaction const &, nano::account const & pub) const;
	void entry_put_raw (nano::store::write_transaction const &, nano::account const & pub, nano::wallet::wallet_value const & entry);
	nano::result<nano::raw_key> fetch (nano::store::transaction const &, nano::account const & pub) const;
	nano::result<nano::raw_key> fetch (nano::store::transaction const &, nano::wallet::wallet_cipher const &, nano::account const & pub) const;
	bool exists (nano::store::transaction const &, nano::account const & pub) const;
	void destroy (nano::store::write_transaction const &);
	iterator find (nano::store::transaction const &, nano::account const & key) const;
	iterator begin (nano::store::transaction const &, nano::account const & key) const;
	iterator begin (nano::store::transaction const &) const;
	iterator end (nano::store::transaction const &) const;
	nano::raw_key derive_key (nano::store::transaction const &, std::string const & password) const;
	void serialize_json (nano::store::transaction const &, std::string & json) const;
	void write_backup (nano::store::transaction const &, std::filesystem::path const & path) const;
	nano::result<bool> move (nano::store::write_transaction const &, wallet_store & source, std::vector<nano::public_key> const & keys);
	nano::result<bool> import (nano::store::write_transaction const &, wallet_store & source);
	std::optional<uint64_t> work_get (nano::store::transaction const &, nano::public_key const &) const;
	void work_put (nano::store::write_transaction const &, nano::public_key const & pub, uint64_t work);
	unsigned version (nano::store::transaction const &) const;
	void version_put (nano::store::write_transaction const &, unsigned version);

public:
	nano::fan password;
	nano::fan wallet_key_mem;
	nano::kdf & kdf;
	nano::locked<nano::wallet::wallet_handle> handle;

private:
	// Serializes compound password/wallet-key operations; the fans are individually synchronized already
	mutable std::recursive_mutex mutex;

private:
	// Decrypts the wallet key using whatever the live password fan currently is.
	// Used only by unlock() and during construction; callers outside wallet_store
	// must go through unlock() so that the password is validated first.
	nano::raw_key wallet_key_decrypt (nano::store::transaction const &) const;

	// Decrypts the wallet seed using an already-unlocked cipher.
	// Centralises the seed_special / seed_iv_index plumbing so callers can't drift.
	nano::raw_key seed_decrypt (nano::store::transaction const &, nano::wallet::wallet_cipher const &) const;

private:
	nano::wallet::wallets_backend & backend;

public:
	static unsigned const version_1 = 1;
	static unsigned const version_2 = 2;
	static unsigned const version_3 = 3;
	static unsigned const version_4 = 4;
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
