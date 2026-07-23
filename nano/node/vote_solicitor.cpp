#include <nano/lib/blocks.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/messages/publish.hpp>
#include <nano/node/block_rebroadcaster.hpp>
#include <nano/node/election_behavior.hpp>
#include <nano/node/network.hpp>
#include <nano/node/vote_solicitor.hpp>

#include <algorithm>
#include <limits>

/*
 * vote_solicitor_round
 */

nano::vote_solicitor_round::vote_solicitor_round (std::vector<nano::representative> const & reps, limits const & limits_a) :
	limits_m{ limits_a },
	reps_requests{ reps },
	reps_broadcasts{ reps }
{
}

auto nano::vote_solicitor_round::decide_request (nano::solicitation const & solicitation, nano::account const & rep) -> rep_decision
{
	auto const existing = solicitation.votes.find (rep);
	bool const exists = existing != solicitation.votes.end ();
	if (exists && existing->second.hash != solicitation.winner->hash ())
	{
		return rep_decision::solicit_fork;
	}
	// Before quorum any vote is conclusive, after quorum only a final vote is
	bool const is_final = exists && (!solicitation.quorum || existing->second.timestamp == std::numeric_limits<uint64_t>::max ());
	if (!exists || !is_final)
	{
		return rep_decision::solicit;
	}
	return rep_decision::skip;
}

auto nano::vote_solicitor_round::decide_broadcast (nano::solicitation const & solicitation, nano::account const & rep) -> rep_decision
{
	auto const existing = solicitation.votes.find (rep);
	bool const exists = existing != solicitation.votes.end ();
	if (!exists)
	{
		return rep_decision::solicit;
	}
	if (existing->second.hash != solicitation.winner->hash ())
	{
		return rep_decision::solicit_fork;
	}
	return rep_decision::skip;
}

auto nano::vote_solicitor_round::add (nano::solicitation const & solicitation) -> result
{
	debug_assert (solicitation.winner != nullptr);

	result result;
	if (solicitation.request)
	{
		result.requested = add_requests (solicitation);
	}
	if (solicitation.broadcast)
	{
		result.broadcasted = add_broadcasts (solicitation);
	}
	return result;
}

bool nano::vote_solicitor_round::add_requests (nano::solicitation const & solicitation)
{
	bool added = false;
	std::size_t count = 0;
	for (auto it = reps_requests.begin (); it != reps_requests.end () && count < limits_m.max_election_requests;)
	{
		auto const decision = decide_request (solicitation, it->account);
		if (decision == rep_decision::skip)
		{
			++it;
			continue;
		}
		if (channel_full (it->channel))
		{
			it = reps_requests.erase (it); // Stop soliciting this channel for the rest of the round
			continue;
		}
		requests_m[it->channel].emplace_back (solicitation.winner->hash (), solicitation.winner->root ());
		count += (decision == rep_decision::solicit_fork) ? 0 : 1;
		added = true;
		++it;
	}
	return added;
}

bool nano::vote_solicitor_round::add_broadcasts (nano::solicitation const & solicitation)
{
	if (block_broadcasts >= limits_m.max_block_broadcasts)
	{
		return false;
	}
	++block_broadcasts;

	channels_t targets;
	std::size_t count = 0;
	for (auto const & rep : reps_broadcasts)
	{
		if (count >= limits_m.max_election_broadcasts)
		{
			break;
		}
		auto const decision = decide_broadcast (solicitation, rep.account);
		if (decision == rep_decision::skip)
		{
			continue;
		}
		targets.push_back (rep.channel);
		count += (decision == rep_decision::solicit_fork) ? 0 : 1;
	}

	broadcasts_m.emplace_back (solicitation.winner, std::move (targets));
	return true;
}

auto nano::vote_solicitor_round::requests () const -> std::unordered_map<std::shared_ptr<nano::transport::channel>, roots_hashes_t> const &
{
	return requests_m;
}

auto nano::vote_solicitor_round::broadcasts () const -> std::vector<std::pair<std::shared_ptr<nano::block>, channels_t>> const &
{
	return broadcasts_m;
}

/*
 * vote_solicitor
 */

std::chrono::milliseconds nano::vote_solicitor::request_interval (nano::election_behavior behavior, std::chrono::milliseconds base_latency)
{
	switch (behavior)
	{
		case nano::election_behavior::manual:
		case nano::election_behavior::priority:
		case nano::election_behavior::hinted:
			return base_latency * 5;
		case nano::election_behavior::optimistic:
			return base_latency * 2;
	}
	debug_assert (false);
	return {};
}

auto nano::vote_solicitor::decide (nano::election_snapshot const & snapshot, pacing const & pacing_a, std::chrono::milliseconds base_latency, std::chrono::milliseconds broadcast_interval, std::chrono::steady_clock::time_point now) -> due
{
	bool allow_requests = false;
	bool allow_broadcasts = false;
	switch (snapshot.state)
	{
		case nano::election_state::active:
			allow_requests = true;
			allow_broadcasts = true;
			break;
		case nano::election_state::confirmed:
		case nano::election_state::expired_confirmed:
			allow_broadcasts = true; // Ensure the winner is broadcasted after confirmation
			break;
		default:
			return {};
	}

	due result;
	result.request = allow_requests && request_interval (snapshot.behavior, base_latency) < now - pacing_a.last_request;
	// Broadcast when enough time has passed since the last broadcast or the winner has changed
	result.broadcast = allow_broadcasts && (pacing_a.last_broadcast + broadcast_interval < now || snapshot.winner->hash () != pacing_a.last_broadcast_hash);
	return result;
}

nano::vote_solicitor::vote_solicitor (nano::network & network_a, nano::rep_crawler & rep_crawler_a, nano::block_rebroadcaster & block_rebroadcaster_a, nano::network_constants const & network_constants_a, nano::stats & stats_a, nano::logger & logger_a) :
	limits{ .max_block_broadcasts = network_constants_a.is_dev_network () ? 4u : 30u },
	round_interval{ network_constants_a.aec_loop_interval },
	base_latency{ network_constants_a.is_dev_network () ? std::chrono::milliseconds{ 25 } : std::chrono::milliseconds{ 1000 } },
	broadcast_interval{ network_constants_a.block_broadcast_interval },
	network{ network_a },
	rep_crawler{ rep_crawler_a },
	block_rebroadcaster{ block_rebroadcaster_a },
	network_constants{ network_constants_a },
	stats{ stats_a },
	logger{ logger_a }
{
}

nano::vote_solicitor::~vote_solicitor ()
{
	// Thread must be stopped before destruction
	debug_assert (!thread.joinable ());
}

void nano::vote_solicitor::start ()
{
	debug_assert (!thread.joinable ());

	thread = std::thread{ [this] () {
		nano::thread_role::set (nano::thread_role::name::vote_solicitor);
		run ();
	} };
}

void nano::vote_solicitor::stop ()
{
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		stopped = true;
		elections.clear ();
		pending_count = 0;
	}
	condition.notify_all ();
	if (thread.joinable ())
	{
		thread.join ();
	}
}

void nano::vote_solicitor::trigger (std::shared_ptr<nano::election> const & election)
{
	release_assert (election != nullptr);

	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (stopped)
		{
			return;
		}
		auto & entry = elections[election->qualified_root];
		entry.election = election;
		pending_count += entry.pending == nullptr ? 1 : 0;
		entry.pending = election;
	}
	condition.notify_all ();

	stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::triggered);
}

void nano::vote_solicitor::run ()
{
	nano::unique_lock<nano::mutex> lock{ mutex };
	while (!stopped)
	{
		condition.wait (lock, [this] { return stopped || pending_count > 0; });
		if (stopped)
		{
			break;
		}

		// Keep a fixed round cadence so the per round caps hold their meaning
		auto const next_round = last_round + round_interval;
		while (!stopped && std::chrono::steady_clock::now () < next_round)
		{
			condition.wait_for (lock, next_round - std::chrono::steady_clock::now (), [this] { return stopped; });
		}
		if (stopped)
		{
			break;
		}
		last_round = std::chrono::steady_clock::now ();

		stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::loop);

		// Drain pending elections together with their current pacing
		std::deque<round_item> items;
		for (auto & [root, entry] : elections)
		{
			if (entry.pending != nullptr)
			{
				items.push_back ({ root, std::move (entry.pending), entry.pacing });
				entry.pending = nullptr;
			}
		}
		pending_count = 0;

		// Sweep entries for dead elections, drained items keep theirs alive until the round completes
		std::erase_if (elections, [] (auto const & pair) {
			return pair.second.pending == nullptr && pair.second.election.expired ();
		});

		lock.unlock ();
		run_round (items);
		lock.lock ();
	}
}

void nano::vote_solicitor::run_round (std::deque<round_item> & items)
{
	auto const now = std::chrono::steady_clock::now ();

	// Pull fresh snapshots and decide the work due, elections with nothing due are skipped
	std::vector<round_item *> solicited;
	std::vector<nano::solicitation> solicitations;
	for (auto & item : items)
	{
		auto snapshot = item.election->snapshot ();
		auto const due = decide (snapshot, item.pacing, base_latency, broadcast_interval, now);
		if (!due.request && !due.broadcast)
		{
			continue;
		}
		solicited.push_back (&item);
		solicitations.push_back ({ snapshot.winner, std::move (snapshot.votes), snapshot.quorum, due.request, due.broadcast });
	}

	auto const results = solicit (solicitations);
	release_assert (results.size () == solicitations.size ());

	// Advance pacing only for work actually performed
	for (std::size_t i = 0; i < results.size (); ++i)
	{
		auto & item = *solicited[i];
		auto const & result = results[i];
		auto const & solicitation = solicitations[i];

		if (result.requested)
		{
			item.pacing.last_request = now;
			++item.election->confirmation_request_count;

			stats.inc (nano::stat::type::election, nano::stat::detail::confirmation_request);
			logger.debug (nano::log::type::vote_solicitor, "Requested confirmations for root: {} (voters: {}, confirmation requests: {})",
			item.root,
			solicitation.votes.size (),
			item.election->confirmation_request_count.load ());
		}
		if (result.broadcasted)
		{
			bool const initial = item.pacing.last_broadcast_hash.is_zero ();
			item.pacing.last_broadcast = now;
			item.pacing.last_broadcast_hash = solicitation.winner->hash ();

			stats.inc (nano::stat::type::election, initial ? nano::stat::detail::broadcast_block_initial : nano::stat::detail::broadcast_block_repeat);
			logger.debug (nano::log::type::vote_solicitor, "Broadcasted winner: {} for root: {}",
			item.pacing.last_broadcast_hash,
			item.root);
		}
	}

	// Store the advanced pacing back into the table
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		for (auto const & item : items)
		{
			if (auto existing = elections.find (item.root); existing != elections.end ())
			{
				existing->second.pacing = item.pacing;
			}
		}
	}
}

std::vector<nano::vote_solicitor::result> nano::vote_solicitor::solicit (std::vector<nano::solicitation> const & solicitations) const
{
	if (solicitations.empty ())
	{
		return {};
	}

	auto round_limits = limits;
	round_limits.max_election_broadcasts = std::max<std::size_t> (network.fanout () / 2, 1);

	vote_solicitor_round round{ rep_crawler.principal_representatives (), round_limits };
	round.channel_full = [] (std::shared_ptr<nano::transport::channel> const & channel) {
		return channel->max (nano::transport::traffic_type::confirmation_requests);
	};

	std::vector<result> results;
	results.reserve (solicitations.size ());
	for (auto const & solicitation : solicitations)
	{
		stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::process);
		results.push_back (round.add (solicitation));
	}

	flush (round);
	return results;
}

void nano::vote_solicitor::flush (nano::vote_solicitor_round const & round) const
{
	for (auto const & [block, channels] : round.broadcasts ())
	{
		nano::messages::publish message{ network_constants, block };
		for (auto const & channel : channels)
		{
			stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::broadcast, nano::stat::dir::out);
			channel->send (message, nano::transport::traffic_type::block_broadcast);
		}
		// Random flood for block propagation
		block_rebroadcaster.push (block);
	}

	for (auto const & [channel, roots_hashes] : round.requests ())
	{
		vote_solicitor_round::roots_hashes_t batch;
		for (auto const & root_hash : roots_hashes)
		{
			batch.push_back (root_hash);
			if (batch.size () == nano::network::confirm_req_hashes_max)
			{
				stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::request, nano::stat::dir::out);
				nano::messages::confirm_req message{ network_constants, batch };
				channel->send (message, nano::transport::traffic_type::confirmation_requests);
				batch.clear ();
			}
		}
		if (!batch.empty ())
		{
			stats.inc (nano::stat::type::vote_solicitor, nano::stat::detail::request, nano::stat::dir::out);
			nano::messages::confirm_req message{ network_constants, batch };
			channel->send (message, nano::transport::traffic_type::confirmation_requests);
		}
	}
}

std::size_t nano::vote_solicitor::size () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return elections.size ();
}

bool nano::vote_solicitor::empty () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return elections.empty ();
}

nano::container_info nano::vote_solicitor::container_info () const
{
	nano::lock_guard<nano::mutex> guard{ mutex };

	nano::container_info info;
	info.put ("elections", elections);
	info.put ("pending", pending_count);
	return info;
}
