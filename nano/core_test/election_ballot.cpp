#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/node/election_ballot.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <unordered_map>

using namespace std::chrono_literals;

namespace
{
uint64_t constexpr final_timestamp = std::numeric_limits<uint64_t>::max ();

// Start well past the clock epoch so cooldown arithmetic behaves like on a running system
std::chrono::steady_clock::time_point start_time ()
{
	return std::chrono::steady_clock::time_point{} + 1h;
}

std::shared_ptr<nano::block> make_block (uint64_t seed)
{
	nano::keypair key;
	nano::block_builder builder;
	return builder
	.send ()
	.previous (seed)
	.destination (seed + 1000)
	.balance (1000000 - seed)
	.sign (key.prv, key.pub)
	.work (seed + 2000)
	.build ();
}

nano::account make_account (uint64_t seed)
{
	return nano::account{ seed };
}

/**
 * Backs the ballot with an explicit account -> weight map, so tests can control the tally exactly.
 * Accounts absent from the map weigh nothing.
 */
struct test_context
{
	std::unordered_map<nano::account, nano::uint128_t> weights;
	std::shared_ptr<nano::block> initial;
	nano::election_ballot ballot;

	explicit test_context (std::shared_ptr<nano::block> initial_a) :
		initial{ std::move (initial_a) },
		ballot{ initial, [this] (nano::account const & account) { return weights.contains (account) ? weights[account] : 0; }, start_time () }
	{
	}

	test_context () :
		test_context{ make_block (1) }
	{
	}

	// Register a rep with the given weight and immediately record its vote
	void vote (uint64_t account_seed, nano::uint128_t weight, nano::block_hash const & hash, uint64_t timestamp = 1)
	{
		auto account = make_account (account_seed);
		weights[account] = weight;
		ASSERT_EQ (nano::election_ballot::vote_result::accepted, ballot.insert_vote (account, timestamp, hash, start_time (), 0s));
	}
};
}

TEST (election_ballot, construction)
{
	test_context ctx;
	ASSERT_EQ (1, ctx.ballot.block_count ());
	ASSERT_EQ (1, ctx.ballot.voter_count ()); // The sentinel entry for the initial block
	ASSERT_TRUE (ctx.ballot.contains (ctx.initial->hash ()));
	ASSERT_EQ (ctx.initial, ctx.ballot.find (ctx.initial->hash ()));
	ASSERT_EQ (nullptr, ctx.ballot.find (nano::block_hash{ 42 }));
	ASSERT_FALSE (ctx.ballot.full ());
	ASSERT_TRUE (ctx.ballot.votes_with_weight ().empty ()); // Sentinel entry is not reported
}

/*
 * Vote admission
 */

TEST (election_ballot, vote_replay_older_timestamp)
{
	test_context ctx;
	auto rep = make_account (1);
	auto hash = ctx.initial->hash ();

	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 5, hash, start_time (), 0s));
	ASSERT_EQ (nano::election_ballot::vote_result::replay, ctx.ballot.insert_vote (rep, 4, hash, start_time (), 0s));
	ASSERT_EQ (5, ctx.ballot.get_vote (rep).timestamp);
}

TEST (election_ballot, vote_equal_timestamp_tie_broken_by_hash)
{
	test_context ctx;
	auto rep = make_account (1);

	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 5, nano::block_hash{ 10 }, start_time (), 0s));
	// Same timestamp, higher hash wins the tie
	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 5, nano::block_hash{ 11 }, start_time (), 0s));
	ASSERT_EQ (nano::block_hash{ 11 }, ctx.ballot.get_vote (rep).hash);
	// Same timestamp, lower or equal hash is a replay
	ASSERT_EQ (nano::election_ballot::vote_result::replay, ctx.ballot.insert_vote (rep, 5, nano::block_hash{ 10 }, start_time (), 0s));
	ASSERT_EQ (nano::election_ballot::vote_result::replay, ctx.ballot.insert_vote (rep, 5, nano::block_hash{ 11 }, start_time (), 0s));
}

TEST (election_ballot, vote_cooldown)
{
	test_context ctx;
	auto rep = make_account (1);
	auto hash = ctx.initial->hash ();
	auto now = start_time ();

	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 1, hash, now, 5s));
	ASSERT_EQ (nano::election_ballot::vote_result::ignored, ctx.ballot.insert_vote (rep, 2, hash, now + 1s, 5s));
	ASSERT_EQ (nano::election_ballot::vote_result::ignored, ctx.ballot.insert_vote (rep, 2, hash, now + 4s, 5s));
	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 2, hash, now + 5s, 5s));
}

TEST (election_ballot, vote_cooldown_disabled)
{
	test_context ctx;
	auto rep = make_account (1);
	auto hash = ctx.initial->hash ();
	auto now = start_time ();

	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 1, hash, now, 0s));
	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 2, hash, now, 0s));
	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 3, hash, now, 0s));
}

TEST (election_ballot, vote_final_bypasses_cooldown)
{
	test_context ctx;
	auto rep = make_account (1);
	auto hash = ctx.initial->hash ();
	auto now = start_time ();

	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, 1, hash, now, 15s));
	ASSERT_EQ (nano::election_ballot::vote_result::ignored, ctx.ballot.insert_vote (rep, 2, hash, now, 15s));
	ASSERT_EQ (nano::election_ballot::vote_result::accepted, ctx.ballot.insert_vote (rep, final_timestamp, hash, now, 15s));
	// A second final vote is a replay, not a cooldown bypass
	ASSERT_EQ (nano::election_ballot::vote_result::replay, ctx.ballot.insert_vote (rep, final_timestamp, hash, now, 15s));
}

TEST (election_ballot, vote_cooldown_by_weight)
{
	nano::uint128_t const online_stake = 1000;
	ASSERT_EQ (1s, nano::vote_cooldown (online_stake / 20 + 1, online_stake)); // Above 5%
	ASSERT_EQ (5s, nano::vote_cooldown (online_stake / 20, online_stake)); // Exactly 5%
	ASSERT_EQ (5s, nano::vote_cooldown (online_stake / 100 + 1, online_stake)); // Above 1%
	ASSERT_EQ (15s, nano::vote_cooldown (online_stake / 100, online_stake)); // Exactly 1%
	ASSERT_EQ (15s, nano::vote_cooldown (0, online_stake));
}

TEST (election_ballot, erase_votes)
{
	test_context ctx;
	auto hash = ctx.initial->hash ();
	ctx.vote (1, 100, hash);
	ctx.vote (2, 100, hash);
	ctx.vote (3, 100, hash);
	ASSERT_EQ (4, ctx.ballot.voter_count ());

	ctx.ballot.erase_votes ({ make_account (1), make_account (3), make_account (99) });
	ASSERT_EQ (2, ctx.ballot.voter_count ());
	ASSERT_EQ (0, ctx.ballot.get_vote (make_account (1)).timestamp);
	ASSERT_EQ (1, ctx.ballot.get_vote (make_account (2)).timestamp);
}

/*
 * Tally
 */

TEST (election_ballot, tally_orders_by_weight)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 100, ctx.initial->hash ());
	ctx.vote (2, 300, fork->hash ());

	auto tally = ctx.ballot.tally ();
	ASSERT_EQ (2, tally.size ());
	ASSERT_EQ (300, tally.begin ()->first);
	ASSERT_EQ (fork->hash (), tally.begin ()->second->hash ());
}

TEST (election_ballot, tally_excludes_unknown_blocks)
{
	test_context ctx;
	ctx.vote (1, 100, ctx.initial->hash ());
	ctx.vote (2, 300, nano::block_hash{ 999 }); // Vote for a block this election does not know about

	auto tally = ctx.ballot.tally ();
	ASSERT_EQ (1, tally.size ());
	ASSERT_EQ (100, tally.begin ()->first);

	// The unknown block's weight is still part of the total
	auto result = ctx.ballot.evaluate (1);
	ASSERT_EQ (100, result.total_weight);
}

TEST (election_ballot, evaluate_quorum_threshold)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 300, ctx.initial->hash ());
	ctx.vote (2, 100, fork->hash ());

	// Winner leads the runner-up by exactly 200
	auto exact = ctx.ballot.evaluate (200);
	ASSERT_EQ (ctx.initial->hash (), exact.winner->hash ());
	ASSERT_EQ (300, exact.winner_weight);
	ASSERT_EQ (400, exact.total_weight);
	ASSERT_TRUE (exact.quorum);

	ASSERT_FALSE (ctx.ballot.evaluate (201).quorum);
	ASSERT_TRUE (ctx.ballot.evaluate (199).quorum);
}

TEST (election_ballot, evaluate_quorum_no_runner_up)
{
	test_context ctx;
	ctx.vote (1, 300, ctx.initial->hash ());

	auto result = ctx.ballot.evaluate (300);
	ASSERT_EQ (ctx.initial->hash (), result.winner->hash ());
	ASSERT_TRUE (result.quorum);
	ASSERT_FALSE (ctx.ballot.evaluate (301).quorum);
}

TEST (election_ballot, evaluate_final_weight)
{
	test_context ctx;
	auto hash = ctx.initial->hash ();
	ctx.vote (1, 300, hash, final_timestamp);
	ctx.vote (2, 100, hash, 1); // Non-final vote does not count toward final weight

	auto result = ctx.ballot.evaluate (300);
	ASSERT_EQ (400, result.winner_weight);
	ASSERT_EQ (300, result.final_weight);
	ASSERT_TRUE (result.quorum);
	ASSERT_TRUE (result.final_quorum);

	// Quorum can be reached without final quorum
	auto partial = ctx.ballot.evaluate (350);
	ASSERT_TRUE (partial.quorum);
	ASSERT_FALSE (partial.final_quorum);
}

TEST (election_ballot, evaluate_final_weight_follows_winner)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 100, ctx.initial->hash (), final_timestamp);
	ctx.vote (2, 300, fork->hash (), 1);

	// The winner is the fork, which has no final votes behind it
	auto result = ctx.ballot.evaluate (1);
	ASSERT_EQ (fork->hash (), result.winner->hash ());
	ASSERT_EQ (0, result.final_weight);
	ASSERT_FALSE (result.final_quorum);
}

TEST (election_ballot, evaluate_winner_flip)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 100, ctx.initial->hash ());
	ASSERT_EQ (ctx.initial->hash (), ctx.ballot.evaluate (1).winner->hash ());

	ctx.vote (2, 300, fork->hash ());
	ASSERT_EQ (fork->hash (), ctx.ballot.evaluate (1).winner->hash ());
}

/*
 * Blocks
 */

TEST (election_ballot, insert_block)
{
	test_context ctx;
	auto fork = make_block (2);

	ASSERT_TRUE (ctx.ballot.insert_block (fork));
	ASSERT_EQ (2, ctx.ballot.block_count ());
	ASSERT_TRUE (ctx.ballot.contains (fork->hash ()));

	// Reinserting the same hash refreshes the stored pointer instead of adding
	auto duplicate = make_block (2);
	ASSERT_EQ (fork->hash (), duplicate->hash ());
	ASSERT_FALSE (ctx.ballot.insert_block (duplicate));
	ASSERT_EQ (2, ctx.ballot.block_count ());
	ASSERT_EQ (duplicate, ctx.ballot.find (fork->hash ()));
}

TEST (election_ballot, full)
{
	test_context ctx;
	for (uint64_t i = 2; i <= 10; ++i)
	{
		ASSERT_FALSE (ctx.ballot.full ());
		ASSERT_TRUE (ctx.ballot.insert_block (make_block (i)));
	}
	ASSERT_EQ (10, ctx.ballot.block_count ());
	ASSERT_TRUE (ctx.ballot.full ());
}

TEST (election_ballot, erase_block)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 100, ctx.initial->hash ());
	ctx.vote (2, 300, fork->hash ());
	ASSERT_EQ (3, ctx.ballot.voter_count ());

	auto erased = ctx.ballot.erase_block (fork->hash (), ctx.initial->hash ());
	ASSERT_EQ (fork, erased);
	ASSERT_EQ (1, ctx.ballot.block_count ());
	ASSERT_FALSE (ctx.ballot.contains (fork->hash ()));
	ASSERT_EQ (2, ctx.ballot.voter_count ()); // The vote for the erased block is gone
}

TEST (election_ballot, erase_block_refuses_winner_and_unknown)
{
	test_context ctx;
	ASSERT_EQ (nullptr, ctx.ballot.erase_block (ctx.initial->hash (), ctx.initial->hash ()));
	ASSERT_EQ (1, ctx.ballot.block_count ());
	ASSERT_EQ (nullptr, ctx.ballot.erase_block (nano::block_hash{ 999 }, ctx.initial->hash ()));
	ASSERT_EQ (1, ctx.ballot.block_count ());
}

/*
 * Block replacement
 */

TEST (election_ballot, replacement_requires_inactive_tally)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));
	ctx.vote (1, 100, ctx.initial->hash ());
	ctx.ballot.evaluate (1);

	ASSERT_FALSE (ctx.ballot.replacement_candidate (0, ctx.initial->hash ()).has_value ());
}

TEST (election_ballot, replacement_picks_untallied_block)
{
	test_context ctx;
	auto fork = make_block (2);
	ASSERT_TRUE (ctx.ballot.insert_block (fork));

	ctx.vote (1, 100, ctx.initial->hash ());
	ctx.ballot.evaluate (1); // Only the initial block is tallied, the fork has no votes

	auto candidate = ctx.ballot.replacement_candidate (1, ctx.initial->hash ());
	ASSERT_TRUE (candidate.has_value ());
	ASSERT_EQ (fork->hash (), *candidate);
}

TEST (election_ballot, replacement_picks_lowest_tally_when_full)
{
	test_context ctx;
	// Fill the ballot and give every block a distinct tally so no block is untallied
	std::vector<std::shared_ptr<nano::block>> forks{ ctx.initial };
	for (uint64_t i = 2; i <= 10; ++i)
	{
		auto fork = make_block (i);
		ASSERT_TRUE (ctx.ballot.insert_block (fork));
		forks.push_back (fork);
	}
	for (size_t i = 0; i < forks.size (); ++i)
	{
		ctx.vote (i + 1, 100 * (i + 1), forks[i]->hash ());
	}
	ctx.ballot.evaluate (1);

	auto winner = forks.back ()->hash (); // Highest tally
	// The lowest tally is the initial block at 100, which is not the winner
	auto candidate = ctx.ballot.replacement_candidate (150, winner);
	ASSERT_TRUE (candidate.has_value ());
	ASSERT_EQ (ctx.initial->hash (), *candidate);

	// New block does not outweigh the lowest tally
	ASSERT_FALSE (ctx.ballot.replacement_candidate (100, winner).has_value ());
}

TEST (election_ballot, replacement_avoids_winner)
{
	test_context ctx;
	std::vector<std::shared_ptr<nano::block>> forks{ ctx.initial };
	for (uint64_t i = 2; i <= 10; ++i)
	{
		auto fork = make_block (i);
		ASSERT_TRUE (ctx.ballot.insert_block (fork));
		forks.push_back (fork);
	}
	for (size_t i = 0; i < forks.size (); ++i)
	{
		ctx.vote (i + 1, 100 * (i + 1), forks[i]->hash ());
	}
	ctx.ballot.evaluate (1);

	// Pretend the lowest tallied block (100) is the winner, the second lowest (200) must be picked instead
	auto winner = ctx.initial->hash ();
	auto candidate = ctx.ballot.replacement_candidate (250, winner);
	ASSERT_TRUE (candidate.has_value ());
	ASSERT_EQ (forks[1]->hash (), *candidate);

	// Outweighs the winner but not the second lowest, nothing is replaced
	ASSERT_FALSE (ctx.ballot.replacement_candidate (150, winner).has_value ());
}
