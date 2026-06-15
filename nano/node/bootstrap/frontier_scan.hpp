#pragma once

#include <nano/lib/fwd.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/bootstrap_config.hpp>
#include <nano/node/bootstrap/common.hpp>
#include <nano/node/bootstrap/peer_pool.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <utility>
#include <vector>

namespace nano::bootstrap
{
class frontier_round final
{
public:
	frontier_round (nano::frontier_scan_config const &, nano::account position, nano::account range_end);

	void feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	bool done () const;
	bool empty_range () const;
	bool settled () const;
	std::optional<nano::account> conclude () const;

	size_t completed () const;
	size_t candidate_count () const;

private:
	nano::frontier_scan_config const & config;
	nano::account const position;
	nano::account const range_end;
	std::set<nano::account> candidates;
	size_t completed_m{ 0 };
};

class frontier_scan_engine final
{
public:
	struct probes
	{
		std::function<peer_probe_status (std::span<nano::account const> exclude)> peer_status;
		std::function<size_t (std::span<id_t const> tag_ids)> count_inflight;
	};

	struct launch_slot
	{
		size_t head_index;
		nano::account position;
		std::span<nano::account const> exclude;
	};

	frontier_scan_engine (nano::frontier_scan_config const &, nano::stats &);

	void settle (std::chrono::steady_clock::time_point now, probes const &);
	std::optional<launch_slot> peek_launch (std::chrono::steady_clock::time_point now, probes const &);
	void commit (size_t head_index, nano::account const & position, nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now);
	void erase_sample (size_t head_index, id_t tag_id);
	void erase_sample (id_t tag_id, nano::account const & start);
	bool process (id_t tag_id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers);

	void reset ();

	nano::container_info container_info () const;

	static size_t constexpr max_round_samples_factor = 4;

private:
	struct round_state
	{
		nano::account const position;
		frontier_round votes;
		std::vector<nano::account> used;
		std::vector<id_t> tag_ids;
		size_t launched{ 0 };
		std::chrono::steady_clock::time_point last_launch{};

		round_state (nano::frontier_scan_config const &, nano::account position, nano::account range_end);
		bool owns (id_t tag_id) const;
		bool erase (id_t tag_id);
	};

	struct head_state
	{
		size_t const index;
		nano::account const start;
		nano::account const end;
		nano::account cursor;
		std::unique_ptr<round_state> round;
		std::chrono::steady_clock::time_point pause_until{};
		size_t rounds{ 0 };
		size_t processed{ 0 };

		head_state (size_t index, nano::account start, nano::account end);
		void reset ();
	};

	void conclude (head_state &, std::chrono::steady_clock::time_point now);
	head_state & find_head (nano::account const & position);
	head_state const & find_head (nano::account const & position) const;

	nano::frontier_scan_config const & config;
	nano::stats & stats;
	std::vector<head_state> heads;
	size_t robin{ 0 };
};
}
