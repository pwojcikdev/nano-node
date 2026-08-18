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
#include <nano/node/wallet/wallet_store.hpp>
#include <nano/node/wallet/wallets_actions.hpp>
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

// A block prepared and signed against the wallet store, ready for work generation and processing
class prepared_block final
{
public:
	std::shared_ptr<nano::block> block; // nullptr when preparation failed
	nano::block_details details{};
};

// Result of preparing a send, which may resolve to a block already recorded for the send id
class prepared_send final
{
public:
	std::shared_ptr<nano::block> block; // nullptr when preparation failed
	nano::block_details details{};
	bool error{ false };
	bool cached{ false }; // block was recorded for the send id earlier and has already been processed
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

	// Store phase of the block actions: resolves state, fetches keys and signs under the mutex; work and processing happen in the actions component
	prepared_block prepare_receive (nano::wallet_id const &, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work);
	prepared_block prepare_change (nano::wallet_id const &, nano::account const & source, nano::account const & representative, uint64_t work);
	prepared_send prepare_send (nano::wallet_id const &, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, std::optional<std::string> send_id);
	// Stores generated work for the account if its frontier still matches root
	void update_work (nano::wallet_id const &, nano::account const &, nano::root const &, uint64_t work);

	// Notified with (true) before and (false) after each wallet action runs
	void set_observer (std::function<void (bool)> observer);

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
	nano::kdf kdf;

	mutable nano::mutex mutex;

	// Local representative tracking and voting key cache
	wallets_reps rep_tracker;
	// Periodic receivable scanning and confirmed receives
	wallets_receivable receivable_tracker;
	// Action queue and block operations
	wallets_actions action_runner;

	// Consecutive unused accounts scanned past the last used one before a seed scan gives up
	static uint32_t constexpr deterministic_check_gap{ 64 };

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
