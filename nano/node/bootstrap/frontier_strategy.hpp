#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp>
#include <nano/node/fwd.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
/*
 * Scans the account space for outdated frontiers. The space is divided into equal ranges, each owned
 * by a long-lived head coroutine that advances a cursor through its range. One round samples the
 * frontiers at the cursor from several distinct peers (the fanout), votes on the gathered candidates
 * and advances at a single settlement point. Responses are reconciled against the ledger off the
 * strand and outdated accounts enter the priority set.
 */
class frontier_strategy
{
public:
	explicit frontier_strategy (service &);

	// Root coroutine, spawned by the service supervisor; invoked again after a service reset
	asio::awaitable<void> run ();

	nano::container_info container_info () const;

private:
	// Long-lived per-head state; everything that is local to one round lives in round_state instead
	struct head_state
	{
		head_index_t const index;
		nano::account const start;
		nano::account const end; // The range of accounts to scan is [start, end)
		nano::account cursor; // Current scan position

		size_t processed{ 0 }; // Total candidates accepted, for introspection
		size_t rounds{ 0 }; // Total rounds concluded, for introspection

		nano::async::condition_any wake; // Notified by samples whenever the round state changes
		nano::async::scope children; // Owns the sample coroutines, including post-settlement stragglers

		head_state (head_index_t index, nano::account start, nano::account end, nano::async::strand &);
	};

	// Shared state of one fanout round; samples feed it while `open`, stragglers from a settled
	// round still release their peer slot but feed nothing
	struct round_state
	{
		frontier_round round;
		std::vector<nano::account> used; // Node ids sampled this round, enforcing the distinct-peer rule
		size_t inflight{ 0 };
		bool open{ true };
	};

	asio::awaitable<void> run_head (head_state &);
	asio::awaitable<void> run_round (head_state &);
	asio::awaitable<void> run_sample (head_state &, std::shared_ptr<round_state>, nano::account start, std::shared_ptr<nano::transport::channel>, nano::async::semaphore::token budget);
	asio::awaitable<void> classify (std::deque<std::pair<nano::account, nano::block_hash>> frontiers);

	// Waits until a sample settles or the timeout elapses
	asio::awaitable<void> wait_progress (head_state &, std::chrono::milliseconds timeout);

	service & ctx;

	std::vector<std::unique_ptr<head_state>> heads;
	nano::async::scope head_scope;

	nano::async::condition_any classify_changed;
	size_t classify_inflight{ 0 };
};
}
