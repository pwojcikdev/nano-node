#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <thread>

namespace nano::bootstrap
{
class database_strategy
{
public:
	explicit database_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one (bool should_throttle);

private:
	void run ();

	nano::account next_database (bool should_throttle);
	nano::account wait_database (bool should_throttle);

	bootstrap_context & ctx;
	std::thread thread;
};
}
