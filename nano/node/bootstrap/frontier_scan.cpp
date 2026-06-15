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
frontier_round::frontier_round (nano::frontier_scan_config const & config_a, nano::account position_a, nano::account range_end_a, std::function<void ()> sample_reserved_a) :
	config{ config_a },
	position_m{ position_a },
	range_end_m{ range_end_a },
	sample_reserved_m{ std::move (sample_reserved_a) }
{
}

void frontier_round::feed (std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	++completed_m;

	// Only accounts after the current cursor can advance this range
	for (auto const & [account, _] : frontiers)
	{
		if (account.number () > position_m.number ())
		{
			candidates.insert (account);
		}
	}

	// Keep the nearest candidates; the far end is least useful for incremental scanning
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
		// Advance to the farthest retained nearby candidate so the next round resumes after useful work
		return *std::prev (candidates.end ());
	}
	if (completed_m > 0)
	{
		// Completed samples with no candidates indicate the rest of this head range is empty
		return range_end_m;
	}
	return std::nullopt;
}

nano::account const & frontier_round::position () const
{
	return position_m;
}

size_t frontier_round::completed () const
{
	return completed_m;
}

size_t frontier_round::candidate_count () const
{
	return candidates.size ();
}

void frontier_round::sample_reserved ()
{
	if (sample_reserved_m)
	{
		sample_reserved_m ();
	}
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
		// Account zero is not a real frontier position, so the first range starts at one
		nano::uint256_t start = (i == 0) ? 1 : i * range_size;
		// The last range absorbs division remainder and reaches the full uint256 account space
		nano::uint256_t end = (i == config.head_parallelism - 1) ? max_account : start + range_size;
		heads.emplace_back (i, nano::account{ start }, nano::account{ end });
	}

	release_assert (!heads.empty ());
}

void frontier_scan_engine::settle (std::chrono::steady_clock::time_point now, peer_probes const & probe)
{
	for (auto & head : heads)
	{
		if (!head.round)
		{
			continue;
		}

		auto & round = *head.round;

		if (round.launched () == 0)
		{
			continue;
		}

		bool ripe = round.settled ();
		bool capped = round.launched () >= config.consideration_count * max_round_samples_factor;

		if (!ripe)
		{
			// Before the quorum target we launch eagerly; after that we pace extra samples by cooldown
			bool const pacing_open = round.launched () < config.consideration_count || now >= round.last_launch () + config.cooldown;
			if ((pacing_open || capped) && probe.count_inflight (round.tag_ids ()) == 0)
			{
				if (capped)
				{
					// Avoid unbounded sampling if peers keep disappearing or disagreeing
					stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::sample_cap);
					ripe = true;
				}
				else if (probe.peer_status (round.used ()) == peer_probe_status::none)
				{
					// No unsampled peer remains, so the best available evidence must conclude the round
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

std::shared_ptr<frontier_round> frontier_scan_engine::next_round (std::chrono::steady_clock::time_point now, peer_probes const & probe)
{
	std::optional<peer_probe_status> empty_probe;

	for (size_t offset = 0; offset < heads.size (); ++offset)
	{
		auto const index = (robin + offset) % heads.size ();
		auto & head = heads[index];

		if (head.round)
		{
			auto const & round = head.round;
			if (round->settled ())
			{
				// Settled rounds are handled by settle (); next_round never mutates the cursor
				continue;
			}
			if (round->launched () >= config.consideration_count * max_round_samples_factor)
			{
				continue;
			}
			if (round->launched () >= config.consideration_count && now < round->last_launch () + config.cooldown)
			{
				continue;
			}

			// Excluding already-used node IDs keeps all samples in a round on distinct peers
			auto const status = probe.peer_status (round->used ());
			if (status == peer_probe_status::available)
			{
				return round;
			}
			continue;
		}

		if (now < head.pause_until)
		{
			continue;
		}
		if (!empty_probe)
		{
			// Empty exclusion probes are identical for idle heads, so perform it at most once per next_round
			empty_probe = probe.peer_status (std::span<nano::account const>{});
		}
		if (*empty_probe == peer_probe_status::available)
		{
			auto round = std::make_shared<frontier_round> (config, head.cursor, head.end, [this, index] () {
				robin = (index + 1) % heads.size ();
				stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::sample);
			});
			head.round = round;
			return round;
		}
	}

	return nullptr;
}

void frontier_scan_engine::erase_sample (id_t tag_id, nano::account const & start)
{
	auto & head = find_head (start);
	if (!head.round || head.round->position () != start || !head.round->erase (tag_id))
	{
		stats.inc (nano::stat::type::bootstrap_frontier_scan, nano::stat::detail::unknown_id);
	}
}

bool frontier_scan_engine::process (id_t tag_id, nano::account const & start, std::deque<std::pair<nano::account, nano::block_hash>> const & frontiers)
{
	auto & head = find_head (start);
	if (!head.round || head.round->position () != start || !head.round->erase (tag_id))
	{
		// Guard against stale responses from an older round at the same head
		return false;
	}

	head.round->feed (frontiers);
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
			// Store fixed-point per-million progress to avoid relying on floating container_info values
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
		return total + (head.round ? head.round->tag_ids ().size () : 0);
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

	bool const clean = round.done ();
	bool const empty_clean = round.empty_range ();
	auto const completed = round.completed ();
	auto const candidates = round.candidate_count ();
	auto target = round.conclude ();

	if (target)
	{
		if (candidates > 0)
		{
			// Clean means the configured quorum was reached; otherwise this is a best-effort advance
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
			// Each head loops over its assigned range so the scan keeps refreshing frontier coverage
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
		// Partial rounds back off before retrying to avoid tight loops when peers are scarce
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
		// Ranges are sorted by start, so the last start not greater than position owns it
		result = it;
	}

	release_assert (result != heads.end ());
	return *result;
}
}
