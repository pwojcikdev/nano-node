#pragma once

#include <nano/lib/fan.hpp>
#include <nano/lib/kdf.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/result.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/lib/work.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/openclwork.hpp>
#include <nano/node/wallet/wallets_receivable.hpp>
#include <nano/node/wallet/wallets_reps.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/typed_iterator.hpp>
#include <nano/wallet/wallet_value.hpp>
#include <nano/wallet/wallets_backend.hpp>

#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

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

/**
 * Internal per-wallet state. All access is serialized by the owning `wallets` mutex;
 * the password fans inside `store` are additionally internally synchronized.
 */
class wallet_data final
{
public:
	wallet_data (nano::store::write_transaction &, wallets &, nano::wallet_id const &);
	wallet_data (nano::store::write_transaction &, wallets &, nano::wallet_id const &, std::string const & json);

	nano::wallet_id const id;
	nano::wallet::wallet_store store;
	// Canonical long-lived handle returned by open ()/create ()/all_wallets (); handles are stateless, so it may safely outlive destroy ()
	std::shared_ptr<wallet> handle;
	// Notified on password attempts with (invalid, password_empty)
	std::function<void (bool, bool)> lock_observer{ [] (bool, bool) {} };
};

/**
 * A wallet is a set of account keys encrypted by a common encryption key.
 * This handle holds no wallet state, only the id; every operation forwards to the
 * id-keyed `wallets` API, so a handle can never dangle: operations on a destroyed
 * wallet simply report `wallet_not_found`.
 */
class wallet final
{
public:
	wallet (nano::wallet::wallets &, nano::wallet_id const &);

	// Password and lock management
	void enter_initial_password ();
	bool enter_password (std::string const & password);
	bool rekey (std::string const & password);
	bool is_locked () const;
	void lock ();
	void set_lock_observer (std::function<void (bool, bool)> observer);

	// Account management
	nano::result<nano::public_key> insert_adhoc (nano::raw_key const & prv, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (uint32_t index, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (bool generate_work = true);
	bool insert_watch (nano::public_key const & pub);
	void remove_account (nano::account const & account);
	std::vector<nano::account> accounts () const;
	bool exists (nano::public_key const & pub) const;
	nano::result<bool> move_accounts (wallet & source, std::vector<nano::public_key> const & accounts);
	nano::wallet::key_type key_type (nano::account const & account) const;

	// Seed management
	nano::result<nano::raw_key> get_seed () const;
	nano::result<nano::public_key> change_seed (nano::raw_key const & seed, uint32_t count = 0);
	void deterministic_restore ();
	std::optional<uint32_t> deterministic_check (uint32_t index) const;
	uint32_t get_deterministic_index () const;

	// Representative management
	void set_representative (nano::account const & rep);
	nano::account get_representative () const;

	// Local wallet representatives
	std::unordered_set<nano::account> reps () const;

	// Key retrieval
	nano::result<nano::raw_key> fetch_prv (nano::account const & pub) const;

	// Block actions
	std::shared_ptr<nano::block> change_action (nano::account const & source, nano::account const & representative, uint64_t work = 0, bool generate_work = true);
	std::shared_ptr<nano::block> receive_action (nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work = 0, bool generate_work = true);
	std::shared_ptr<nano::block> send_action (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work = 0, bool generate_work = true, std::optional<std::string> send_id = {});

	// Sync/async block operations
	bool change_sync (nano::account const & source, nano::account const & representative);
	void change_async (nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	bool receive_sync (std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount);
	void receive_async (nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	nano::block_hash send_sync (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount);
	void send_async (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true, std::optional<std::string> send_id = {});

	// Work cache
	void work_cache_blocking (nano::account const & account, nano::root const & root);
	void work_ensure (nano::account const & account, nano::root const & root);
	nano::result<uint64_t> get_work (nano::public_key const &) const;
	void set_work (nano::public_key const & pub, uint64_t work);

	// Receivable
	bool search_receivable ();

	// Import/export
	bool import (std::string const & json, std::string const & password);
	void serialize_json (std::string & json) const;
	void write_backup (std::filesystem::path const & path) const;

	// The internally synchronized password fan, for tests and diagnostics; valid while the wallet exists
	nano::fan & password_fan ();

public:
	nano::wallet::wallets & wallets;
	nano::wallet_id const id;
};

// Spendable accounts and representative of a wallet, captured so scanning can run outside the wallets mutex
class wallet_scan_info final
{
public:
	nano::account representative;
	std::vector<nano::account> accounts;
};

/**
 * The wallets set is all the wallets a node controls.
 * A node may contain multiple wallets independently encrypted and operated.
 */
class wallets final
{
public:
	wallets (
	nano::node &,
	nano::wallet::wallets_backend &,
	nano::ledger &,
	nano::node_config const &,
	nano::network_params const &,
	nano::online_reps &,
	nano::network &,
	nano::stats &,
	nano::logger &);

	~wallets ();

	void start ();
	void stop ();

	// Wallet management
	std::shared_ptr<wallet> open (nano::wallet_id const &);
	std::shared_ptr<wallet> create (nano::wallet_id const &);
	std::shared_ptr<wallet> create_from_json (nano::wallet_id const &, std::string const & json);
	// Returns true if the wallet existed and was destroyed
	bool destroy (nano::wallet_id const &);
	void reload ();
	void clear_send_ids ();

	// Wallet queries, each returns a consistent snapshot
	std::unordered_map<nano::wallet_id, std::shared_ptr<wallet>> all_wallets ();
	std::vector<nano::wallet_id> wallet_ids () const;
	std::size_t wallet_count () const;

	// Account lookup
	bool exists (nano::account const &);
	bool exists_any (nano::account const &, nano::account const &);

	// Receivable, delegated to the receivable component
	bool search_receivable (nano::wallet_id const &);
	void search_receivable_all ();
	void receive_confirmed (nano::block_hash const & hash, nano::account const & destination);

	// Spendable accounts and representative of the wallet, errors when the wallet is missing or locked
	nano::result<wallet_scan_info> scan_info (nano::wallet_id const &) const;
	// Wallets containing the account, together with their configured representative
	std::vector<std::pair<nano::wallet_id, nano::account>> holders (nano::account const &) const;

public: // Id-keyed wallet operations; each reports `wallet_not_found` (or its return type's natural miss) when the wallet does not exist
	// Password and lock management
	void enter_initial_password (nano::wallet_id const &);
	bool enter_password (nano::wallet_id const &, std::string const & password);
	bool rekey (nano::wallet_id const &, std::string const & password);
	bool is_locked (nano::wallet_id const &) const;
	void lock (nano::wallet_id const &);
	void set_lock_observer (nano::wallet_id const &, std::function<void (bool, bool)> observer);

	// Account management
	nano::result<nano::public_key> insert_adhoc (nano::wallet_id const &, nano::raw_key const & prv, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (nano::wallet_id const &, uint32_t index, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (nano::wallet_id const &, bool generate_work = true);
	bool insert_watch (nano::wallet_id const &, nano::public_key const &);
	void remove_account (nano::wallet_id const &, nano::account const &);
	std::vector<nano::account> accounts (nano::wallet_id const &) const;
	bool exists (nano::wallet_id const &, nano::account const &) const;
	nano::result<bool> move_accounts (nano::wallet_id const & target, nano::wallet_id const & source, std::vector<nano::public_key> const & accounts);
	nano::wallet::key_type key_type (nano::wallet_id const &, nano::account const &) const;

	// Seed management
	nano::result<nano::raw_key> get_seed (nano::wallet_id const &) const;
	nano::result<nano::public_key> change_seed (nano::wallet_id const &, nano::raw_key const & seed, uint32_t count = 0);
	// Inserts accounts up to the highest one with ledger activity, does nothing when the wallet is locked
	void deterministic_restore (nano::wallet_id const &);
	// Scans accounts from index, returns the highest index with ledger activity, if any
	std::optional<uint32_t> deterministic_check (nano::wallet_id const &, uint32_t index) const;
	uint32_t get_deterministic_index (nano::wallet_id const &) const;

	// Representative management
	void set_representative (nano::wallet_id const &, nano::account const &);
	nano::account get_representative (nano::wallet_id const &) const;
	// Local representatives detected among the wallet's accounts
	std::unordered_set<nano::account> reps (nano::wallet_id const &) const;

	// Key retrieval
	nano::result<nano::raw_key> fetch_prv (nano::wallet_id const &, nano::account const &) const;

	// Block actions
	std::shared_ptr<nano::block> change_action (nano::wallet_id const &, nano::account const & source, nano::account const & representative, uint64_t work = 0, bool generate_work = true);
	std::shared_ptr<nano::block> receive_action (nano::wallet_id const &, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work = 0, bool generate_work = true);
	std::shared_ptr<nano::block> send_action (nano::wallet_id const &, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work = 0, bool generate_work = true, std::optional<std::string> send_id = {});

	// Sync/async block operations
	bool change_sync (nano::wallet_id const &, nano::account const & source, nano::account const & representative);
	void change_async (nano::wallet_id const &, nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	bool receive_sync (nano::wallet_id const &, std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount);
	void receive_async (nano::wallet_id const &, nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	nano::block_hash send_sync (nano::wallet_id const &, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount);
	void send_async (nano::wallet_id const &, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true, std::optional<std::string> send_id = {});

	// Work cache
	void work_cache_blocking (nano::wallet_id const &, nano::account const &, nano::root const &);
	void work_ensure (nano::wallet_id const &, nano::account const &, nano::root const &);
	nano::result<uint64_t> get_work (nano::wallet_id const &, nano::public_key const &) const;
	void set_work (nano::wallet_id const &, nano::public_key const &, uint64_t work);

	// Import/export
	bool import (nano::wallet_id const &, std::string const & json, std::string const & password);
	void serialize_json (nano::wallet_id const &, std::string & json) const;
	void write_backup (nano::wallet_id const &, std::filesystem::path const & path) const;

	// The wallet's internally synchronized password fan, for tests and diagnostics; the wallet must exist
	nano::fan & password_fan (nano::wallet_id const &);

	// Wallet actions queue
	void do_wallet_actions ();
	void queue_wallet_action (nano::uint128_t const & priority, std::shared_ptr<wallet> const &, std::function<void (wallet &)> action);

	// Representatives, delegated to the reps component
	void foreach_representative (std::function<void (nano::public_key const &, nano::raw_key const &)> const & action);
	void refresh_reps ();
	wallet_representatives reps () const;

	using signer_t = wallets_reps::signer_t;
	signer_t signer ();

	nano::container_info container_info () const;

private: // Transactions
	nano::store::write_transaction tx_begin_write ();
	nano::store::read_transaction tx_begin_read () const;

public: // Dependencies
	nano::node & node;
	nano::wallet::wallets_backend & backend;
	nano::ledger & ledger;
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::online_reps & online_reps;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

public:
	std::function<void (bool)> observer;

	std::multimap<nano::uint128_t, std::pair<std::shared_ptr<wallet>, std::function<void (wallet &)>>, std::greater<nano::uint128_t>> actions;
	nano::locked<std::unordered_map<nano::account, nano::root>> delayed_work;

	nano::kdf kdf;

	mutable nano::mutex mutex;
	mutable nano::mutex action_mutex;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };
	std::thread thread;

	nano::thread_pool workers;

	// Local representative tracking and voting key cache
	wallets_reps rep_tracker;
	// Periodic receivable scanning and confirmed receives
	wallets_receivable receivable_tracker;

	static nano::uint128_t const generate_priority;
	static nano::uint128_t const high_priority;

	// Consecutive unused accounts scanned past the last used one before a seed scan gives up
	static uint32_t constexpr deterministic_check_gap{ 64 };

private:
	// Queues an action against the wallet id; stale ids simply no-op when the action runs
	void queue_wallet_action (nano::uint128_t const & priority, nano::wallet_id const &, std::function<void (wallet &)> action);
	// Regenerates and processes the block (work + ledger), must be called without the mutex held
	bool action_complete (nano::wallet_id const &, std::shared_ptr<nano::block> const & block, nano::account const & account, bool generate_work, nano::block_details const & details);

private: // Per-wallet operations, each requires the mutex to be held
	wallet_data * find_wallet (nano::wallet_id const &) const;
	// Attempts to unlock with the password and queues a receivable search on success, returns true if the password was wrong
	bool enter_password_impl (wallet_data &, nano::store::transaction const &, std::string const & password);
	// Inserts the account at the stored deterministic index and advances it
	nano::public_key deterministic_insert_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, bool generate_work = true);
	// Inserts the account at an explicit index, leaving the stored index untouched
	nano::public_key deterministic_insert_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, uint32_t index, bool generate_work = true);
	// Caches work for an account, discarding it if the root is no longer the account frontier
	void work_update_impl (wallet_data &, nano::store::write_transaction const &, nano::account const &, nano::root const &, uint64_t work);
	// Scans accounts from index, returns the highest index with ledger activity, if any
	std::optional<uint32_t> deterministic_check_impl (wallet_data const &, nano::store::transaction const &, nano::wallet::wallet_cipher const &, uint32_t index) const;
	// Inserts accounts until every index up to and including last exists, returns the last account inserted
	std::optional<nano::public_key> deterministic_insert_up_to_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, uint32_t last);
	// Replaces the seed and inserts accounts 0..count, or up to the highest account in use when count is 0
	nano::public_key change_seed_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, nano::raw_key const & seed, uint32_t count = 0);

private:
	// All open wallets, protected by mutex
	std::unordered_map<nano::wallet_id, std::unique_ptr<wallet_data>> items;
};
}
