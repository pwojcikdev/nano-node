#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/peer_pool.hpp>
#include <nano/node/bootstrap/round.hpp>
#include <nano/secure/common.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
class topo_round final : public round
{
public:
	topo_round (nano::topo_scan_config const &, nano::topo_key position, unsigned quorum, std::function<void ()> sample_reserved = {});

	void feed (std::deque<nano::topo_key> const & entries);

	bool done () const;
	bool empty_page () const;
	bool settled () const;
	std::optional<nano::topo_key> conclude () const;
	std::deque<nano::topo_key> candidates () const;

	nano::topo_key const & position () const;
	size_t completed () const;
	size_t candidate_count () const;
	unsigned quorum () const;

private:
	void sample_reserved () override;

	nano::topo_scan_config const & config;
	nano::topo_key const position_m;
	unsigned const quorum_m;
	std::function<void ()> sample_reserved_m;
	std::set<nano::topo_key> candidates_m;
	size_t completed_m{ 0 };
};

class topo_scan_engine final
{
public:
	struct probes
	{
		std::function<peer_probe_status (std::span<nano::account const> exclude)> peer_status;
		std::function<size_t (std::span<id_t const> tag_ids)> count_inflight;
	};

	topo_scan_engine (nano::topo_scan_config const &, nano::stats &);

	void orient (nano::topo_key const &);
	nano::topo_key cursor () const;

	void settle (std::chrono::steady_clock::time_point now, probes const &);
	std::shared_ptr<topo_round> next_round (std::chrono::steady_clock::time_point now, probes const &);
	bool process_page (id_t tag_id, nano::topo_key const & start, std::deque<nano::topo_key> const & entries);
	void erase_page (id_t tag_id, nano::topo_key const & start);

	bool fetch_ready (std::chrono::steady_clock::time_point now) const;
	std::deque<nano::block_hash> next_fetch_candidates (std::chrono::steady_clock::time_point now, size_t max_count) const;
	void commit_fetch (std::deque<nano::block_hash> const & requested, std::deque<nano::block_hash> const & already_local, id_t tag_id, std::chrono::steady_clock::time_point now);
	bool process_blocks (id_t tag_id, std::deque<std::shared_ptr<nano::block>> const & blocks, std::chrono::steady_clock::time_point now);
	void erase_fetch (id_t tag_id, std::chrono::steady_clock::time_point now);

	bool submit_ready (std::chrono::steady_clock::time_point now) const;
	std::deque<std::shared_ptr<nano::block>> next_submit_batch (std::chrono::steady_clock::time_point now, size_t max_count);
	bool inspect (nano::block_hash const &, nano::block_status, std::chrono::steady_clock::time_point now);

	bool caught_up () const;
	size_t queued () const;
	size_t fetch_inflight () const;
	size_t frontier_gap_count () const;

	void reset ();

	nano::container_info container_info () const;

	static size_t constexpr max_round_samples_factor = 4;

private:
	enum class member_state
	{
		discovered,
		fetching,
		fetched,
		submitted,
		done,
	};

	struct member
	{
		nano::topo_key key;
		nano::block_hash hash;
		member_state state{ member_state::discovered };
		std::shared_ptr<nano::block> block;
		bool frontier{ false };
		bool frontier_gap{ false };
		bool fetch_queued{ false };
		bool submit_queued{ false };
		std::chrono::steady_clock::time_point next_action{};
	};

	struct head_state
	{
		size_t const index;
		nano::topo_key cursor{};
		std::shared_ptr<topo_round> round;
		std::chrono::steady_clock::time_point pause_until{};
		uint64_t floor_height{ 0 };
		uint64_t ceiling_height{ 0 };
		size_t empty_rounds{ 0 };
		size_t rounds{ 0 };
		size_t pages{ 0 };
		size_t processed{ 0 };

		explicit head_state (size_t index);
		bool spear () const;
		void reset (nano::topo_key const & start);
	};

	struct fetch_state
	{
		std::deque<nano::block_hash> hashes;
	};

	struct discover_result
	{
		std::optional<nano::topo_key> last;
		bool blocked{ false };
	};

	using action_key = std::pair<std::chrono::steady_clock::time_point, nano::topo_key>;

	unsigned quorum (head_state const &) const;
	void conclude (head_state &, std::chrono::steady_clock::time_point now);
	discover_result discover (std::deque<nano::topo_key> const &);
	void refresh_repair_band (head_state &);
	void wrap_repair (head_state &, std::chrono::steady_clock::time_point now);
	bool repair_quiesced () const;
	bool discovery_backpressured () const;

	void enqueue_fetch (member &, std::chrono::steady_clock::time_point);
	void erase_fetch_queue (member &);
	void enqueue_submit (member &, std::chrono::steady_clock::time_point);
	void erase_submit_queue (member &);
	void erase_member (std::unordered_map<nano::block_hash, member>::iterator);
	void mark_resolved (member &);
	void advance_frontier ();
	bool terminal (nano::block_status) const;
	bool dependency_gap (nano::block_status) const;

	std::unordered_map<nano::block_hash, member> members;
	std::map<nano::topo_key, nano::block_hash> members_by_key;
	std::set<action_key> fetch_queue;
	std::set<action_key> submit_queue;
	std::unordered_map<id_t, fetch_state> fetches;

	nano::topo_scan_config const & config;
	nano::stats & stats;
	std::vector<head_state> heads;
	size_t robin{ 0 };
	nano::topo_key submit_frontier{};
	bool spear_at_tip{ false };
	size_t frontier_gaps{ 0 };
};
}
