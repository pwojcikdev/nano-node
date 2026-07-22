#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/utility.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/messages/publish.hpp>
#include <nano/node/block_rebroadcaster.hpp>
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

nano::vote_solicitor::vote_solicitor (nano::network & network_a, nano::rep_crawler & rep_crawler_a, nano::block_rebroadcaster & block_rebroadcaster_a, nano::network_constants const & network_constants_a, nano::stats & stats_a, nano::logger & logger_a) :
	limits{ .max_block_broadcasts = network_constants_a.is_dev_network () ? 4u : 30u },
	network{ network_a },
	rep_crawler{ rep_crawler_a },
	block_rebroadcaster{ block_rebroadcaster_a },
	network_constants{ network_constants_a },
	stats{ stats_a },
	logger{ logger_a }
{
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
