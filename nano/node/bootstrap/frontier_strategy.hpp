#pragma once

#include <nano/lib/thread_pool.hpp>
#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <deque>
#include <memory>
#include <optional>
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
	struct launch_result
	{
		std::shared_ptr<nano::transport::channel> channel;
		std::shared_ptr<frontier_round> round;
		nano::account position{ 0 };
		id_t id{ 0 };
	};

	void run ();
	std::optional<launch_result> next_frontier_launch ();

	void process_frontiers (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	bootstrap_context & ctx;
	std::thread thread;

	// Dedicated pool for the frontier processing tasks (ledger reads)
	nano::thread_pool workers;
};

struct frontier_classification
{
	std::deque<nano::account> prioritize;
	size_t outdated{ 0 };
	size_t pending{ 0 };
};

frontier_classification classify_frontiers (nano::secure::transaction const &, nano::ledger &, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);
}
