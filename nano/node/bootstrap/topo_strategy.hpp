#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

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
	// One scan loop per head: head 0 is the spear, the rest are repair heads.
	void run_scan (std::size_t head);
	void run_one_scan (std::size_t head);
	void run_blocks ();
	void run_one_blocks ();
	void run_processing ();
	void run_one_processing ();

	// Blocks until head `h` offers a cursor to query (spear: frontier; repair:
	// rolled-back gap region), or there's nothing for it to do.
	std::optional<nano::topo_key> wait_position (std::size_t head);
	// Blocks until there are member hashes to fetch, or all work is done.
	std::deque<nano::block_hash> wait_block_batch ();
	// Blocks until there are blocks ready to submit, or all work is done.
	std::deque<std::shared_ptr<nano::block>> wait_submit_batch ();
	// Waits for a channel whose peer advertises the topo_index capability.
	std::shared_ptr<nano::transport::channel> wait_topo_index_channel ();

	bool request_index (std::size_t head, nano::topo_key cursor, std::shared_ptr<nano::transport::channel> const & channel);
	bool request_blocks (std::deque<nano::block_hash> hashes, std::shared_ptr<nano::transport::channel> const & channel);

private:
	bootstrap_context & ctx;

	std::vector<std::thread> scan_threads; // one per head
	std::thread thread_blocks;
	std::thread thread_processing;
};
}
