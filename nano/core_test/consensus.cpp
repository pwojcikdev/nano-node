#include <nano/consensus/consensus.hpp>
#include <nano/lib/keypair.hpp>
#include <nano/lib/numbers.hpp>

#include <gtest/gtest.h>

#include <unordered_map>

// Pure unit tests for the ORV consensus engine. No node/system: representative weight and the
// quorum delta are injected, so each test is a deterministic function of the ballots. These tests
// pin the rules (margin quorum, live weight, sticky winner-specific final weight, sum-gated winner
// flip, one-shot quorum latch, candidate restriction). The engine is hash-only, so the tests need
// no blocks at all.

namespace
{
nano::block_hash bh (uint64_t n)
{
	return nano::block_hash{ nano::uint256_t{ n } };
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

TEST (consensus, construction_seeds_initial_candidate)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	ASSERT_EQ (1, engine.voter_count ()); // seeded null-account ballot
	ASSERT_EQ (1, engine.candidate_count ());
	ASSERT_EQ (h1, engine.winner ());
	ASSERT_TRUE (engine.contains_candidate (h1));
	ASSERT_EQ (nano::consensus::election_state::passive, engine.state ());
	ASSERT_FALSE (engine.confirmed ());
	ASSERT_FALSE (engine.has_quorum ()); // margin 0 - 0 < delta
}

TEST (consensus, margin_quorum_single_block)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;

	auto fx = engine.vote (rep1.pub, h1, 1);

	ASSERT_TRUE (engine.has_quorum ()); // (delta - 0) >= delta
	ASSERT_TRUE (fx.on_quorum_reached.has_value ());
	ASSERT_EQ (h1, fx.on_quorum_reached->winner);
	ASSERT_FALSE (fx.on_final_quorum_reached.has_value ()); // no final votes
}

// The margin rule is NOT an absolute "leader >= delta" threshold: the leader must beat the
// runner-up by delta.
TEST (consensus, margin_is_not_absolute_with_fork)
{
	fixture f;
	f.delta = 100;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 };
	ASSERT_TRUE (engine.register_candidate (h2));

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 150;
	f.weights[rep2.pub] = 60;

	engine.vote (rep1.pub, h1, 1);
	engine.vote (rep2.pub, h2, 1);

	// winner h1=150, runner-up h2=60, margin 90 < 100 -> NO quorum
	ASSERT_FALSE (engine.has_quorum ());

	f.weights[rep1.pub] = 170; // margin 170 - 60 = 110 >= 100
	ASSERT_TRUE (engine.has_quorum ());
}

TEST (consensus, live_weight_is_recomputed_not_frozen)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;
	engine.vote (rep1.pub, h1, 1);
	ASSERT_TRUE (engine.has_quorum ());

	f.weights[rep1.pub] = f.delta - 1; // weight dropped after the ballot was cast
	ASSERT_FALSE (engine.has_quorum ()); // recomputed from live weight, not a cast-time snapshot
}

TEST (consensus, sticky_winner_specific_final_weight)
{
	fixture f;
	f.delta = 100;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 };
	ASSERT_TRUE (engine.register_candidate (h2));

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 50;
	f.weights[rep2.pub] = 200;

	// rep1 casts a FINAL ballot for h2 (the leader while only it has voted)
	engine.vote (rep1.pub, h2, nano::consensus::ballot::final_timestamp);
	ASSERT_EQ (50, engine.final_weight ());

	// rep2 normal-votes h1, making h1 the leader. h1 has no final votes, so the winner-specific
	// final weight stays sticky at 50 (NOT reset to 0).
	engine.vote (rep2.pub, h1, 1);
	auto tally = engine.tally ();
	ASSERT_EQ (h1, tally.begin ()->second); // h1 (200) leads
	ASSERT_EQ (50, engine.final_weight ()); // sticky
}

TEST (consensus, sum_gated_winner_flip)
{
	fixture f;
	f.delta = 100;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 }; // initial winner = h1
	ASSERT_TRUE (engine.register_candidate (h2));

	nano::keypair rep1, rep2;

	// Leader is h2 but total cast weight < delta -> NO flip yet (sum gate).
	f.weights[rep1.pub] = 60;
	auto fx1 = engine.vote (rep1.pub, h2, 1);
	ASSERT_FALSE (fx1.on_winner_changed.has_value ());
	ASSERT_EQ (h1, engine.winner ());

	// Now sum >= delta and the leader (h2) differs from the locked winner (h1) -> flip.
	f.weights[rep2.pub] = 80;
	auto fx2 = engine.vote (rep2.pub, h2, 1);
	ASSERT_TRUE (fx2.on_winner_changed.has_value ());
	ASSERT_EQ (h1, fx2.on_winner_changed->previous_winner);
	ASSERT_EQ (h2, fx2.on_winner_changed->new_winner);
	ASSERT_EQ (h2, engine.winner ());
	ASSERT_TRUE (fx2.on_quorum_reached.has_value ()); // margin 140 - 0 >= 100
	ASSERT_EQ (h2, fx2.on_quorum_reached->winner);
}

TEST (consensus, final_quorum_confirms_and_freezes)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	nano::keypair rep1;
	f.weights[rep1.pub] = f.delta;

	auto fx = engine.vote (rep1.pub, h1, nano::consensus::ballot::final_timestamp);

	ASSERT_TRUE (fx.on_final_quorum_reached.has_value ());
	ASSERT_EQ (h1, fx.on_final_quorum_reached->winner);
	ASSERT_EQ (nano::consensus::election_state::confirmed, engine.state ());
	ASSERT_TRUE (engine.confirmed ());

	// Once confirmed, further ballots are not evaluated.
	nano::keypair rep2;
	f.weights[rep2.pub] = f.delta;
	auto fx2 = engine.vote (rep2.pub, h1, 5);
	ASSERT_FALSE (fx2.on_quorum_reached.has_value ());
	ASSERT_FALSE (fx2.on_winner_changed.has_value ());
	ASSERT_FALSE (fx2.on_final_quorum_reached.has_value ());
}

TEST (consensus, quorum_reached_is_one_shot)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = f.delta;
	f.weights[rep2.pub] = f.delta;

	auto fx1 = engine.vote (rep1.pub, h1, 1);
	ASSERT_TRUE (fx1.on_quorum_reached.has_value ());

	auto fx2 = engine.vote (rep2.pub, h1, 1);
	ASSERT_TRUE (engine.has_quorum ());
	ASSERT_FALSE (fx2.on_quorum_reached.has_value ()); // latched, fires only once
}

TEST (consensus, equal_weight_tally_collision)
{
	fixture f;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 };
	ASSERT_TRUE (engine.register_candidate (h2));

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 50;
	f.weights[rep2.pub] = 50;
	engine.vote (rep1.pub, h1, 1);
	engine.vote (rep2.pub, h2, 1);

	// tally_t is keyed by weight; equal-weight blocks collide and one entry survives. Deterministic
	// on size and key.
	auto tally = engine.tally ();
	ASSERT_EQ (1, tally.size ());
	ASSERT_EQ (nano::uint128_t{ 50 }, tally.begin ()->first);
}

TEST (consensus, remove_candidate_guards_winner_and_orphans_ballots)
{
	fixture f;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 }; // winner = h1
	ASSERT_TRUE (engine.register_candidate (h2));

	nano::keypair rep1;
	f.weights[rep1.pub] = 10;
	engine.vote (rep1.pub, h2, 1);

	engine.remove_candidate (h1); // guarded: h1 is the winner -> no-op
	ASSERT_EQ (2, engine.candidate_count ());
	ASSERT_TRUE (engine.contains_candidate (h1));

	engine.remove_candidate (h2); // removed + orphan ballot erased
	ASSERT_EQ (1, engine.candidate_count ());
	ASSERT_FALSE (engine.contains_candidate (h2));
	ASSERT_FALSE (engine.contains_voter (rep1.pub));
}

TEST (consensus, forget_voters_erases_accounts)
{
	fixture f;
	auto h1 = bh (1);
	nano::consensus::engine engine{ f.ports (), h1 };

	nano::keypair rep1, rep2;
	f.weights[rep1.pub] = 10;
	f.weights[rep2.pub] = 10;
	engine.vote (rep1.pub, h1, 1);
	engine.vote (rep2.pub, h1, 1);

	engine.forget_voters ({ nano::account{ rep1.pub } });
	ASSERT_FALSE (engine.contains_voter (rep1.pub));
	ASSERT_TRUE (engine.contains_voter (rep2.pub));
}

TEST (consensus, register_candidate_is_idempotent)
{
	fixture f;
	auto h1 = bh (1);
	auto h2 = bh (2);
	nano::consensus::engine engine{ f.ports (), h1 };

	ASSERT_TRUE (engine.register_candidate (h2)); // newly added
	ASSERT_EQ (2, engine.candidate_count ());
	ASSERT_FALSE (engine.register_candidate (h2)); // already present
	ASSERT_EQ (2, engine.candidate_count ());
	ASSERT_FALSE (engine.register_candidate (h1)); // initial candidate already present
}

TEST (consensus, state_transitions_match_valid_change)
{
	fixture f;
	auto h1 = bh (1);

	{
		nano::consensus::engine engine{ f.ports (), h1 };
		// state_change returns true on FAILURE, false on success.
		ASSERT_FALSE (engine.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::active));
		ASSERT_EQ (nano::consensus::election_state::active, engine.state ());
		ASSERT_FALSE (engine.state_change (nano::consensus::election_state::active, nano::consensus::election_state::confirmed));
		ASSERT_TRUE (engine.confirmed ());
		ASSERT_FALSE (engine.state_change (nano::consensus::election_state::confirmed, nano::consensus::election_state::expired_confirmed));
		// No transitions are valid out of a terminal state.
		ASSERT_TRUE (engine.state_change (nano::consensus::election_state::expired_confirmed, nano::consensus::election_state::active));
	}
	{
		nano::consensus::engine engine{ f.ports (), h1 };
		// Invalid direct transition (passive -> expired_confirmed is not in the table).
		ASSERT_TRUE (engine.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::expired_confirmed));
		ASSERT_EQ (nano::consensus::election_state::passive, engine.state ());
		ASSERT_FALSE (engine.state_change (nano::consensus::election_state::passive, nano::consensus::election_state::cancelled));
		ASSERT_EQ (nano::consensus::election_state::cancelled, engine.state ());
	}
}
