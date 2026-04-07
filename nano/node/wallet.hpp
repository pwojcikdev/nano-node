#pragma once

#include <nano/lib/lmdbconfig.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/lib/work.hpp>
#include <nano/node/fwd.hpp>
#include <nano/node/openclwork.hpp>
#include <nano/node/wallet/wallet_store.hpp>
#include <nano/secure/common.hpp>
#include <nano/store/lmdb/lmdb_env.hpp>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>

namespace nano
{
class wallets;

/**
 * A wallet is a set of account keys encrypted by a common encryption key
 */
class wallet final : public std::enable_shared_from_this<nano::wallet>
{
public:
	static std::shared_ptr<nano::wallet> open_existing (nano::store::write_transaction &, nano::wallets &, std::string const & wallet_path);
	static std::shared_ptr<nano::wallet> create (nano::store::write_transaction &, nano::wallets &, std::string const & wallet_path, nano::account representative);
	static std::shared_ptr<nano::wallet> create_from_json (nano::store::write_transaction &, nano::wallets &, std::string const & wallet_path, std::string const & json);

	// Password and lock management
	void enter_initial_password ();
	bool enter_password (std::string const & password);
	bool rekey (std::string const & password);
	bool is_locked () const;
	void lock ();

	// Account management
	nano::result<nano::public_key> insert_adhoc (nano::raw_key const & prv, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (uint32_t index, bool generate_work = true);
	nano::result<nano::public_key> deterministic_insert (bool generate_work = true);
	bool insert_watch (nano::public_key const & pub);
	void remove_account (nano::account const & account);
	std::vector<nano::account> accounts () const;
	bool exists (nano::public_key const & pub);
	bool move_accounts (wallet & source, std::vector<nano::public_key> const & accounts);
	nano::key_type key_type (nano::account const & account) const;

	// Seed management
	nano::result<nano::raw_key> get_seed () const;
	nano::public_key change_seed (nano::raw_key const & seed, uint32_t count = 0);
	void deterministic_restore ();
	uint32_t deterministic_check (uint32_t index);
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
	std::shared_ptr<nano::block> send_action (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, uint64_t work = 0, bool generate_work = true, boost::optional<std::string> id = {});
	bool action_complete (std::shared_ptr<nano::block> const & block, nano::account const & account, bool generate_work, nano::block_details const & details);

	// Sync/async block operations
	bool change_sync (nano::account const & source, nano::account const & representative);
	void change_async (nano::account const & source, nano::account const & representative, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	bool receive_sync (std::shared_ptr<nano::block> const & block, nano::account const & representative, nano::uint128_t const & amount);
	void receive_async (nano::block_hash const & hash, nano::account const & representative, nano::uint128_t const & amount, nano::account const & account, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true);
	nano::block_hash send_sync (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount);
	void send_async (nano::account const & source, nano::account const & destination, nano::uint128_t const & amount, std::function<void (std::shared_ptr<nano::block> const &)> const & action, uint64_t work = 0, bool generate_work = true, boost::optional<std::string> id = {});

	// Work cache
	void work_cache_blocking (nano::account const & account, nano::root const & root);
	void work_ensure (nano::account const & account, nano::root const & root);
	nano::result<uint64_t> get_work (nano::public_key const &) const;
	void set_work (nano::public_key const & pub, uint64_t work);

	// Receivable
	bool search_receivable ();

	// Import/export
	bool import (std::string const & json, std::string const & password);
	std::string serialize_json ();
	void write_backup (std::filesystem::path const & path);

	// Status
	bool is_open ();

public:
	std::unordered_set<nano::account> free_accounts;
	std::function<void (bool, bool)> lock_observer;
	nano::wallet_store store;
	nano::wallets & wallets;
	nano::logger & logger;

private:
	explicit wallet (nano::wallets &);

	// Internal implementation methods (accept transactions for batching scenarios)
	bool enter_password_impl (nano::store::transaction const &, std::string const & password);
	bool insert_watch_impl (nano::store::write_transaction const &, nano::public_key const & pub);
	nano::public_key deterministic_insert_impl (nano::store::write_transaction const &, bool generate_work = true);
	nano::public_key deterministic_insert_impl (nano::store::write_transaction const &, uint32_t index, bool generate_work = true);
	void work_update_impl (nano::store::write_transaction const &, nano::account const & account, nano::root const & root, uint64_t work);
	bool search_receivable_impl (nano::store::transaction const &);
	void init_free_accounts_impl (nano::store::transaction const &);
	uint32_t deterministic_check_impl (nano::store::transaction const &, uint32_t index);
	nano::public_key change_seed_impl (nano::store::write_transaction const &, nano::raw_key const & seed, uint32_t count = 0);
	void deterministic_restore_impl (nano::store::write_transaction const &);

private:
	nano::locked<std::unordered_set<nano::account>> representatives;

	friend class wallets;
};

class wallet_representatives
{
public:
	uint64_t voting{ 0 }; // Number of representatives with at least the configured minimum voting weight
	bool half_principal{ false }; // has representatives with at least 50% of principal representative requirements
	std::unordered_set<nano::account> accounts; // Representatives with at least the configured minimum voting weight
	bool have_half_rep () const
	{
		return half_principal;
	}
	bool exists (nano::account const & rep_a) const
	{
		return accounts.count (rep_a) > 0;
	}
	void clear ()
	{
		voting = 0;
		half_principal = false;
		accounts.clear ();
	}
};

class wallets_store
{
public:
	virtual ~wallets_store () = default;
};

class mdb_wallets_store final : public wallets_store
{
public:
	mdb_wallets_store (std::filesystem::path const &, nano::lmdb_config const & lmdb_config_a = nano::lmdb_config{});
	nano::store::lmdb::env environment;
	bool error{ false };
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
	nano::wallets_store &,
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
	std::shared_ptr<nano::wallet> open (nano::wallet_id const &);
	std::shared_ptr<nano::wallet> create (nano::wallet_id const &);
	std::shared_ptr<nano::wallet> create_from_json (nano::wallet_id const &, std::string const & json);
	void destroy (nano::wallet_id const &);
	void reload ();
	void clear_send_ids ();

	// Account lookup
	std::unordered_map<nano::wallet_id, std::shared_ptr<nano::wallet>> all_wallets ();
	bool exists (nano::account const &);
	bool exists_any (nano::account const &, nano::account const &);

	// Receivable
	bool search_receivable (nano::wallet_id const &);
	void search_receivable_all ();
	void receive_confirmed (nano::block_hash const & hash, nano::account const & destination);

	// Wallet actions queue
	void do_wallet_actions ();
	void queue_wallet_action (nano::uint128_t const & priority, std::shared_ptr<nano::wallet> const &, std::function<void (nano::wallet &)> action);

	// Representatives
	void foreach_representative (std::function<void (nano::public_key const &, nano::raw_key const &)> const & action);
	bool check_rep (nano::account const &);
	void refresh_reps ();
	nano::wallet_representatives reps () const;

	/// Returns a signer that iterates over all representatives in the wallet
	using signer_t = std::function<void (std::function<void (nano::public_key const &, nano::raw_key const &)> const &)>;
	signer_t signer ();

	nano::container_info container_info () const;

private: // Transactions
	nano::store::write_transaction tx_begin_write ();
	nano::store::read_transaction tx_begin_read ();

public: // Dependencies
	nano::node & node;
	nano::wallets_store & wallets_store;
	nano::ledger & ledger;
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::online_reps & online_reps;
	nano::network & network;
	nano::stats & stats;
	nano::logger & logger;

public:
	std::function<void (bool)> observer;

	std::unordered_map<nano::wallet_id, std::shared_ptr<nano::wallet>> items;
	std::multimap<nano::uint128_t, std::pair<std::shared_ptr<nano::wallet>, std::function<void (nano::wallet &)>>, std::greater<nano::uint128_t>> actions;
	nano::locked<std::unordered_map<nano::account, nano::root>> delayed_work;

	nano::kdf kdf;

	MDB_dbi handle{};
	MDB_dbi send_action_ids{};
	nano::store::lmdb::env & env;

	mutable nano::mutex mutex;
	mutable nano::mutex action_mutex;
	nano::condition_variable condition;
	nano::condition_variable reps_condition;
	nano::condition_variable receivable_condition;
	std::atomic<bool> stopped{ false };
	std::thread thread;
	std::thread reps_thread;
	std::thread receivable_thread;

	nano::thread_pool workers;

	static nano::uint128_t const generate_priority;
	static nano::uint128_t const high_priority;

private:
	void run_reps_scan ();
	void run_receivable_scan ();
	bool check_rep_impl (wallet_representatives &, nano::account const &, nano::uint128_t const & half_principal_weight);
	bool exists_impl (nano::store::transaction const &, nano::account const &);
	void refresh_rep_index ();
	void refresh_rep_keys_cache ();

private:
	mutable nano::locked<nano::wallet_representatives> representatives;
	nano::locked<std::vector<std::pair<nano::public_key, std::unique_ptr<nano::fan>>>> rep_keys_cache;

	friend class wallet;
};
}
