#pragma once

#include <nano/lib/async.hpp>
#include <nano/lib/numbers_templ.hpp>
#include <nano/node/bootstrap/account_sets_index.hpp>
#include <nano/node/bootstrap/queries.hpp>
#include <nano/node/fwd.hpp>

#include <deque>
#include <unordered_map>

namespace nano::bootstrap
{
/*
 * Pulls blocks for accounts from the priority set. Pull queries are prefetched in batches with a
 * single ledger read transaction (off the strand); each request runs as a pipelined child coroutine
 * that awaits its response and applies the outcome.
 */
class priority_strategy
{
public:
	explicit priority_strategy (service &);

	// Root coroutine, spawned by the service supervisor; invoked again after a service reset
	asio::awaitable<void> run ();

private:
	using priority_result = account_sets_index::priority_result;

	struct buffered_request
	{
		blocks_query query;
		unsigned fails;
	};

	asio::awaitable<void> run_requests ();

	// Fills the buffer with pull queries for the next priority batch, reading the ledger once for the whole batch
	asio::awaitable<void> refill ();

	// One pipelined pull request
	asio::awaitable<void> run_pull (buffered_request, std::shared_ptr<nano::transport::channel>, nano::async::semaphore::token budget);

	// Builds a pull query for a single priority account; runs on the worker pool
	blocks_query prepare_query (nano::store::transaction const &, priority_result const &, bool optimistic) const;

	service & ctx;
	nano::async::scope pulls;

	// Prefetched pull queries, drained one per request
	std::deque<buffered_request> buffer;

	// Per-account in-flight requests, limiting the concurrency per account (the old count_tags filter)
	std::unordered_map<nano::account, unsigned> inflight;

	static std::size_t constexpr batch_size = 32;
	static unsigned constexpr max_inflight_per_account = 4;
};
}
