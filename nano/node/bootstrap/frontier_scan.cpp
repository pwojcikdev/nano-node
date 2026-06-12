#include <nano/lib/container_info.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/bootstrap/frontier_scan.hpp>

#include <boost/multiprecision/cpp_dec_float.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <iterator>
#include <limits>
#include <numeric>
#include <utility>

namespace nano::bootstrap
{
frontier_round::frontier_round (nano::frontier_scan_config const & config_a, nano::account position_a, nano::account range_end_a) :
	config{ config_a },
	position{ position_a },
	range_end{ range_end_a }
{
}

void frontier_round::feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	++completed_m;

	for (auto const & [account, _] : frontiers)
	{
		if (account.number () > position.number ())
		{
			candidates.insert (account);
		}
	}

	while (candidates.size () > config.candidates)
	{
		candidates.erase (std::prev (candidates.end ()));
	}
}

bool frontier_round::done () const
{
	return completed_m >= config.consideration_count && !candidates.empty ();
}

bool frontier_round::empty_range () const
{
	return completed_m >= config.consideration_count * 2 && candidates.empty ();
}

bool frontier_round::settled () const
{
	return done () || empty_range ();
}

std::optional<nano::account> frontier_round::conclude () const
{
	if (!candidates.empty ())
	{
		return *std::prev (candidates.end ());
	}
	if (completed_m > 0)
	{
		return range_end;
	}
	return std::nullopt;
}

size_t frontier_round::completed () const
{
	return completed_m;
}

size_t frontier_round::candidate_count () const
{
	return candidates.size ();
}

/*
 *
 */

frontier_scan_engine::round_state::round_state (nano::frontier_scan_config const & config, nano::account position_a, nano::account range_end) :
	position{ position_a },
	votes{ config, position_a, range_end }
{
}

bool frontier_scan_engine::round_state::owns (id_t tag_id) const
{
	return std::find (tag_ids.begin (), tag_ids.end (), tag_id) != tag_ids.end ();
}

bool frontier_scan_engine::round_state::erase (id_t tag_id)
{
	auto const it = std::find (tag_ids.begin (), tag_ids.end (), tag_id);
	if (it == tag_ids.end ())
	{
		return false;
	}
	tag_ids.erase (it);
	return true;
}

frontier_scan_engine::head_state::head_state (size_t index_a, nano::account start_a, nano::account end_a) :
	index{ index_a },
	start{ start_a },
	end{ end_a },
	cursor{ start_a }
{
}

void frontier_scan_engine::head_state::reset ()
{
	cursor = start;
	round.reset ();
	pause_until = {};
	rounds = 0;
	processed = 0;
}

frontier_scan_engine::frontier_scan_engine (nano::frontier_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
	nano::uint256_t max_account = std::numeric_limits<nano::uint256_t>::max ();
	nano::uint256_t range_size = max_account / config.head_parallelism;

	heads.reserve (config.head_parallelism);
	for (unsigned i = 0; i < config.head_parallelism; ++i)
	{
		nano::uint256_t start = (i == 0) ? 1 : i * range_size;
		nano::uint256_t end = (i == config.head_parallelism - 1) ? max_account : start + range_size;
		heads.emplace_back (i, nano::account{ start }, nano::account{ end });
	}

	release_assert (!heads.empty ());
}

void frontier_scan_engine::settle (std::chrono::steady_clock::time_point now, probes const & probes_a)
{
	for (auto & head : heads)
	{
		if (!head.round)
		{
			continue;
		}

		auto & round = *head.round;
		bool ripe = round.votes.settled ();
		bool capped = round.launched >= config.consideration_count * max_round_samples_factor;

		if (!ripe)
		{
			bool const pacing_open = round.launched < config.consideration_count || now >= round.last_launch + config.cooldown;
			if ((pacing_open || capped) && probes_a.count_inflight (round.tag_ids) == 0)
			{
				if (capped)
				{
					stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::sample_cap);
					ripe = true;
				}
				else if (probes_a.peer_status (round.used) == peer_probe_status::none)
				{
					ripe = true;
				}
			}
		}

		if (ripe)
		{
			conclude (head, now);
		}
	}
}

std::optional<frontier_scan_engine::launch_slot> frontier_scan_engine::next_launch (std::chrono::steady_clock::time_point now, probes const & probes_a)
{
	std::optional<peer_probe_status> empty_probe;

	for (size_t offset = 0; offset < heads.size (); ++offset)
	{
		auto const index = (robin + offset) % heads.size ();
		auto & head = heads[index];

		if (head.round)
		{
			auto const & round = *head.round;
			if (round.votes.settled ())
			{
				continue;
			}
			if (round.launched >= config.consideration_count * max_round_samples_factor)
			{
				continue;
			}
			if (round.launched >= config.consideration_count && now < round.last_launch + config.cooldown)
			{
				continue;
			}

			auto const status = probes_a.peer_status (round.used);
			if (status == peer_probe_status::available)
			{
				robin = (index + 1) % heads.size ();
				return launch_slot{ index, round.position, round.used };
			}
			continue;
		}

		if (now < head.pause_until)
		{
			continue;
		}
		if (!empty_probe)
		{
			empty_probe = probes_a.peer_status (std::span<nano::account const>{});
		}
		if (*empty_probe == peer_probe_status::available)
		{
			robin = (index + 1) % heads.size ();
			return launch_slot{ index, head.cursor, std::span<nano::account const>{} };
		}
	}

	return std::nullopt;
}

void frontier_scan_engine::commit (size_t head_index, nano::account const & position, nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now)
{
	release_assert (head_index < heads.size ());
	auto & head = heads[head_index];

	if (!head.round)
	{
		debug_assert (position == head.cursor);
		head.round = std::make_unique<round_state> (config, head.cursor, head.end);
	}

	auto & round = *head.round;
	debug_assert (position == round.position);
	debug_assert (!round.owns (tag_id));
	debug_assert (std::find (round.used.begin (), round.used.end (), node_id) == round.used.end ());

	round.used.push_back (node_id);
	round.tag_ids.push_back (tag_id);
	++round.launched;
	round.last_launch = now;

	stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::sample);
}

void frontier_scan_engine::erase_sample (size_t head_index, id_t tag_id)
{
	if (head_index >= heads.size ())
	{
		stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::unknown_id);
		return;
	}

	auto & head = heads[head_index];
	if (!head.round || !head.round->erase (tag_id))
	{
		stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::unknown_id);
	}
}

bool frontier_scan_engine::process (id_t tag_id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	auto & head = find_head (start);
	if (!head.round || head.round->position != start || !head.round->erase (tag_id))
	{
		return false;
	}

	head.round->votes.feed (frontiers);
	return true;
}

void frontier_scan_engine::reset ()
{
	for (auto & head : heads)
	{
		head.reset ();
	}
	robin = 0;
}

nano::container_info frontier_scan_engine::container_info () const
{
	auto collect_progress = [&] () {
		nano::container_info info;
		for (auto const & head : heads)
		{
			boost::multiprecision::cpp_dec_float_50 start{ head.start.number ().str () };
			boost::multiprecision::cpp_dec_float_50 cursor{ head.cursor.number ().str () };
			boost::multiprecision::cpp_dec_float_50 end{ head.end.number ().str () };
			boost::multiprecision::cpp_dec_float_50 progress = (cursor - start) * boost::multiprecision::cpp_dec_float_50 (1000000) / (end - start);
			info.put (std::to_string (head.index), progress.convert_to<std::uint64_t> ());
		}
		return info;
	};

	auto total_processed = std::accumulate (heads.begin (), heads.end (), std::size_t{ 0 }, [] (auto total, auto const & head) {
		return total + head.processed;
	});
	auto open_rounds = std::count_if (heads.begin (), heads.end (), [] (auto const & head) {
		return head.round != nullptr;
	});
	auto tracked_ids = std::accumulate (heads.begin (), heads.end (), std::size_t{ 0 }, [] (auto total, auto const & head) {
		return total + (head.round ? head.round->tag_ids.size () : 0);
	});

	nano::container_info info;
	info.put ("total_processed", total_processed);
	info.put ("open_rounds", open_rounds);
	info.put ("tracked_ids", tracked_ids);
	info.add ("progress", collect_progress ());
	return info;
}

void frontier_scan_engine::conclude (head_state & head, std::chrono::steady_clock::time_point now)
{
	release_assert (head.round != nullptr);
	auto & round = *head.round;

	bool const clean = round.votes.done ();
	bool const empty_clean = round.votes.empty_range ();
	auto const completed = round.votes.completed ();
	auto const candidates = round.votes.candidate_count ();
	auto target = round.votes.conclude ();

	if (target)
	{
		if (candidates > 0)
		{
			stats.inc (nano::stat::type::bootstrap_frontier_scan, clean ? nano::stat::detail::done : nano::stat::detail::done_partial);
			head.processed += candidates;
		}
		else
		{
			stats.inc (nano::stat::type::bootstrap_frontier_scan, empty_clean ? nano::stat::detail::done_empty : nano::stat::detail::done_empty_partial);
		}

		debug_assert (target->number () > head.cursor.number ());
		head.cursor = *target;
		if (head.cursor.number () >= head.end.number ())
		{
			stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_range);
			head.cursor = head.start;
		}
	}
	else
	{
		debug_assert (completed == 0);
		stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::done_none);
	}

	++head.rounds;
	head.round.reset ();
	if (!clean)
	{
		head.pause_until = now + config.cooldown;
	}
}

auto frontier_scan_engine::find_head (nano::account const & position) -> head_state &
{
	return const_cast<head_state &> (std::as_const (*this).find_head (position));
}

auto frontier_scan_engine::find_head (nano::account const & position) const -> head_state const &
{
	auto result = heads.begin ();
	for (auto it = heads.begin (); it != heads.end (); ++it)
	{
		if (it->start.number () > position.number ())
		{
			break;
		}
		result = it;
	}

	release_assert (result != heads.end ());
	return *result;
}
}
