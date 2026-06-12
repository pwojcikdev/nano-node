#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <deque>
#include <memory>
#include <thread>

namespace nano::bootstrap
{
class topo_strategy
{
public:
	explicit topo_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();
	void run_one ();

	bool process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag);
	void timeout (async_tag const & tag);
	void failure (async_tag const & tag);
	void inspect (nano::block_status const & result, nano::block_context const & context);

private:
	void run ();
	void orient ();

	bool try_submit ();
	bool try_fetch ();
	bool try_page_or_wait ();

	bootstrap_context & ctx;
	topo_scan_engine::probes probes;
	std::thread thread;
};
}
