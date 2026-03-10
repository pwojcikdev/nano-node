#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <thread>

namespace nano::bootstrap
{
class dependency_strategy
{
public:
	explicit dependency_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::account_info_payload const & response, async_tag const & tag);

private:
	void run ();
	void run_sync ();

	nano::block_hash next_blocking ();
	nano::block_hash wait_blocking ();
	bool request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel);

	bootstrap_context & ctx;
	std::thread thread;
	std::thread sync_thread;
};
}
