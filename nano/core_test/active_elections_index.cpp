#include <nano/node/active_elections_index.hpp>
#include <nano/node/election.hpp>
#include <nano/test_common/chains.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <numeric>

using namespace std::chrono_literals;

namespace
{
// Full node is necessary to create elections
class test_context final
{
public:
	nano::test::system system;
	nano::node & node;
	std::deque<std::shared_ptr<nano::block>> blocks;

	explicit test_context (size_t count = 10) :
		node{ *system.add_node () }
	{
		auto chain = nano::test::setup_chain (system, node, count);
		blocks.insert (blocks.end (), chain.begin (), chain.end ());
	}

	std::shared_ptr<nano::block> next_block ()
	{
		debug_assert (!blocks.empty ());
		auto block = blocks.front ();
		blocks.pop_front ();
		return block;
	}

	std::shared_ptr<nano::election> random_election (nano::election_behavior behavior = nano::election_behavior::priority)
	{
		return std::make_shared<nano::election> (node, next_block (), nullptr, nullptr, behavior);
	}
};
}

TEST (active_elections_index, insert)
{
	test_context context{ 10 };
	nano::active_elections_index index;

	// Create elections with different behaviors and buckets
	auto election1 = context.random_election (nano::election_behavior::priority);
	auto election2 = context.random_election (nano::election_behavior::hinted);
	auto election3 = context.random_election (nano::election_behavior::optimistic);

	// Test initial state
	ASSERT_EQ (index.size (), 0);
	ASSERT_FALSE (index.exists (election1));

	// Insert first election
	index.insert (election1, nano::election_behavior::priority, 1, 100);
	ASSERT_EQ (index.size (), 1);
	ASSERT_TRUE (index.exists (election1));
	ASSERT_TRUE (index.exists (election1->qualified_root));
	ASSERT_EQ (index.size (nano::election_behavior::priority), 1);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 1);

	// Insert second election with different behavior
	index.insert (election2, nano::election_behavior::hinted, 2, 200);
	ASSERT_EQ (index.size (), 2);
	ASSERT_TRUE (index.exists (election2));
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::hinted, 2), 1);

	// Insert third election
	index.insert (election3, nano::election_behavior::optimistic, 1, 50);
	ASSERT_EQ (index.size (), 3);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic), 1);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic, 1), 1);
}

TEST (active_elections_index, erase)
{
	test_context context{ 5 };
	nano::active_elections_index index;

	auto election1 = context.random_election ();
	auto election2 = context.random_election ();
	auto election3 = context.random_election ();

	// Insert elections
	index.insert (election1, nano::election_behavior::priority, 1, 100);
	index.insert (election2, nano::election_behavior::priority, 1, 200);
	index.insert (election3, nano::election_behavior::hinted, 2, 300);

	ASSERT_EQ (index.size (), 3);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 2);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 2);

	// Erase existing election
	ASSERT_TRUE (index.erase (election1));
	ASSERT_EQ (index.size (), 2);
	ASSERT_FALSE (index.exists (election1));
	ASSERT_FALSE (index.exists (election1->qualified_root));
	ASSERT_EQ (index.size (nano::election_behavior::priority), 1);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 1);

	// Try to erase non-existent election
	ASSERT_FALSE (index.erase (election1));
	ASSERT_EQ (index.size (), 2);

	// Erase remaining elections
	ASSERT_TRUE (index.erase (election2));
	ASSERT_TRUE (index.erase (election3));
	ASSERT_EQ (index.size (), 0);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 0);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 0);
}

TEST (active_elections_index, update)
{
	test_context context{ 5 };
	nano::active_elections_index index;

	auto election1 = context.random_election ();
	auto election2 = context.random_election ();

	// Insert elections
	index.insert (election1, nano::election_behavior::priority, 1, 100);
	index.insert (election2, nano::election_behavior::hinted, 2, 200);

	ASSERT_EQ (index.size (nano::election_behavior::priority), 1);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 1);
	ASSERT_EQ (index.size (nano::election_behavior::hinted, 2), 1);

	// Update election1 behavior from priority to optimistic
	index.update (election1, nano::election_behavior::optimistic);

	ASSERT_EQ (index.size (), 2);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 0);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic), 1);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 0);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic, 1), 1);

	// Verify election still exists
	ASSERT_TRUE (index.exists (election1));

	// Update with same behavior (no change)
	index.update (election2, nano::election_behavior::hinted);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::hinted, 2), 1);
}

TEST (active_elections_index, exists)
{
	test_context context{ 3 };
	nano::active_elections_index index;

	auto election1 = context.random_election ();
	auto election2 = context.random_election ();
	auto election3 = context.random_election ();

	// Test non-existent elections
	ASSERT_FALSE (index.exists (election1));
	ASSERT_FALSE (index.exists (election1->qualified_root));

	// Insert election1
	index.insert (election1, nano::election_behavior::priority, 1, 100);

	// Test exists with election pointer
	ASSERT_TRUE (index.exists (election1));
	ASSERT_FALSE (index.exists (election2));
	ASSERT_FALSE (index.exists (election3));

	// Test exists with qualified_root
	ASSERT_TRUE (index.exists (election1->qualified_root));
	ASSERT_FALSE (index.exists (election2->qualified_root));
	ASSERT_FALSE (index.exists (election3->qualified_root));

	// Add more elections and test
	index.insert (election2, nano::election_behavior::hinted, 2, 200);
	ASSERT_TRUE (index.exists (election2));
	ASSERT_TRUE (index.exists (election2->qualified_root));
}

TEST (active_elections_index, election_lookup)
{
	test_context context{ 3 };
	nano::active_elections_index index;

	auto election1 = context.random_election ();
	auto election2 = context.random_election ();

	// Test lookup on empty index
	ASSERT_EQ (index.election (election1->qualified_root), nullptr);

	// Insert elections
	index.insert (election1, nano::election_behavior::priority, 1, 100);
	index.insert (election2, nano::election_behavior::hinted, 2, 200);

	// Test successful lookup
	ASSERT_EQ (index.election (election1->qualified_root), election1);
	ASSERT_EQ (index.election (election2->qualified_root), election2);

	// Test lookup for non-existent root
	auto election3 = context.random_election ();
	ASSERT_EQ (index.election (election3->qualified_root), nullptr);

	// Test info method
	auto info1 = index.info (election1);
	ASSERT_TRUE (info1.has_value ());
	ASSERT_EQ (info1->election, election1);
	ASSERT_EQ (info1->behavior, nano::election_behavior::priority);
	ASSERT_EQ (info1->bucket, 1);
	ASSERT_EQ (info1->priority, 100);

	auto info3 = index.info (election3);
	ASSERT_FALSE (info3.has_value ());
}

TEST (active_elections_index, size_operations)
{
	test_context context{ 10 };
	nano::active_elections_index index;

	// Test empty index
	ASSERT_EQ (index.size (), 0);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 0);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 0);

	// Add elections with different behaviors and buckets
	auto e1 = context.random_election ();
	auto e2 = context.random_election ();
	auto e3 = context.random_election ();
	auto e4 = context.random_election ();
	auto e5 = context.random_election ();

	index.insert (e1, nano::election_behavior::priority, 1, 100);
	index.insert (e2, nano::election_behavior::priority, 1, 200);
	index.insert (e3, nano::election_behavior::priority, 2, 300);
	index.insert (e4, nano::election_behavior::hinted, 1, 400);
	index.insert (e5, nano::election_behavior::optimistic, 3, 500);

	// Test total size
	ASSERT_EQ (index.size (), 5);

	// Test size by behavior
	ASSERT_EQ (index.size (nano::election_behavior::priority), 3);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic), 1);
	ASSERT_EQ (index.size (nano::election_behavior::manual), 0);

	// Test size by behavior and bucket
	ASSERT_EQ (index.size (nano::election_behavior::priority, 1), 2);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 2), 1);
	ASSERT_EQ (index.size (nano::election_behavior::priority, 3), 0);
	ASSERT_EQ (index.size (nano::election_behavior::hinted, 1), 1);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic, 3), 1);
}

TEST (active_elections_index, highest)
{
	test_context context{ 10 };
	nano::active_elections_index index;

	// Test empty index
	auto [empty_election, empty_priority] = index.highest (nano::election_behavior::priority, 1);
	ASSERT_EQ (empty_election, nullptr);
	ASSERT_EQ (empty_priority, std::numeric_limits<nano::priority_timestamp>::max ());

	// Add elections with different priorities
	auto e1 = context.random_election ();
	auto e2 = context.random_election ();
	auto e3 = context.random_election ();
	auto e4 = context.random_election ();

	index.insert (e1, nano::election_behavior::priority, 1, 300); // Highest priority value in bucket 1
	index.insert (e2, nano::election_behavior::priority, 1, 100);
	index.insert (e3, nano::election_behavior::priority, 1, 200);
	index.insert (e4, nano::election_behavior::priority, 2, 50); // Only election in bucket 2

	// Test highest for bucket 1 (should return e1 with priority 300 - highest value)
	auto [top1_election, top1_priority] = index.highest (nano::election_behavior::priority, 1);
	ASSERT_EQ (top1_election, e1);
	ASSERT_EQ (top1_priority, 300);

	// Test highest for bucket 2 (should return e4 with priority 50)
	auto [top2_election, top2_priority] = index.highest (nano::election_behavior::priority, 2);
	ASSERT_EQ (top2_election, e4);
	ASSERT_EQ (top2_priority, 50);

	// Test highest for empty bucket
	auto [top3_election, top3_priority] = index.highest (nano::election_behavior::priority, 3);
	ASSERT_EQ (top3_election, nullptr);
	ASSERT_EQ (top3_priority, std::numeric_limits<nano::priority_timestamp>::max ());

	// Test highest for different behavior
	auto e5 = context.random_election ();
	index.insert (e5, nano::election_behavior::hinted, 1, 75);
	auto [top4_election, top4_priority] = index.highest (nano::election_behavior::hinted, 1);
	ASSERT_EQ (top4_election, e5);
	ASSERT_EQ (top4_priority, 75);
}

TEST (active_elections_index, list_with_cutoff)
{
	test_context context{ 5 };
	nano::active_elections_index index;

	auto e1 = context.random_election ();
	auto e2 = context.random_election ();
	auto e3 = context.random_election ();

	// Insert elections (they start with timestamp at epoch)
	index.insert (e1, nano::election_behavior::priority, 1, 100);
	index.insert (e2, nano::election_behavior::priority, 2, 200);
	index.insert (e3, nano::election_behavior::hinted, 1, 300);

	auto initial_time = std::chrono::steady_clock::now ();
	auto cutoff = initial_time + 1s; // All elections should be before this cutoff

	// Process all elections and update their timestamps to initial_time
	auto elections_list = index.list (cutoff, initial_time);
	
	// All elections should have been processed
	ASSERT_EQ (elections_list.size (), 3);
	std::set<std::shared_ptr<nano::election>> processed (elections_list.begin (), elections_list.end ());
	ASSERT_TRUE (processed.count (e1));
	ASSERT_TRUE (processed.count (e2));
	ASSERT_TRUE (processed.count (e3));

	// Now test with earlier cutoff - no elections should be processed
	// because their timestamps were updated to initial_time in the previous list call
	auto earlier_cutoff = initial_time - 1s;
	auto elections_list2 = index.list (earlier_cutoff);

	ASSERT_EQ (elections_list2.size (), 0);
	ASSERT_TRUE (elections_list2.empty ());

	// Test with future time - should process all elections again
	auto future_time = initial_time + 2s;
	auto future_cutoff = future_time - 1s; // Between initial_time and future_time
	auto elections_list3 = index.list (future_cutoff, future_time);

	// All elections should be processed since cutoff is after their timestamp
	ASSERT_EQ (elections_list3.size (), 3);
}

TEST (active_elections_index, trigger)
{
	test_context context{ 3 };
	nano::active_elections_index index;

	auto e1 = context.random_election ();
	auto e2 = context.random_election ();

	// Insert elections (they start with timestamp at epoch)
	index.insert (e1, nano::election_behavior::priority, 1, 100);
	index.insert (e2, nano::election_behavior::priority, 2, 200);

	// Process elections to update their timestamps to a future time
	auto future_time = std::chrono::steady_clock::now () + 1h;
	auto cutoff = future_time + 1s;
	index.list (cutoff, future_time);

	// Trigger e1 to reset its timestamp
	index.trigger (e1);

	// Check that e1 has been triggered (timestamp reset to epoch)
	// We can verify this by using list with a very early cutoff
	auto very_early_cutoff = std::chrono::steady_clock::time_point{} + 1ms;
	auto triggered_elections = index.list (very_early_cutoff);

	// Only e1 should have been processed (because it was triggered)
	ASSERT_EQ (triggered_elections.size (), 1);
	ASSERT_EQ (triggered_elections[0], e1);
}

TEST (active_elections_index, any)
{
	test_context context{ 3 };
	nano::active_elections_index index;

	// Test empty index
	auto cutoff = std::chrono::steady_clock::now ();
	ASSERT_FALSE (index.any (cutoff));

	auto e1 = context.random_election ();
	auto e2 = context.random_election ();

	// Insert elections (they will have timestamp at epoch initially)
	index.insert (e1, nano::election_behavior::priority, 1, 100);
	index.insert (e2, nano::election_behavior::priority, 2, 200);

	// Check with cutoff in the future - should find elections
	auto future_cutoff = std::chrono::steady_clock::now () + 1s;
	ASSERT_TRUE (index.any (future_cutoff));

	// Update timestamps by processing elections
	auto now = std::chrono::steady_clock::now ();
	index.list (future_cutoff, now);

	// Check with cutoff in the past - should not find elections
	auto past_cutoff = now - 1s;
	ASSERT_FALSE (index.any (past_cutoff));

	// Trigger one election to reset its timestamp
	index.trigger (e1);

	// Now should find the triggered election even with past cutoff
	ASSERT_TRUE (index.any (past_cutoff));
}

TEST (active_elections_index, clear)
{
	test_context context{ 5 };
	nano::active_elections_index index;

	// Add multiple elections
	auto e1 = context.random_election ();
	auto e2 = context.random_election ();
	auto e3 = context.random_election ();
	auto e4 = context.random_election ();

	index.insert (e1, nano::election_behavior::priority, 1, 100);
	index.insert (e2, nano::election_behavior::priority, 2, 200);
	index.insert (e3, nano::election_behavior::hinted, 1, 300);
	index.insert (e4, nano::election_behavior::optimistic, 3, 400);

	// Verify index is populated
	ASSERT_EQ (index.size (), 4);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 2);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 1);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic), 1);
	ASSERT_TRUE (index.exists (e1));
	ASSERT_TRUE (index.exists (e2->qualified_root));

	// Clear the index
	index.clear ();

	// Verify everything is cleared
	ASSERT_EQ (index.size (), 0);
	ASSERT_EQ (index.size (nano::election_behavior::priority), 0);
	ASSERT_EQ (index.size (nano::election_behavior::hinted), 0);
	ASSERT_EQ (index.size (nano::election_behavior::optimistic), 0);
	ASSERT_EQ (index.size (nano::election_behavior::manual), 0);
	ASSERT_FALSE (index.exists (e1));
	ASSERT_FALSE (index.exists (e2));
	ASSERT_FALSE (index.exists (e3));
	ASSERT_FALSE (index.exists (e4));
	ASSERT_FALSE (index.exists (e1->qualified_root));

	// Test that we can still use the index after clearing
	index.insert (e1, nano::election_behavior::manual, 0, 50);
	ASSERT_EQ (index.size (), 1);
	ASSERT_TRUE (index.exists (e1));
	ASSERT_EQ (index.size (nano::election_behavior::manual), 1);
}