#pragma once

#include <nano/node/bootstrap/bootstrap_service.hpp>

#include <deque>
#include <memory>
#include <thread>
#include <utility>

namespace nano::bootstrap
{
class frontier_strategy
{
public:
	explicit frontier_strategy (nano::bootstrap_service & service);

	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::frontiers_payload const & response, nano::bootstrap_service::async_tag const & tag);

private:
	void run ();

	nano::account wait_frontier ();
	bool request_frontiers (nano::account start, std::shared_ptr<nano::transport::channel> const & channel);
	nano::bootstrap_service::verify_result verify (nano::messages::asc_pull_ack::frontiers_payload const & response, nano::bootstrap_service::async_tag const & tag) const;
	void process_frontiers (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	nano::bootstrap_service & service;
	std::thread thread;
};
}
