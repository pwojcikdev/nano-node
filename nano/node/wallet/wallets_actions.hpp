#pragma once

#include <nano/lib/locks.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/node/fwd.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>

namespace nano::wallet
{
/**
 * Runs wallet block actions: a prioritized action queue with its worker thread,
 * the send/receive/change operations and the work pre-caching around them.
 * Store access happens exclusively through the wallets prepare/update API.
 */
class wallets_actions final
{
public:
	wallets_actions (wallets &, nano::node &, nano::node_config const &, nano::network_params const &, nano::network &, nano::logger &);
	~wallets_actions ();

	void start ();
	void stop ();

	// Queues an action against the wallet id; stale ids simply no-op when the action runs
	void queue (nano::uint128_t const & priority, nano::wallet_id const &, std::function<void (wallet &)> action);
	void queue (nano::uint128_t const & priority, std::shared_ptr<wallet> const &, std::function<void (wallet &)> action);

	// Block actions, must not be called while holding the wallets mutex
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

	// Notified with (true) before and (false) after each action runs
	void set_observer (std::function<void (bool)> observer);

	nano::container_info container_info () const;

	static nano::uint128_t const generate_priority;
	static nano::uint128_t const high_priority;

private: // Dependencies
	wallets & wallets;
	nano::node & node;
	nano::node_config const & config;
	nano::network_params const & network_params;
	nano::network & network;
	nano::logger & logger;

private:
	void run ();
	// Regenerates and processes the block (work + ledger), must be called without the wallets mutex held
	bool action_complete (nano::wallet_id const &, std::shared_ptr<nano::block> const & block, nano::account const & account, bool generate_work, nano::block_details const & details);

public:
	// Roots scheduled for delayed work pre-caching, exposed for tests
	nano::locked<std::unordered_map<nano::account, nano::root>> delayed_work;

private:
	std::function<void (bool)> observer{ [] (bool) {} };

	std::multimap<nano::uint128_t, std::pair<std::shared_ptr<wallet>, std::function<void (wallet &)>>, std::greater<nano::uint128_t>> actions;

	std::atomic<bool> stopped{ false };
	mutable nano::mutex mutex;
	nano::condition_variable condition;
	std::thread thread;

	nano::thread_pool workers;
};
}
