#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/stats.hpp>
#include <nano/messages/confirm.hpp>
#include <nano/messages/publish.hpp>
#include <nano/node/active_elections.hpp>
#include <nano/node/election.hpp>
#include <nano/node/nodeconfig.hpp>
#include <nano/node/repcrawler.hpp>
#include <nano/node/transport/test_channel.hpp>
#include <nano/node/vote_solicitor.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
// Winner blocks for the planner only need to be well-formed, they are never validated against a ledger
std::shared_ptr<nano::block> make_winner (uint64_t seed = 100)
{
	nano::block_builder builder;
	return builder
	.send ()
	.previous (nano::dev::genesis->hash ())
	.destination (nano::keypair ().pub)
	.balance (nano::dev::constants.genesis_amount - seed)
	.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
	.work (0)
	.build ();
}

nano::solicitation make_solicitation (std::shared_ptr<nano::block> const & winner, bool request = true, bool broadcast = false)
{
	return { winner, /* votes */ {}, /* quorum */ false, request, broadcast };
}

uint64_t constexpr final_timestamp = std::numeric_limits<uint64_t>::max ();
}

/*
 * A representative that has not voted yet should be requested and the request accumulated per channel
 */
TEST (vote_solicitor_round, request_basic)
{
	nano::vote_solicitor_round round{ { { nano::account{ 1 }, nullptr } }, {} };

	auto winner = make_winner ();
	auto result = round.add (make_solicitation (winner));
	ASSERT_TRUE (result.requested);
	ASSERT_FALSE (result.broadcasted);

	ASSERT_EQ (1, round.requests ().size ());
	auto const & queued = round.requests ().begin ()->second;
	ASSERT_EQ (1, queued.size ());
	ASSERT_EQ (winner->hash (), queued[0].first);
	ASSERT_EQ (winner->root (), queued[0].second);
	ASSERT_TRUE (round.broadcasts ().empty ());
}

/*
 * Before quorum any existing vote for the winner is conclusive, no request is needed
 */
TEST (vote_solicitor_round, skip_voted)
{
	nano::account rep{ 1 };
	nano::vote_solicitor_round round{ { { rep, nullptr } }, {} };

	auto winner = make_winner ();
	auto solicitation = make_solicitation (winner);
	solicitation.votes[rep] = { std::chrono::steady_clock::now (), /* timestamp */ 1, winner->hash () };

	auto result = round.add (solicitation);
	ASSERT_FALSE (result.requested);
	ASSERT_TRUE (round.requests ().empty ());
}

/*
 * After quorum only final votes are conclusive, non-final voters should be requested again
 */
TEST (vote_solicitor_round, final_upgrade)
{
	nano::account rep{ 1 };
	auto winner = make_winner ();

	// Non-final vote after quorum is requested again
	{
		nano::vote_solicitor_round round{ { { rep, nullptr } }, {} };
		auto solicitation = make_solicitation (winner);
		solicitation.quorum = true;
		solicitation.votes[rep] = { std::chrono::steady_clock::now (), /* timestamp */ 1, winner->hash () };
		ASSERT_TRUE (round.add (solicitation).requested);
	}
	// Final vote after quorum is conclusive
	{
		nano::vote_solicitor_round round{ { { rep, nullptr } }, {} };
		auto solicitation = make_solicitation (winner);
		solicitation.quorum = true;
		solicitation.votes[rep] = { std::chrono::steady_clock::now (), final_timestamp, winner->hash () };
		ASSERT_FALSE (round.add (solicitation).requested);
	}
}

/*
 * Votes for a different hash should be re-requested without counting towards the per election cap
 */
TEST (vote_solicitor_round, fork_bypasses_cap)
{
	std::size_t const max_requests = 3;

	std::vector<nano::representative> reps;
	for (uint64_t i = 1; i <= max_requests + 1; ++i)
	{
		reps.push_back ({ nano::account{ i }, nullptr });
	}

	auto winner = make_winner ();

	// All reps voted for a different hash, the cap does not apply
	{
		nano::vote_solicitor_round round{ reps, { .max_election_requests = max_requests } };
		auto solicitation = make_solicitation (winner);
		for (auto const & rep : reps)
		{
			solicitation.votes[rep.account] = { std::chrono::steady_clock::now (), /* timestamp */ 1, nano::block_hash{ 999 } };
		}
		ASSERT_TRUE (round.add (solicitation).requested);
		ASSERT_EQ (max_requests + 1, round.requests ().begin ()->second.size ());
	}
	// No votes at all, the cap applies
	{
		nano::vote_solicitor_round round{ reps, { .max_election_requests = max_requests } };
		ASSERT_TRUE (round.add (make_solicitation (winner)).requested);
		ASSERT_EQ (max_requests, round.requests ().begin ()->second.size ());
	}
}

/*
 * A full channel should be dropped from request planning for the rest of the round
 */
TEST (vote_solicitor_round, channel_full)
{
	nano::test::system system;
	auto & node = *system.add_node ();

	auto channel_full = nano::test::fake_channel (node, nano::account{ 1 });
	auto channel_ok = nano::test::fake_channel (node, nano::account{ 2 });

	std::vector<nano::representative> reps{ { nano::account{ 1 }, channel_full }, { nano::account{ 2 }, channel_ok } };
	nano::vote_solicitor_round round{ reps, {} };
	round.channel_full = [&] (auto const & channel) {
		return channel == channel_full;
	};

	auto result = round.add (make_solicitation (make_winner ()));
	ASSERT_TRUE (result.requested);
	ASSERT_EQ (1, round.requests ().size ());
	ASSERT_TRUE (round.requests ().contains (channel_ok));
	ASSERT_FALSE (round.requests ().contains (channel_full));

	// The full channel stays dropped for subsequent solicitations
	round.add (make_solicitation (make_winner (200)));
	ASSERT_FALSE (round.requests ().contains (channel_full));
}

/*
 * Winner broadcasts are globally capped per round, non-voted reps are targeted
 */
TEST (vote_solicitor_round, broadcast_caps)
{
	std::size_t const max_broadcasts = 2;
	nano::vote_solicitor_round round{ { { nano::account{ 1 }, nullptr } }, { .max_block_broadcasts = max_broadcasts } };

	auto first = round.add (make_solicitation (make_winner (100), /* request */ false, /* broadcast */ true));
	ASSERT_TRUE (first.broadcasted);
	auto second = round.add (make_solicitation (make_winner (200), /* request */ false, /* broadcast */ true));
	ASSERT_TRUE (second.broadcasted);
	auto third = round.add (make_solicitation (make_winner (300), /* request */ false, /* broadcast */ true));
	ASSERT_FALSE (third.broadcasted);

	ASSERT_EQ (max_broadcasts, round.broadcasts ().size ());
	ASSERT_EQ (1, round.broadcasts ()[0].second.size ()); // Non-voted rep is targeted
}

/*
 * Reps that voted for the winner are not broadcast targets, forks are targeted without counting towards the cap
 */
TEST (vote_solicitor_round, broadcast_targets)
{
	nano::account rep{ 1 };
	auto winner = make_winner ();

	// Voted for the winner, not targeted but the broadcast still counts as performed
	{
		nano::vote_solicitor_round round{ { { rep, nullptr } }, {} };
		auto solicitation = make_solicitation (winner, false, true);
		solicitation.votes[rep] = { std::chrono::steady_clock::now (), /* timestamp */ 1, winner->hash () };
		ASSERT_TRUE (round.add (solicitation).broadcasted);
		ASSERT_EQ (1, round.broadcasts ().size ());
		ASSERT_TRUE (round.broadcasts ()[0].second.empty ());
	}
	// Voted for a different hash, targeted
	{
		nano::vote_solicitor_round round{ { { rep, nullptr } }, {} };
		auto solicitation = make_solicitation (winner, false, true);
		solicitation.votes[rep] = { std::chrono::steady_clock::now (), /* timestamp */ 1, nano::block_hash{ 999 } };
		ASSERT_TRUE (round.add (solicitation).broadcasted);
		ASSERT_EQ (1, round.broadcasts ()[0].second.size ());
	}
}

/*
 * Component end-to-end: solicitations are planned and sent as confirm_req and publish messages
 */
TEST (vote_solicitor, solicit)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	flags.disable_request_loop = true;
	auto & node = *system.add_node (flags);

	auto channel = nano::test::test_channel (node);
	node.rep_crawler.force_add_rep (nano::dev::genesis_key.pub, channel);

	std::atomic<size_t> confirm_reqs{ 0 };
	std::atomic<size_t> confirm_req_hashes{ 0 };
	std::atomic<size_t> publishes{ 0 };
	channel->observe<nano::messages::confirm_req> ([&] (nano::messages::confirm_req const & message) {
		confirm_reqs += 1;
		confirm_req_hashes += message.roots_hashes.size ();
	});
	channel->observe<nano::messages::publish> ([&] (nano::messages::publish const & message) {
		publishes += 1;
	});

	// One plain solicitation and one with the rep voting for a different hash
	auto solicitation1 = make_solicitation (make_winner (100), /* request */ true, /* broadcast */ true);
	auto solicitation2 = make_solicitation (make_winner (200), /* request */ true, /* broadcast */ false);
	solicitation2.votes[nano::dev::genesis_key.pub] = { std::chrono::steady_clock::now (), /* timestamp */ 1, nano::block_hash{ 999 } };

	auto results = node.vote_solicitor.solicit ({ solicitation1, solicitation2 });
	ASSERT_EQ (2, results.size ());
	ASSERT_TRUE (results[0].requested);
	ASSERT_TRUE (results[0].broadcasted);
	ASSERT_TRUE (results[1].requested);
	ASSERT_FALSE (results[1].broadcasted);

	// Both requests are batched into a single confirm_req to the same channel
	ASSERT_EQ (1, confirm_reqs);
	ASSERT_EQ (2, confirm_req_hashes);
	ASSERT_EQ (1, publishes);
}

/*
 * Election tick should request solicitation when active, try_solicit should snapshot and advance pacing
 */
TEST (vote_solicitor, election_tick)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_request_loop = true;
	auto & node = *system.add_node (flags);

	nano::block_builder builder;
	auto send = builder
				.send ()
				.previous (nano::dev::genesis->hash ())
				.destination (nano::keypair ().pub)
				.balance (nano::dev::constants.genesis_amount - 100)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (*system.work.generate (nano::dev::genesis->hash ()))
				.build ();
	ASSERT_EQ (nano::block_status::progress, node.process (send));

	auto election = nano::test::start_election (system, node, send->hash ());
	ASSERT_NE (nullptr, election);
	election->transition_active ();

	auto result = election->tick ();
	ASSERT_FALSE (result.finished);
	ASSERT_TRUE (result.solicit);

	auto solicitation = election->try_solicit ();
	ASSERT_TRUE (solicitation.has_value ());
	ASSERT_TRUE (solicitation->request);
	ASSERT_TRUE (solicitation->broadcast);
	ASSERT_EQ (send->hash (), solicitation->winner->hash ());

	// The snapshot alone does not advance pacing, only performed work reported back does
	ASSERT_EQ (0, election->confirmation_request_count);

	election->solicited (/* requested */ true, send->hash ());
	ASSERT_EQ (1, election->confirmation_request_count);
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::confirmation_request));
	ASSERT_EQ (1, node.stats.count (nano::stat::type::election, nano::stat::detail::broadcast_block_initial));
}

/*
 * Passive elections should not produce solicitations
 */
TEST (vote_solicitor, election_passive)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_request_loop = true;
	auto & node = *system.add_node (flags);

	nano::block_builder builder;
	auto send = builder
				.send ()
				.previous (nano::dev::genesis->hash ())
				.destination (nano::keypair ().pub)
				.balance (nano::dev::constants.genesis_amount - 100)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (*system.work.generate (nano::dev::genesis->hash ()))
				.build ();
	send->sideband_set ({});

	auto election = std::make_shared<nano::election> (node, send, nano::election_behavior::priority);
	ASSERT_EQ (nano::election_state::passive, election->state ());

	ASSERT_FALSE (election->try_solicit ().has_value ());
	ASSERT_EQ (0, election->confirmation_request_count);
}

/*
 * Full asynchronous path: the request loop ticks the election, triggers the solicitor and messages reach the representative
 */
TEST (vote_solicitor, async_round)
{
	nano::test::system system;
	nano::node_flags flags;
	flags.disable_rep_crawler = true;
	auto & node = *system.add_node (flags);

	auto channel = nano::test::test_channel (node);
	node.rep_crawler.force_add_rep (nano::dev::genesis_key.pub, channel);

	std::atomic<size_t> confirm_reqs{ 0 };
	std::atomic<size_t> publishes{ 0 };
	channel->observe<nano::messages::confirm_req> ([&] (nano::messages::confirm_req const & message) {
		confirm_reqs += 1;
	});
	channel->observe<nano::messages::publish> ([&] (nano::messages::publish const & message) {
		publishes += 1;
	});

	nano::block_builder builder;
	auto send = builder
				.send ()
				.previous (nano::dev::genesis->hash ())
				.destination (nano::keypair ().pub)
				.balance (nano::dev::constants.genesis_amount - 100)
				.sign (nano::dev::genesis_key.prv, nano::dev::genesis_key.pub)
				.work (*system.work.generate (nano::dev::genesis->hash ()))
				.build ();
	ASSERT_EQ (nano::block_status::progress, node.process (send));

	auto election = nano::test::start_election (system, node, send->hash ());
	ASSERT_NE (nullptr, election);

	// The request loop activates the election, triggers the solicitor and the round queries the representative
	ASSERT_TIMELY (5s, confirm_reqs >= 1);
	ASSERT_TIMELY (5s, publishes >= 1);
	ASSERT_TIMELY (5s, election->confirmation_request_count >= 1);
}
