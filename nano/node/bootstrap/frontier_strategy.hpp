#pragma once

#include <nano/lib/thread_pool.hpp>
#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <deque>
#include <memory>
#include <thread>
#include <utility>

namespace nano::bootstrap
{
class frontier_strategy
{
public:
	explicit frontier_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag);

private:
	void run ();

	nano::account wait_frontier ();
	bool request_frontiers (nano::account start, std::shared_ptr<nano::transport::channel> const & channel);
	verify_result verify (nano::messages::asc_pull_ack::frontiers_payload const & response, async_tag const & tag) const;
	void process_frontiers (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	bootstrap_context & ctx;
	std::thread thread;

	// Dedicated pool for the (ledger-reading) frontier processing tasks. Kept separate from
	// ctx.workers, which carries block-processor submissions, so the two workloads don't contend
	// for a single thread and so the frontier scan's `queued_tasks ()` backpressure only counts
	// its own pending work.
	nano::thread_pool workers;
};
}
