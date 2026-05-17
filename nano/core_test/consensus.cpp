#include <nano/consensus/consensus.hpp>
#include <nano/lib/blockbuilders.hpp>
#include <nano/lib/blocks.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/numbers.hpp>

#include <gtest/gtest.h>

#include <unordered_map>

// Pure unit tests for the ORV consensus core. No node/system: ledger weight and delta are
// injected via fake ports, so every test is a deterministic function of the vote stream. These
// tests pin the parity rules that the experimental sketch got wrong (margin quorum, live weight,
// sticky winner-specific final weight, sum-gated winner flip).

namespace
{
std::shared_ptr<nano::block> make_block (uint64_t balance)
{
	static nano::keypair key{};
	nano::state_block_builder builder;
	return builder.make_block ()
	.account (key.pub)
	.previous (0)
	.representative (key.pub)
	.balance (balance) // distinct balance => distinct hash
	.link (0)
	.sign (key.prv, key.pub)
	.work (0)
	.build ();
}

struct fixture
{
	std::unordered_map<nano::account, nano::uint128_t> weights;
	nano::uint128_t delta{ 100 };

	nano::consensus::ports ports ()
	{
		return nano::consensus::ports{
			[this] (nano::account const & account) -> nano::uint128_t {
				auto it = weights.find (account);
				return it == weights.end () ? nano::uint128_t{ 0 } : it->second;
			},
			[this] () -> nano::uint128_t { return delta; }
		};
	}
};
}

TEST (consensus, construction_seeds_initial_block)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	ASSERT_EQ (1, election.voter_count ()); // seeded null-account vote
	ASSERT_EQ (1, election.block_count ());
	ASSERT_EQ (b1, election.status_winner ());
	ASSERT_TRUE (election.contains_block (b1->hash ()));
	ASSERT_EQ (nano::consensus::election_state::passive, election.state ());
	ASSERT_FALSE (election.confirmed ());
	ASSERT_FALSE (election.has_quorum ()); // margin 0 - 0 < delta
}

TEST (consensus, margin_quorum_single_block)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;

	nano::consensus::effects e;
	election.vote (rep1.pub, b1->hash (), 1, e);

	ASSERT_TRUE (election.has_quorum ()); // (delta - 0) >= delta
	ASSERT_TRUE (e.on_quorum_reached.has_value ());
	ASSERT_EQ (b1->hash (), e.on_quorum_reached->winner);
	ASSERT_FALSE (e.on_final_quorum_reached.has_value ()); // no final votes
}

// THE discriminating test: the sketch used an absolute threshold (winner >= delta) and would
// confirm here; canonical nano requires the MARGIN over the runner-up to reach delta.
TEST (consensus, margin_is_not_absolute_with_fork)
{
	fixture f;
	f.delta = 100;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 };
	election.publish (b2);

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 150;
	f.weights[rep2.pub] = 60;

	nano::consensus::effects e1;
	election.vote (rep1.pub, b1->hash (), 1, e1);
	nano::consensus::effects e2;
	election.vote (rep2.pub, b2->hash (), 1, e2);

	// winner b1=150, runner-up b2=60, margin 90 < 100 -> NO quorum (absolute sketch would say yes)
	ASSERT_FALSE (election.has_quorum ());

	f.weights[rep1.pub] = 170; // margin 170 - 60 = 110 >= 100
	ASSERT_TRUE (election.has_quorum ());
}

TEST (consensus, live_weight_is_recomputed_not_frozen)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;
	nano::consensus::effects e;
	election.vote (rep1.pub, b1->hash (), 1, e);
	ASSERT_TRUE (election.has_quorum ());

	f.weights[rep1.pub] = f.delta - 1; // weight dropped after the vote was cast
	ASSERT_FALSE (election.has_quorum ()); // recomputed from live weight, not the cast-time snapshot
}

TEST (consensus, sticky_winner_specific_final_weight)
{
	fixture f;
	f.delta = 100;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 };
	election.publish (b2);

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 50;
	f.weights[rep2.pub] = 200;

	// rep1 casts a FINAL vote for b2 (the non-winner once rep2 votes)
	nano::consensus::effects e1;
	election.vote (rep1.pub, b2->hash (), std::numeric_limits<uint64_t>::max (), e1);
	ASSERT_EQ (50, election.final_tally ()); // b2 is the tally leader here and has finals

	// rep2 normal-votes b1, making b1 the winner. b1 has no final votes, so the winner-specific
	// final weight must stay sticky at 50 (NOT reset to 0).
	nano::consensus::effects e2;
	election.vote (rep2.pub, b1->hash (), 1, e2);
	auto tally = election.tally ();
	ASSERT_EQ (b1->hash (), tally.begin ()->second->hash ()); // b1 (200) is the winner
	ASSERT_EQ (50, election.final_tally ()); // sticky: unchanged despite winner having no finals
}

TEST (consensus, sum_gated_winner_flip)
{
	fixture f;
	f.delta = 100;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 }; // status winner = b1
	election.publish (b2);

	nano::keypair rep1, rep2;

	// Leader is b2 but total cast weight < delta -> NO flip yet (sum gate).
	f.weights[rep1.pub] = 60;
	nano::consensus::effects e1;
	election.vote (rep1.pub, b2->hash (), 1, e1);
	ASSERT_FALSE (e1.on_winner_changed.has_value ());
	ASSERT_EQ (b1, election.status_winner ());

	// Now sum >= delta and leader (b2) != status winner (b1) -> flip.
	f.weights[rep2.pub] = 80;
	nano::consensus::effects e2;
	election.vote (rep2.pub, b2->hash (), 1, e2);
	ASSERT_TRUE (e2.on_winner_changed.has_value ());
	ASSERT_EQ (b1->hash (), e2.on_winner_changed->previous_winner);
	ASSERT_EQ (b2->hash (), e2.on_winner_changed->new_winner->hash ());
	ASSERT_EQ (b2->hash (), election.status_winner ()->hash ());
	ASSERT_TRUE (e2.on_quorum_reached.has_value ()); // margin 140 - 0 >= 100
	ASSERT_EQ (b2->hash (), e2.on_quorum_reached->winner);
}

TEST (consensus, final_quorum_confirms_and_freezes)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;

	nano::consensus::effects e;
	election.vote (rep1.pub, b1->hash (), std::numeric_limits<uint64_t>::max (), e);

	ASSERT_TRUE (e.on_final_quorum_reached.has_value ());
	ASSERT_EQ (b1->hash (), e.on_final_quorum_reached->winner->hash ());
	ASSERT_EQ (nano::consensus::election_state::confirmed, election.state ());
	ASSERT_TRUE (election.confirmed ());

	// Once confirmed, further votes are not evaluated (matches `if (!confirmed_locked())`).
	nano::keypair rep2;
	f.weights[rep2.pub] = f.delta;
	nano::consensus::effects e2;
	election.vote (rep2.pub, b1->hash (), 5, e2);
	ASSERT_FALSE (e2.on_quorum_reached.has_value ());
	ASSERT_FALSE (e2.on_winner_changed.has_value ());
	ASSERT_FALSE (e2.on_final_quorum_reached.has_value ());
}

TEST (consensus, quorum_reached_is_one_shot)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = f.delta;
	f.weights[rep2.pub] = f.delta;

	nano::consensus::effects e1;
	election.vote (rep1.pub, b1->hash (), 1, e1);
	ASSERT_TRUE (e1.on_quorum_reached.has_value ());

	nano::consensus::effects e2;
	election.vote (rep2.pub, b1->hash (), 1, e2);
	ASSERT_TRUE (election.has_quorum ());
	ASSERT_FALSE (e2.on_quorum_reached.has_value ()); // latched, fires only once
}

TEST (consensus, equal_weight_tally_collision)
{
	fixture f;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 };
	election.publish (b2);

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 50;
	f.weights[rep2.pub] = 50;
	nano::consensus::effects e1;
	election.vote (rep1.pub, b1->hash (), 1, e1);
	nano::consensus::effects e2;
	election.vote (rep2.pub, b2->hash (), 1, e2);

	// tally_t is keyed by weight; equal-weight blocks collide and one entry survives (parity
	// quirk preserved from election.cpp:455-462). Deterministic on size and key.
	auto tally = election.tally ();
	ASSERT_EQ (1, tally.size ());
	ASSERT_EQ (nano::uint128_t{ 50 }, tally.begin ()->first);
}

TEST (consensus, remove_block_guards_winner_and_orphans_votes)
{
	fixture f;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 }; // status winner = b1
	election.publish (b2);

	nano::keypair rep1;
	f.weights[rep1.pub] = 10;
	nano::consensus::effects e;
	election.vote (rep1.pub, b2->hash (), 1, e);

	election.remove_block (b1->hash ()); // guarded: b1 is the status winner -> no-op
	ASSERT_EQ (2, election.block_count ());
	ASSERT_TRUE (election.contains_block (b1->hash ()));

	election.remove_block (b2->hash ()); // removed + orphan vote erased
	ASSERT_EQ (1, election.block_count ());
	ASSERT_FALSE (election.contains_block (b2->hash ()));
	ASSERT_FALSE (election.contains_voter (rep1.pub));
}

TEST (consensus, apply_removed_votes_erases_accounts)
{
	fixture f;
	auto b1 = make_block (1);
	nano::consensus::election election{ f.ports (), b1 };

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 10;
	f.weights[rep2.pub] = 10;
	nano::consensus::effects e1;
	election.vote (rep1.pub, b1->hash (), 1, e1);
	nano::consensus::effects e2;
	election.vote (rep2.pub, b1->hash (), 1, e2);

	election.apply_removed_votes ({ nano::account{ rep1.pub } });
	ASSERT_FALSE (election.contains_voter (rep1.pub));
	ASSERT_TRUE (election.contains_voter (rep2.pub));
}

TEST (consensus, publish_add_then_replace)
{
	fixture f;
	auto b1 = make_block (1);
	auto b2 = make_block (2);
	nano::consensus::election election{ f.ports (), b1 };

	ASSERT_FALSE (election.publish (b2)); // newly added
	ASSERT_EQ (2, election.block_count ());

	auto b2_again = make_block (2); // same fields => same hash
	ASSERT_EQ (b1->hash () == b2_again->hash (), false);
	ASSERT_TRUE (election.publish (b2_again)); // already present -> content replaced
	ASSERT_EQ (2, election.block_count ());

	// Replacing the status-winner's block updates the cached winner pointer.
	auto b1_again = make_block (1);
	ASSERT_EQ (b1->hash (), b1_again->hash ());
	ASSERT_TRUE (election.publish (b1_again));
	ASSERT_EQ (b1_again, election.status_winner ());
}

TEST (consensus, state_transitions_match_valid_change)
{
	fixture f;
	auto b1 = make_block (1);

	{
		nano::consensus::election election{ f.ports (), b1 };
		// state_change returns true on FAILURE, false on success (inverted, matching develop).
		ASSERT_FALSE (election.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::active));
		ASSERT_EQ (nano::consensus::election_state::active, election.state ());
		ASSERT_FALSE (election.state_change (nano::consensus::election_state::active, nano::consensus::election_state::confirmed));
		ASSERT_TRUE (election.confirmed ());
		ASSERT_FALSE (election.state_change (nano::consensus::election_state::confirmed, nano::consensus::election_state::expired_confirmed));
		// No transitions are valid out of a terminal state.
		ASSERT_TRUE (election.state_change (nano::consensus::election_state::expired_confirmed, nano::consensus::election_state::active));
	}
	{
		nano::consensus::election election{ f.ports (), b1 };
		// Invalid direct transition (passive -> expired_confirmed is not in the table).
		ASSERT_TRUE (election.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::expired_confirmed));
		ASSERT_EQ (nano::consensus::election_state::passive, election.state ());
		ASSERT_FALSE (election.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::cancelled));
		ASSERT_EQ (nano::consensus::election_state::cancelled, election.state ());
	}
}
