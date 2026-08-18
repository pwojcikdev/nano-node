#pragma once

#include <nano/lib/fan.hpp>
#include <nano/lib/kdf.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/result.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/wallet/wallet_store.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano::wallet
{
/**
 * Internal per-wallet state. All access is serialized by the owning `wallets_core` mutex;
 * the password fans inside `store` are additionally internally synchronized.
 */
class wallet_data final
{
public:
	wallet_data (nano::store::write_transaction &, wallets_core &, nano::wallet_id const &);
	wallet_data (nano::store::write_transaction &, wallets_core &, nano::wallet_id const &, std::string const & json);

	nano::wallet_id const id;
	nano::wallet::wallet_store store;
	// Canonical long-lived handle returned by open ()/create ()/all_wallets (); handles are stateless, so it may safely outlive destroy ()
	std::shared_ptr<wallet> handle;
	// Notified on password attempts with (invalid, password_empty)
	std::function<void (bool, bool)> lock_observer{ [] (bool, bool) {} };
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
 * The wallet store core: owns the open wallet set and serializes all wallet store access under a single mutex.
 * Pure storage and signing; work generation, ledger processing, scanning and rep tracking live in the sibling components.
 * Write transactions are always acquired before the mutex so commit fsyncs never happen inside the critical section.
 */
class wallets_core final
{
public:
	wallets_core (wallets &, nano::wallet::wallets_backend &, nano::ledger &, nano::node_config const &, nano::network_params const &, nano::logger &);

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

	// Password and lock management
	// Attempts the default password on a fresh password fan, returns true if the wallet became unlocked
	bool enter_initial_password (nano::wallet_id const &);
	bool enter_password (nano::wallet_id const &, std::string const & password);
	bool rekey (nano::wallet_id const &, std::string const & password);
	bool is_locked (nano::wallet_id const &) const;
	// Clears the password, returns true if the wallet existed
	bool lock (nano::wallet_id const &);
	void set_lock_observer (nano::wallet_id const &, std::function<void (bool, bool)> observer);

	// Account management
	nano::result<nano::public_key> insert_adhoc (nano::wallet_id const &, nano::raw_key const & prv);
	nano::result<nano::public_key> deterministic_insert (nano::wallet_id const &, uint32_t index);
	nano::result<nano::public_key> deterministic_insert (nano::wallet_id const &);
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

	// Key retrieval
	nano::result<nano::raw_key> fetch_prv (nano::wallet_id const &, nano::account const &) const;

	// Store phase of the block actions: resolves state, fetches keys and signs under the mutex
	prepared_block prepare_receive (nano::wallet_id const &, nano::block_hash const & send_hash, nano::account const & representative, nano::uint128_union const & amount, nano::account const & account, uint64_t work);
	prepared_block prepare_change (nano::wallet_id const &, nano::account const & source, nano::account const & representative, uint64_t work);
	prepared_send prepare_send (nano::wallet_id const &, nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work, std::optional<std::string> send_id);
	// Stores generated work for the account if its frontier still matches root
	void update_work (nano::wallet_id const &, nano::account const &, nano::root const &, uint64_t work);

	// Work cache
	nano::result<uint64_t> get_work (nano::wallet_id const &, nano::public_key const &) const;
	void set_work (nano::wallet_id const &, nano::public_key const &, uint64_t work);

	// Spendable accounts and representative of the wallet, errors when the wallet is missing or locked
	nano::result<wallet_scan_info> scan_info (nano::wallet_id const &) const;
	// Wallets containing the account, together with their configured representative
	std::vector<std::pair<nano::wallet_id, nano::account>> holders (nano::account const &) const;

	// Import/export
	bool import (nano::wallet_id const &, std::string const & json, std::string const & password);
	void serialize_json (nano::wallet_id const &, std::string & json) const;
	void write_backup (nano::wallet_id const &, std::filesystem::path const & path) const;

	// The wallet's internally synchronized password fan, for tests and diagnostics; the wallet must exist
	nano::fan & password_fan (nano::wallet_id const &);

	nano::container_info container_info () const;

	// Consecutive unused accounts scanned past the last used one before a seed scan gives up
	static uint32_t constexpr deterministic_check_gap{ 64 };

public: // Dependencies
	wallets & facade;
	nano::wallet::wallets_backend & backend;
	nano::ledger & ledger;
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::logger & logger;

	nano::kdf kdf;

private: // Transactions
	nano::store::write_transaction tx_begin_write ();
	nano::store::read_transaction tx_begin_read () const;

private: // Per-wallet operations, each requires the mutex to be held
	wallet_data * find_wallet (nano::wallet_id const &) const;
	// Attempts to unlock with the password, returns true if the password was wrong
	bool enter_password_impl (wallet_data &, nano::store::transaction const &, std::string const & password);
	// Inserts the account at the stored deterministic index and advances it
	nano::public_key deterministic_insert_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &);
	// Inserts the account at an explicit index, leaving the stored index untouched
	nano::public_key deterministic_insert_impl (wallet_data &, nano::store::write_transaction const &, nano::wallet::wallet_cipher const &, uint32_t index);
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

	mutable nano::mutex mutex;
};
}
