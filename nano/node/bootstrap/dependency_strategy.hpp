#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/fwd.hpp>

#include <unordered_set>

namespace nano::bootstrap
{
/*
 * Walks the blocked set: for each missing dependency it asks a peer which account contains the
 * block, then inserts that account into the priority set. A periodic companion coroutine reinserts
 * already known dependencies.
 */
class dependency_strategy
{
public:
	explicit dependency_strategy (service &);

	// Root coroutine, spawned by the service supervisor; invoked again after a service reset
	asio::awaitable<void> run ();

private:
	asio::awaitable<void> run_requests ();
	asio::awaitable<void> run_sync ();
	asio::awaitable<void> run_walk (nano::block_hash, std::shared_ptr<nano::transport::channel>, nano::async::semaphore::token budget);

	service & ctx;
	nano::async::scope children;

	// Dependencies with a request in flight (the old count_tags == 0 filter)
	std::unordered_set<nano::block_hash> inflight;
};
}
