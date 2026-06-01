#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace nano::bootstrap
{
class topo_strategy
{
public:
	explicit topo_strategy (bootstrap_context &);

	void start ();
	void stop ();

	void inspect (nano::secure::transaction const &, nano::block_status const &, nano::block_context const &);

	bool process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag);
	bool process_blocks (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);

	verify_result verify (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag) const;

private:
	void run_scan ();
	void run_fetch ();
	void run_one_fetch ();
	void run_processing ();
	void run_one_processing ();

	std::optional<std::pair<std::size_t, nano::topo_key>> pick_scan_head (std::chrono::steady_clock::time_point now);

	std::optional<std::pair<std::size_t, nano::topo_key>> wait_scan_head ();

	std::deque<nano::block_hash> wait_fetch_batch ();

	std::deque<std::shared_ptr<nano::block>> wait_block_batch ();

	// Waits for a channel whose peer advertises the topo_index capability
	std::shared_ptr<nano::transport::channel> wait_topo_channel ();

	bool request_index (std::size_t head, nano::topo_key cursor, std::shared_ptr<nano::transport::channel> const & channel);
	bool request_blocks (std::deque<nano::block_hash> hashes, std::shared_ptr<nano::transport::channel> const & channel);

private:
	bootstrap_context & ctx;

	std::thread thread_scan;
	std::thread thread_blocks;
	std::thread thread_processing;

	std::size_t scan_tick{ 0 }; // round-robin counter for weighted head selection
};
}
