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
 * Fresh pacing on an active election should make both requests and broadcasts due
 */
TEST (vote_solicitor, decide_active)
{
	auto winner = make_winner ();
	nano::election_snapshot snapshot{ winner, {}, false, nano::election_state::active, nano::election_behavior::priority, std::chrono::steady_clock::now () };

	auto const now = std::chrono::steady_clock::now ();
	auto due = nano::vote_solicitor::decide (snapshot, {}, 100ms, 500ms, now);
	ASSERT_TRUE (due.request);
	ASSERT_TRUE (due.broadcast);
}

/*
 * Recently performed work should keep the gates closed until the intervals elapse
 */
TEST (vote_solicitor, decide_paced)
{
	auto winner = make_winner ();
	nano::election_snapshot snapshot{ winner, {}, false, nano::election_state::active, nano::election_behavior::priority, std::chrono::steady_clock::now () };

	auto const now = std::chrono::steady_clock::now ();
	nano::vote_solicitor::pacing pacing{ .last_request = now, .last_broadcast = now, .last_broadcast_hash = winner->hash () };

	auto due = nano::vote_solicitor::decide (snapshot, pacing, 100ms, 500ms, now);
	ASSERT_FALSE (due.request);
	ASSERT_FALSE (due.broadcast);

	// Once the intervals elapse both gates open again
	auto later = nano::vote_solicitor::decide (snapshot, pacing, 100ms, 500ms, now + 1s);
	ASSERT_TRUE (later.request);
	ASSERT_TRUE (later.broadcast);
}

/*
 * A changed winner should make the broadcast due regardless of the broadcast interval
 */
TEST (vote_solicitor, decide_winner_changed)
{
	auto winner = make_winner ();
	nano::election_snapshot snapshot{ winner, {}, false, nano::election_state::active, nano::election_behavior::priority, std::chrono::steady_clock::now () };

	auto const now = std::chrono::steady_clock::now ();
	nano::vote_solicitor::pacing pacing{ .last_request = now, .last_broadcast = now, .last_broadcast_hash = nano::block_hash{ 999 } };

	auto due = nano::vote_solicitor::decide (snapshot, pacing, 100ms, 500ms, now);
	ASSERT_TRUE (due.broadcast);
}

/*
 * Election state controls what work is allowed: nothing while passive or expired, broadcasts only once confirmed
 */
TEST (vote_solicitor, decide_states)
{
	auto winner = make_winner ();
	auto const now = std::chrono::steady_clock::now ();

	auto decide_for = [&] (nano::election_state state) {
		nano::election_snapshot snapshot{ winner, {}, false, state, nano::election_behavior::priority, now };
		return nano::vote_solicitor::decide (snapshot, {}, 100ms, 500ms, now);
	};

	for (auto state : { nano::election_state::passive, nano::election_state::expired_unconfirmed, nano::election_state::cancelled })
	{
		auto due = decide_for (state);
		ASSERT_FALSE (due.request);
		ASSERT_FALSE (due.broadcast);
	}
	for (auto state : { nano::election_state::confirmed, nano::election_state::expired_confirmed })
	{
		auto due = decide_for (state);
		ASSERT_FALSE (due.request);
		ASSERT_TRUE (due.broadcast);
	}
}

/*
 * The request interval scales with election behavior, optimistic elections are solicited less aggressively
 */
TEST (vote_solicitor, decide_behavior)
{
	auto winner = make_winner ();
	auto const now = std::chrono::steady_clock::now ();

	// Elapsed time between the optimistic (2x) and priority (5x) intervals
	nano::vote_solicitor::pacing pacing{ .last_request = now - 300ms, .last_broadcast = now, .last_broadcast_hash = winner->hash () };

	nano::election_snapshot priority{ winner, {}, false, nano::election_state::active, nano::election_behavior::priority, now };
	ASSERT_FALSE (nano::vote_solicitor::decide (priority, pacing, 100ms, 500ms, now).request);

	nano::election_snapshot optimistic{ winner, {}, false, nano::election_state::active, nano::election_behavior::optimistic, now };
	ASSERT_TRUE (nano::vote_solicitor::decide (optimistic, pacing, 100ms, 500ms, now).request);
}

/*
 * Election snapshot should capture the voting state under a single lock
 */
TEST (vote_solicitor, election_snapshot)
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

	auto snapshot = election->snapshot ();
	ASSERT_EQ (send->hash (), snapshot.winner->hash ());
	ASSERT_EQ (nano::election_state::passive, snapshot.state);
	ASSERT_EQ (nano::election_behavior::priority, snapshot.behavior);
	// A fresh election only contains the null account tally placeholder
	ASSERT_EQ (1, snapshot.votes.size ());
	ASSERT_TRUE (snapshot.votes.contains (nano::account::null ()));

	election->set_last_vote (nano::dev::genesis_key.pub, { std::chrono::steady_clock::now (), 1, send->hash () });
	election->transition_active ();

	auto snapshot2 = election->snapshot ();
	ASSERT_EQ (nano::election_state::active, snapshot2.state);
	ASSERT_EQ (2, snapshot2.votes.size ());
	ASSERT_TRUE (snapshot2.votes.contains (nano::dev::genesis_key.pub));
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
