#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/fwd.hpp>

#include <unordered_set>

namespace nano::bootstrap
{
/*
 * Issues safe pull requests for accounts crawled directly from the ledger. Paced by the database
 * rate limiter, with the throttle increasing the request cost while results are unproductive.
 */
class database_strategy
{
public:
	explicit database_strategy (service &);

	// Root coroutine, spawned by the service supervisor; invoked again after a service reset
	asio::awaitable<void> run ();

private:
	asio::awaitable<void> run_requests ();
	asio::awaitable<void> run_pull (blocks_query, std::shared_ptr<nano::transport::channel>, nano::async::semaphore::token budget);

	service & ctx;
	nano::async::scope pulls;

	// Accounts with a request in flight (the old count_tags == 0 filter)
	std::unordered_set<nano::account> inflight;
};
}
