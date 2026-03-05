#pragma once

#include <nano/node/bootstrap/bootstrap_service.hpp>

#include <thread>

namespace nano::bootstrap
{
class priority_strategy
{
public:
	explicit priority_strategy (nano::bootstrap_service & service);

	void start ();
	void stop ();
	void run_one ();

private:
	void run ();

	using priority_result = account_sets_index::priority_result;

	priority_result next_priority ();
	priority_result wait_priority ();

	nano::bootstrap_service & service;
	std::thread thread;
};
}
