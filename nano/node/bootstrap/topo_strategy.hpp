#pragma once

#include <nano/node/bootstrap/bootstrap_context.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
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
	// Startup orientation: binary-search peers' topo index for the watermark and seed the
	// spear there before steady-state scanning begins (skips re-paging from genesis on a
	// restart). Runs once at the top of run_scan(); a no-op when disabled or when no capable
	// peer is reachable within the startup window (falls back to scanning from genesis).
	void orient ();
	// Gather the union of distinct capable peers' pages for a probe at {height,0}, so a behind
	// peer's short page can't falsely lower the watermark. The per-probe target is the desired
	// `orient_consideration_count` capped to the reachable capable pool, so a single-peer (early
	// network / two-node) case finalizes on one response instead of stalling for an absent
	// second peer. Bounded by `deadline`; nullopt if no capable peer could be reached.
	std::optional<std::deque<nano::topo_key>> orient_gather (uint64_t height, std::chrono::steady_clock::time_point deadline);
	// Send one orientation probe on `channel` and wait for its response, or nullopt on
	// timeout / stop. Sequential: at most one orientation request is outstanding at a time.
	std::optional<std::deque<nano::topo_key>> orient_request (uint64_t height, std::shared_ptr<nano::transport::channel> const & channel);
	// Acquire a capable channel whose node id is not in `seen`, bounded by `deadline`.
	std::shared_ptr<nano::transport::channel> wait_orient_channel (std::vector<nano::node_id> const & seen, std::chrono::steady_clock::time_point deadline);
	// True if `channel`'s peer advertises the topo_index capability (can serve an index walk).
	static bool orient_capable (std::shared_ptr<nano::transport::channel> const & channel);

	// A SINGLE thread serves all heads (head 0 is the spear, the rest are repair heads),
	// weighting the spear at ~half the requests and sharing the other half across the
	// repair heads round-robin.
	void run_scan ();
	void run_blocks ();
	void run_one_blocks ();
	void run_processing ();
	void run_one_processing ();

	// A chosen scan request: which head, the cursor to query, and the distinct peer to query.
	struct scan_request
	{
		std::size_t head{ 0 };
		nano::topo_key cursor{};
		std::shared_ptr<nano::transport::channel> channel;
	};

	// Pick the next head to query this tick, weighting the spear (head 0) at ~half and
	// round-robining the repair heads through the other half; falls back to any other
	// head with work so one idle head can't stall the thread. Returns the chosen head
	// and its next cursor, or nullopt if no head currently has work. Must be called under
	// the context mutex (it calls topology.next); commits only the returned head.
	std::optional<std::pair<std::size_t, nano::topo_key>> pick_scan_head (std::chrono::steady_clock::time_point now);
	// Waits (with backoff) until some head has work AND a distinct capable peer is available
	// to query it, returning both, or nullopt once the topology stops indexing / is caught
	// up. Head selection, the per-page distinct-peer quorum, and channel acquisition all
	// happen under one lock hold so acquire+commit is atomic.
	std::optional<scan_request> wait_scan_request ();
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

	std::thread thread_scan; // single thread serving all heads (spear weighted ~half)
	std::thread thread_blocks;
	std::thread thread_processing;
	std::size_t scan_tick{ 0 }; // round-robin counter for weighted head selection

	// Orientation rendezvous (guarded by ctx.mutex): the id of the outstanding orientation
	// probe and the response entries stashed by process() for the orient() driver to consume.
	nano::bootstrap::id_t orient_id{ 0 };
	std::optional<std::deque<nano::topo_key>> orient_response;
};
}
