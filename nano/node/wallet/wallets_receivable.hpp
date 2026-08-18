#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/fwd.hpp>

#include <atomic>
#include <thread>

namespace nano::wallet
{
/**
 * Periodically scans the ledger for receivable blocks destined for wallet accounts,
 * receiving confirmed ones and requesting confirmation for the rest.
 */
class wallets_receivable final
{
public:
	wallets_receivable (wallets &, nano::node &, nano::ledger &, nano::node_config const &, nano::network_params const &, nano::stats &, nano::logger &);
	~wallets_receivable ();

	void start ();
	void stop ();

	// Receives confirmed receivables above the receive minimum and starts elections for unconfirmed ones, returns true if the wallet is locked or missing
	bool search (nano::wallet_id const &);
	void search_all ();

	// Attempts to receive the confirmed block into every wallet holding the destination account
	void receive_confirmed (nano::block_hash const & hash, nano::account const & destination);

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

	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::thread thread;
};
}
