#pragma once

#include <nano/lib/fan.hpp>
#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nano::wallet
{
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
	bool exists (nano::account const & rep) const
	{
		return accounts.count (rep) > 0;
	}
	void clear ()
	{
		voting = 0;
		half_principal = false;
		accounts.clear ();
	}
};

/**
 * Tracks which wallet accounts qualify as local representatives and caches their voting keys.
 * State is rebuilt from the wallets on refresh (): periodically by the scan thread, and synchronously after wallet mutations.
 */
class wallets_reps final
{
public:
	wallets_reps (wallets &, nano::node &, nano::ledger &, nano::node_config const &, nano::network_params const &, nano::stats &, nano::logger &);
	~wallets_reps ();

	void start ();
	void stop ();

	// Rebuilds the representative index and the voting key cache from the current wallet contents.
	// Must not be called while holding the wallets mutex, it reads the wallets through their public API
	void refresh ();

	// Aggregate info about local representatives
	wallet_representatives reps () const;
	// Local representatives detected among the wallet's accounts
	std::unordered_set<nano::account> reps (nano::wallet_id const &) const;

	// Iterates cached voting keys; the cache lock is held during callbacks, recursive calls are not allowed
	void foreach_representative (std::function<void (nano::public_key const & pub, nano::raw_key const & prv)> const &);

	/// Returns a signer that iterates over all representatives in the wallet
	using signer_t = std::function<void (std::function<void (nano::public_key const &, nano::raw_key const &)> const &)>;
	signer_t signer ();

	nano::container_info container_info () const;

private: // Dependencies
	wallets & wallets;
	nano::node & node;
	nano::ledger & ledger;
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::stats & stats;
	nano::logger & logger;

private:
	void run ();
	// Registers the account in reps if it qualifies by weight, returns true if newly added
	bool check_rep (wallet_representatives & reps, nano::account const &, nano::uint128_t const & half_principal_weight) const;

private:
	mutable nano::locked<wallet_representatives> representatives;
	nano::locked<std::unordered_map<nano::wallet_id, std::unordered_set<nano::account>>> representatives_by_wallet;
	// Private keys spread across fans to avoid plaintext keys in memory at rest
	nano::locked<std::vector<std::pair<nano::public_key, std::unique_ptr<nano::fan>>>> rep_keys_cache;

	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::thread thread;
};
}
