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
	explicit topo_strategy (bootstrap_context &);

	void start ();
	void stop ();

	void inspect (nano::secure::transaction const &, nano::block_status const &, nano::block_context const &);

	bool process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag);
	bool process_blocks (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);

	verify_result verify (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag) const;

private:
	void run_index ();
	void run_one_index ();
	void run_blocks ();
	void run_one_blocks ();
	void run_processing ();
	void run_one_processing ();

	// Blocks until the topology scan offers a cursor to query, or indexing finishes.
	std::optional<nano::topo_key> wait_position ();
	// Blocks until the topology scan offers a non-empty batch of hashes, or all work is done.
	std::deque<nano::block_hash> wait_block_batch ();
	// Blocks until completed blocks are ready to drain (head of queue completed), or all work is done.
	std::deque<std::shared_ptr<nano::block>> wait_ordered_blocks ();
	// Waits for a channel whose peer advertises the topo_index capability.
	// `topo_index` requests can only be served by peers that maintain the topo index.
	std::shared_ptr<nano::transport::channel> wait_topo_index_channel ();

	bool request_index (nano::topo_key cursor, std::shared_ptr<nano::transport::channel> const & channel);
	bool request_blocks (std::deque<nano::block_hash> hashes, std::shared_ptr<nano::transport::channel> const & channel);

private:
	bootstrap_context & ctx;

	std::thread thread_index;
	std::thread thread_blocks;
	std::thread thread_processing;
};
}
