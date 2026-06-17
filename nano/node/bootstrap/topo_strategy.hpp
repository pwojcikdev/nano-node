#pragma once

#include <nano/lib/thread_pool.hpp>
#include <nano/messages/fwd.hpp>
#include <nano/node/bootstrap/bootstrap_context.hpp>
#include <nano/node/bootstrap/topo_blocks.hpp>
#include <nano/node/bootstrap/topo_scan.hpp>

#include <deque>
#include <memory>
#include <thread>

namespace nano::bootstrap
{
/*
 * Topology bootstrap strategy.
 *
 * Owns three driver threads and a background worker pool, and drives two engines:
 *  - topo_scan walks the peers' topo index across heads and retires pages to the worker pool for
 *    a ledger pre-check (already-present blocks are dropped, the rest become fetch candidates).
 *  - topo_blocks fetches the candidates via random-block requests and releases them to the block
 *    processor in topological order.
 *
 * Back-pressure lives here, not in the engines: the spearhead head pauses discovery while too many
 * blocks await fetch, but the repair heads keep scanning so gaps are continuously rediscovered.
 */
class topo_strategy
{
public:
	explicit topo_strategy (bootstrap_context & ctx);

	void start ();
	void stop ();

	// Re-anchor the spearhead and frontier to our local topology tip
	void orient ();

	void reset ();

	nano::container_info container_info () const;

	// Response/conclusion hooks, invoked by bootstrap_context under ctx.mutex
	bool process (nano::messages::asc_pull_ack::topo_index_payload const & response, async_tag const & tag);
	bool process (nano::messages::asc_pull_ack::blocks_payload const & response, async_tag const & tag);
	void timeout (async_tag const & tag);
	void failure (async_tag const & tag);

private:
	// Driver loops (one thread each) and their single-iteration bodies
	void run_scan ();
	void run_fetch ();
	void run_submit ();
	void scan_one ();
	void fetch_one ();
	void submit_one ();

	// Worker-pool task: drop already-present blocks, hand the rest to the fetch engine
	void precheck (unsigned head, std::deque<nano::topo_key> entries);

	bootstrap_context & ctx;

	topo_scan scan;
	topo_blocks blocks;

	std::thread scan_thread;
	std::thread fetch_thread;
	std::thread submit_thread;

	// Pre-check ledger reads run here, off the message thread
	nano::thread_pool workers;
};
}
