#include <nano/lib/blocks.hpp>
#include <nano/lib/container_info.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/stats_enums.hpp>
#include <nano/node/bootstrap/topo_scan.hpp>

#include <algorithm>
#include <iterator>
#include <unordered_set>

namespace nano::bootstrap
{
topo_round::topo_round (nano::topo_scan_config const & config_a, nano::topo_key position_a, unsigned quorum_a) :
	config{ config_a },
	position{ position_a },
	quorum_m{ std::max (1u, quorum_a) }
{
}

void topo_round::feed (std::deque<nano::topo_key> const & entries)
{
	++completed_m;

	for (auto const & key : entries)
	{
		if (position < key)
		{
			candidates_m.insert (key);
		}
	}

	while (candidates_m.size () > config.candidates)
	{
		candidates_m.erase (std::prev (candidates_m.end ()));
	}
}

bool topo_round::done () const
{
	return completed_m >= quorum_m && !candidates_m.empty ();
}

bool topo_round::empty_page () const
{
	return completed_m >= quorum_m && candidates_m.empty ();
}

bool topo_round::settled () const
{
	return done () || empty_page ();
}

std::optional<nano::topo_key> topo_round::conclude () const
{
	if (!candidates_m.empty ())
	{
		return *std::prev (candidates_m.end ());
	}
	if (completed_m > 0)
	{
		return position;
	}
	return std::nullopt;
}

std::deque<nano::topo_key> topo_round::candidates () const
{
	return { candidates_m.begin (), candidates_m.end () };
}

size_t topo_round::completed () const
{
	return completed_m;
}

size_t topo_round::candidate_count () const
{
	return candidates_m.size ();
}

unsigned topo_round::quorum () const
{
	return quorum_m;
}

/*
 *
 */

topo_scan_engine::round_state::round_state (nano::topo_scan_config const & config, nano::topo_key position_a, unsigned quorum_a) :
	position{ position_a },
	votes{ config, position_a, quorum_a }
{
}

bool topo_scan_engine::round_state::owns (id_t tag_id) const
{
	return std::find (tag_ids.begin (), tag_ids.end (), tag_id) != tag_ids.end ();
}

bool topo_scan_engine::round_state::erase (id_t tag_id)
{
	auto const it = std::find (tag_ids.begin (), tag_ids.end (), tag_id);
	if (it == tag_ids.end ())
	{
		return false;
	}
	tag_ids.erase (it);
	return true;
}

topo_scan_engine::head_state::head_state (size_t index_a) :
	index{ index_a }
{
}

bool topo_scan_engine::head_state::spear () const
{
	return index == 0;
}

void topo_scan_engine::head_state::reset (nano::topo_key const & start)
{
	cursor = spear () ? start : nano::topo_key{};
	round.reset ();
	pause_until = {};
	floor_height = 0;
	ceiling_height = 0;
	empty_rounds = 0;
	rounds = 0;
	pages = 0;
	processed = 0;
}

topo_scan_engine::topo_scan_engine (nano::topo_scan_config const & config_a, nano::stats & stats_a) :
	config{ config_a },
	stats{ stats_a }
{
	auto const head_count = std::max (1u, config.head_count);
	heads.reserve (head_count);
	for (unsigned i = 0; i < head_count; ++i)
	{
		heads.emplace_back (i);
	}
}

void topo_scan_engine::orient (nano::topo_key const & start)
{
	submit_frontier = start;
	spear_at_tip = false;
	frontier_gaps = 0;
	members.clear ();
	members_by_key.clear ();
	fetch_queue.clear ();
	submit_queue.clear ();
	fetches.clear ();
	for (auto & head : heads)
	{
		head.reset (start);
		if (!head.spear ())
		{
			refresh_repair_band (head);
		}
	}
	robin = 0;
	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_orient);
}

nano::topo_key topo_scan_engine::cursor () const
{
	return submit_frontier;
}

void topo_scan_engine::settle (std::chrono::steady_clock::time_point now, probes const & probes_a)
{
	for (auto & head : heads)
	{
		if (!head.round)
		{
			continue;
		}

		auto & round = *head.round;
		bool ripe = round.votes.settled ();
		auto const quorum_l = round.votes.quorum ();
		bool capped = round.launched >= quorum_l * max_round_samples_factor;

		if (!ripe)
		{
			bool const pacing_open = round.launched < quorum_l || now >= round.last_launch + config.cooldown;
			if ((pacing_open || capped) && probes_a.count_inflight (round.tag_ids) == 0)
			{
				if (capped)
				{
					stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::sample_cap);
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

std::optional<topo_scan_engine::page_slot> topo_scan_engine::next_page (std::chrono::steady_clock::time_point now, probes const & probes_a)
{
	if (discovery_backpressured ())
	{
		return std::nullopt;
	}

	std::optional<peer_probe_status> empty_probe;

	for (size_t offset = 0; offset < heads.size (); ++offset)
	{
		auto const index = (robin + offset) % heads.size ();
		auto & head = heads[index];
		auto const quorum_l = quorum (head);

		if (head.round)
		{
			auto const & round = *head.round;
			if (round.votes.settled ())
			{
				continue;
			}
			if (round.launched >= quorum_l * max_round_samples_factor)
			{
				continue;
			}
			if (round.launched >= quorum_l && now < round.last_launch + config.cooldown)
			{
				continue;
			}

			auto const status = probes_a.peer_status (round.used);
			if (status == peer_probe_status::available)
			{
				robin = (index + 1) % heads.size ();
				return page_slot{ index, round.position, round.used, !head.spear () };
			}
			continue;
		}

		if (!head.spear ())
		{
			if (repair_quiesced () || heads.front ().cursor.topo_height < config.repair_activation_height)
			{
				continue;
			}
			if (head.ceiling_height == 0 || head.cursor.topo_height >= head.ceiling_height)
			{
				refresh_repair_band (head);
			}
		}

		if (now < head.pause_until)
		{
			continue;
		}
		if (head.spear () && spear_at_tip && now < head.pause_until)
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
			return page_slot{ index, head.cursor, std::span<nano::account const>{}, !head.spear () };
		}
	}

	return std::nullopt;
}

void topo_scan_engine::commit_page (size_t head_index, nano::topo_key const & position, nano::account const & node_id, id_t tag_id, std::chrono::steady_clock::time_point now)
{
	release_assert (head_index < heads.size ());
	auto & head = heads[head_index];

	if (!head.round)
	{
		debug_assert (position == head.cursor);
		head.round = std::make_unique<round_state> (config, head.cursor, quorum (head));
	}

	auto & round = *head.round;
	debug_assert (position == round.position);
	debug_assert (!round.owns (tag_id));
	debug_assert (std::find (round.used.begin (), round.used.end (), node_id) == round.used.end ());

	round.used.push_back (node_id);
	round.tag_ids.push_back (tag_id);
	++round.launched;
	round.last_launch = now;
	if (head.spear ())
	{
		spear_at_tip = false;
	}

	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::sample);
}

bool topo_scan_engine::process_page (id_t tag_id, nano::topo_key const & start, std::deque<nano::topo_key> const & entries)
{
	for (auto & head : heads)
	{
		if (head.round && head.round->position == start && head.round->erase (tag_id))
		{
			head.round->votes.feed (entries);
			return true;
		}
	}
	return false;
}

void topo_scan_engine::erase_page (id_t tag_id, nano::topo_key const & start)
{
	for (auto & head : heads)
	{
		if (head.round && head.round->position == start && head.round->erase (tag_id))
		{
			return;
		}
	}
	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::unknown_id);
}

bool topo_scan_engine::fetch_ready (std::chrono::steady_clock::time_point now) const
{
	return !fetch_queue.empty () && fetch_queue.begin ()->first <= now && fetches.size () < config.max_blocks_outstanding;
}

std::deque<nano::block_hash> topo_scan_engine::next_fetch_candidates (std::chrono::steady_clock::time_point now, size_t max_count) const
{
	std::deque<nano::block_hash> result;
	for (auto it = fetch_queue.begin (); it != fetch_queue.end () && result.size () < max_count; ++it)
	{
		if (it->first > now)
		{
			break;
		}
		auto member_it = members.find (it->second.hash);
		if (member_it != members.end () && member_it->second.fetch_queued)
		{
			result.push_back (member_it->second.hash);
		}
	}
	return result;
}

void topo_scan_engine::commit_fetch (std::deque<nano::block_hash> const & requested, std::deque<nano::block_hash> const & already_local, id_t tag_id, std::chrono::steady_clock::time_point now)
{
	for (auto const & hash : already_local)
	{
		if (auto it = members.find (hash); it != members.end ())
		{
			mark_resolved (it->second);
		}
	}
	advance_frontier ();

	std::deque<nano::block_hash> committed;
	for (auto const & hash : requested)
	{
		auto it = members.find (hash);
		if (it == members.end ())
		{
			continue;
		}
		auto & item = it->second;
		if (item.state != member_state::discovered || !item.fetch_queued)
		{
			continue;
		}
		erase_fetch_queue (item);
		item.state = member_state::fetching;
		item.next_action = now;
		committed.push_back (hash);
	}

	if (!committed.empty ())
	{
		fetches[tag_id] = fetch_state{ committed };
		stats.add (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_fetch, committed.size ());
	}
	if (!already_local.empty ())
	{
		stats.add (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_fetch_local, already_local.size ());
	}
}

bool topo_scan_engine::process_blocks (id_t tag_id, std::deque<std::shared_ptr<nano::block>> const & blocks, std::chrono::steady_clock::time_point now)
{
	auto fetch_it = fetches.find (tag_id);
	if (fetch_it == fetches.end ())
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::unknown_id);
		return false;
	}

	std::unordered_set<nano::block_hash> remaining{ fetch_it->second.hashes.begin (), fetch_it->second.hashes.end () };
	for (auto const & block : blocks)
	{
		if (!block)
		{
			continue;
		}

		auto const hash = block->hash ();
		remaining.erase (hash);
		auto member_it = members.find (hash);
		if (member_it == members.end ())
		{
			continue;
		}
		auto & item = member_it->second;
		if (item.state != member_state::fetching)
		{
			continue;
		}

		item.block = block;
		item.state = member_state::fetched;
		item.next_action = now;
		if (!item.frontier)
		{
			enqueue_submit (item, now);
		}
	}

	for (auto const & hash : remaining)
	{
		if (auto member_it = members.find (hash); member_it != members.end ())
		{
			auto & item = member_it->second;
			if (item.state == member_state::fetching)
			{
				item.state = member_state::discovered;
				enqueue_fetch (item, now + config.retry_interval);
				stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_fetch_retry);
			}
		}
	}
	if (!remaining.empty ())
	{
		stats.add (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_blocks_missing, remaining.size ());
	}

	fetches.erase (fetch_it);
	return true;
}

void topo_scan_engine::erase_fetch (id_t tag_id, std::chrono::steady_clock::time_point now)
{
	auto fetch_it = fetches.find (tag_id);
	if (fetch_it == fetches.end ())
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::unknown_id);
		return;
	}

	for (auto const & hash : fetch_it->second.hashes)
	{
		if (auto member_it = members.find (hash); member_it != members.end ())
		{
			auto & item = member_it->second;
			if (item.state == member_state::fetching)
			{
				item.state = member_state::discovered;
				enqueue_fetch (item, now + config.retry_interval);
			}
		}
	}
	fetches.erase (fetch_it);
	stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_fetch_retry);
}

bool topo_scan_engine::submit_ready (std::chrono::steady_clock::time_point now) const
{
	auto next = members_by_key.upper_bound (submit_frontier);
	if (next != members_by_key.end ())
	{
		auto const member_it = members.find (next->second);
		if (member_it != members.end ())
		{
			auto const & item = member_it->second;
			if (item.frontier && item.state == member_state::fetched && item.block && item.next_action <= now)
			{
				return true;
			}
		}
	}
	return !submit_queue.empty () && submit_queue.begin ()->first <= now;
}

std::deque<std::shared_ptr<nano::block>> topo_scan_engine::next_submit_batch (std::chrono::steady_clock::time_point now, size_t max_count)
{
	std::deque<std::shared_ptr<nano::block>> result;
	auto batch_cursor = submit_frontier;

	while (result.size () < max_count)
	{
		auto next = members_by_key.upper_bound (batch_cursor);
		if (next == members_by_key.end ())
		{
			break;
		}
		auto member_it = members.find (next->second);
		if (member_it == members.end ())
		{
			break;
		}
		auto & item = member_it->second;
		if (!item.frontier || item.state != member_state::fetched || !item.block || item.next_action > now)
		{
			break;
		}

		item.state = member_state::submitted;
		result.push_back (item.block);
		batch_cursor = item.key;
	}

	for (auto it = submit_queue.begin (); it != submit_queue.end () && result.size () < max_count;)
	{
		if (it->first > now)
		{
			break;
		}
		auto member_it = members.find (it->second.hash);
		it = submit_queue.erase (it);
		if (member_it == members.end ())
		{
			continue;
		}
		auto & item = member_it->second;
		item.submit_queued = false;
		if (item.frontier || item.state != member_state::fetched || !item.block)
		{
			continue;
		}
		item.state = member_state::submitted;
		result.push_back (item.block);
	}

	if (!result.empty ())
	{
		stats.add (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_submit, result.size ());
	}
	return result;
}

bool topo_scan_engine::inspect (nano::block_hash const & hash, nano::block_status result, std::chrono::steady_clock::time_point now)
{
	auto member_it = members.find (hash);
	if (member_it == members.end ())
	{
		return false;
	}

	auto & item = member_it->second;
	if (dependency_gap (result))
	{
		if (item.frontier)
		{
			item.state = member_state::fetched;
			item.next_action = now + config.retry_interval;
			if (!item.frontier_gap)
			{
				item.frontier_gap = true;
				++frontier_gaps;
			}
			stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_result_gap);
		}
		else
		{
			erase_member (member_it);
			stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_result_gap);
		}
		return true;
	}

	if (result == nano::block_status::progress || result == nano::block_status::old || terminal (result))
	{
		if (item.frontier)
		{
			item.state = member_state::done;
			item.block.reset ();
			if (result == nano::block_status::progress || result == nano::block_status::old)
			{
				stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_result_progress);
			}
			else
			{
				stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_result_terminal);
			}
			advance_frontier ();
		}
		else
		{
			erase_member (member_it);
		}
		return true;
	}

	return true;
}

bool topo_scan_engine::caught_up () const
{
	bool const pages_inflight = std::any_of (heads.begin (), heads.end (), [] (auto const & head) {
		return head.round != nullptr && !head.round->tag_ids.empty ();
	});
	return spear_at_tip && members.empty () && fetches.empty () && !pages_inflight;
}

size_t topo_scan_engine::queued () const
{
	return members.size ();
}

size_t topo_scan_engine::fetch_inflight () const
{
	return fetches.size ();
}

size_t topo_scan_engine::frontier_gap_count () const
{
	return frontier_gaps;
}

void topo_scan_engine::reset ()
{
	orient ({});
}

nano::container_info topo_scan_engine::container_info () const
{
	auto collect_counter = [&] (auto getter) {
		nano::container_info result;
		for (auto const & head : heads)
		{
			result.put (std::to_string (head.index), getter (head));
		}
		return result;
	};

	auto tracked = std::count_if (members.begin (), members.end (), [] (auto const & item) {
		return item.second.state == member_state::submitted;
	});

	nano::container_info info;
	info.put ("members", members.size ());
	info.put ("tracked", tracked);
	info.put ("fetch_queue", fetch_queue.size ());
	info.put ("submit_queue", submit_queue.size ());
	info.put ("fetches", fetches.size ());
	info.put ("frontier_gaps", frontier_gaps);
	info.put ("caught_up", caught_up () ? 1 : 0);

	auto cursors_info = collect_counter ([] (head_state const & head) { return head.cursor.topo_height; });
	cursors_info.put ("submit_frontier", submit_frontier.topo_height);
	info.add ("cursors", cursors_info);
	info.add ("rounds", collect_counter ([] (head_state const & head) { return head.rounds; }));
	info.add ("pages_per_head", collect_counter ([] (head_state const & head) { return head.pages; }));
	return info;
}

unsigned topo_scan_engine::quorum (head_state const & head) const
{
	return std::max (1u, head.spear () ? config.consideration_count : config.repair_consideration_count);
}

void topo_scan_engine::conclude (head_state & head, std::chrono::steady_clock::time_point now)
{
	release_assert (head.round != nullptr);
	auto & round = *head.round;

	auto const target = round.votes.conclude ();
	auto const candidates = round.votes.candidates ();
	bool const clean = round.votes.done () || round.votes.empty_page ();

	if (target && *target != round.position)
	{
		++head.pages;
		auto discovered = discover (candidates);
		if (discovered.last)
		{
			debug_assert (head.cursor < *discovered.last || head.cursor == *discovered.last);
			head.cursor = *discovered.last;
		}
		if (!discovered.blocked)
		{
			head.cursor = *target;
		}
		head.empty_rounds = 0;
		head.processed += candidates.size ();
		stats.inc (nano::stat::type::bootstrap_topo_scan, clean ? nano::stat::detail::done : nano::stat::detail::done_partial);
	}
	else if (target)
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, clean ? nano::stat::detail::done_empty : nano::stat::detail::done_empty_partial);
		if (head.spear ())
		{
			++head.empty_rounds;
			if (head.empty_rounds >= 2)
			{
				spear_at_tip = true;
				head.pause_until = now + config.poll_interval;
				stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_caught_up);
			}
			else
			{
				head.pause_until = now + config.cooldown;
			}
		}
		else
		{
			wrap_repair (head, now);
		}
	}
	else
	{
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::done_none);
		head.pause_until = now + config.cooldown;
	}

	++head.rounds;
	head.round.reset ();

	if (!head.spear () && head.cursor.topo_height >= head.ceiling_height)
	{
		wrap_repair (head, now);
	}
	if (!clean)
	{
		head.pause_until = std::max (head.pause_until, now + config.cooldown);
	}
}

auto topo_scan_engine::discover (std::deque<nano::topo_key> const & keys) -> discover_result
{
	discover_result result;
	for (auto const & key : keys)
	{
		if (key.hash.is_zero ())
		{
			continue;
		}
		if (key <= submit_frontier)
		{
			result.last = key;
			continue;
		}
		if (members_by_key.find (key) != members_by_key.end () || members.find (key.hash) != members.end ())
		{
			result.last = key;
			continue;
		}
		if (members.size () >= config.max_blocks_queued)
		{
			result.blocked = true;
			stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_queue_full);
			break;
		}

		member item;
		item.key = key;
		item.hash = key.hash;
		item.frontier = key > submit_frontier;
		auto [it, inserted] = members.emplace (key.hash, std::move (item));
		release_assert (inserted);
		members_by_key.emplace (key, key.hash);
		enqueue_fetch (it->second, {});
		result.last = key;
		stats.inc (nano::stat::type::bootstrap_topo_scan, nano::stat::detail::topo_page);
	}
	return result;
}

void topo_scan_engine::refresh_repair_band (head_state & head)
{
	release_assert (!head.spear ());
	auto const repair_count = heads.size () > 1 ? heads.size () - 1 : 1;
	auto const repair_index = head.index - 1;
	auto const frontier_height = heads.front ().cursor.topo_height;

	head.floor_height = (repair_index * frontier_height) / repair_count;
	head.ceiling_height = ((repair_index + 1) * frontier_height) / repair_count;
	if (head.ceiling_height <= head.floor_height)
	{
		head.ceiling_height = head.floor_height + 1;
	}
	head.cursor = nano::topo_key{ head.floor_height, nano::block_hash{} };
}

void topo_scan_engine::wrap_repair (head_state & head, std::chrono::steady_clock::time_point now)
{
	refresh_repair_band (head);
	head.pause_until = now + config.cooldown;
	head.empty_rounds = 0;
}

bool topo_scan_engine::repair_quiesced () const
{
	return spear_at_tip && frontier_gaps == 0 && members.empty () && fetches.empty ();
}

bool topo_scan_engine::discovery_backpressured () const
{
	return members.size () >= config.max_blocks_queued || frontier_gaps > config.gap_threshold;
}

void topo_scan_engine::enqueue_fetch (member & item, std::chrono::steady_clock::time_point at)
{
	erase_fetch_queue (item);
	item.next_action = at;
	item.fetch_queued = true;
	fetch_queue.emplace (at, item.key);
}

void topo_scan_engine::erase_fetch_queue (member & item)
{
	if (item.fetch_queued)
	{
		fetch_queue.erase ({ item.next_action, item.key });
		item.fetch_queued = false;
	}
}

void topo_scan_engine::enqueue_submit (member & item, std::chrono::steady_clock::time_point at)
{
	erase_submit_queue (item);
	item.next_action = at;
	item.submit_queued = true;
	submit_queue.emplace (at, item.key);
}

void topo_scan_engine::erase_submit_queue (member & item)
{
	if (item.submit_queued)
	{
		submit_queue.erase ({ item.next_action, item.key });
		item.submit_queued = false;
	}
}

void topo_scan_engine::erase_member (std::unordered_map<nano::block_hash, member>::iterator it)
{
	auto & item = it->second;
	erase_fetch_queue (item);
	erase_submit_queue (item);
	if (item.frontier_gap)
	{
		debug_assert (frontier_gaps > 0);
		--frontier_gaps;
	}
	members_by_key.erase (item.key);
	members.erase (it);
}

void topo_scan_engine::mark_resolved (member & item)
{
	erase_fetch_queue (item);
	erase_submit_queue (item);
	item.block.reset ();
	item.state = member_state::done;
}

void topo_scan_engine::advance_frontier ()
{
	while (true)
	{
		auto next = members_by_key.upper_bound (submit_frontier);
		if (next == members_by_key.end ())
		{
			break;
		}
		auto member_it = members.find (next->second);
		if (member_it == members.end () || member_it->second.state != member_state::done)
		{
			break;
		}

		submit_frontier = member_it->second.key;
		erase_member (member_it);
	}
}

bool topo_scan_engine::terminal (nano::block_status result) const
{
	switch (result)
	{
		case nano::block_status::bad_signature:
		case nano::block_status::negative_spend:
		case nano::block_status::fork:
		case nano::block_status::unreceivable:
		case nano::block_status::opened_burn_account:
		case nano::block_status::balance_mismatch:
		case nano::block_status::representative_mismatch:
		case nano::block_status::block_position:
		case nano::block_status::insufficient_work:
			return true;
		default:
			return false;
	}
}

bool topo_scan_engine::dependency_gap (nano::block_status result) const
{
	return result == nano::block_status::gap_previous || result == nano::block_status::gap_source || result == nano::block_status::gap_epoch_open_pending;
}
}
