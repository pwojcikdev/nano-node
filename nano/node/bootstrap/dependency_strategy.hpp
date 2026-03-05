#pragma once

#include <nano/node/bootstrap/bootstrap_service.hpp>

#include <thread>

namespace nano::bootstrap
{
class dependency_strategy
{
public:
	explicit dependency_strategy (nano::bootstrap_service & service);

	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::account_info_payload const & response, nano::bootstrap_service::async_tag const & tag);

private:
	void run ();

	nano::block_hash next_blocking ();
	nano::block_hash wait_blocking ();
	bool request_info (nano::block_hash hash, std::shared_ptr<nano::transport::channel> const & channel);

	nano::bootstrap_service & service;
	std::thread thread;
};
}
