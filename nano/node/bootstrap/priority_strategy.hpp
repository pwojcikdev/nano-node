#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <thread>

namespace nano::bootstrap
{
class priority_strategy
{
public:
	explicit priority_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one ();

private:
	void run ();

	using priority_result = account_sets_index::priority_result;

	priority_result next_priority ();
	priority_result wait_priority ();

	bootstrap_context & ctx;
	std::thread thread;
};
}
