#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <optional>
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
	struct launch_result
	{
		blocks_query query;
		launch_grant grant;
	};

	void run ();

	std::optional<launch_result> next_database (bool should_throttle);
	std::optional<launch_result> wait_database (bool should_throttle);

	bootstrap_context & ctx;
	std::thread thread;
};
}
