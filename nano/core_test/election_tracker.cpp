#include <nano/node/election.hpp>
#include <nano/node/scheduler/election_tracker.hpp>
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
// Helper class to create elections for testing
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
		return std::make_shared<nano::election> (node, next_block (), behavior);
	}
};
}

TEST (election_tracker, basic_operations)
{
	test_context context{ 10 };
	nano::scheduler::election_tracker tracker;

	// Test initial state
	ASSERT_TRUE (tracker.empty ());
	ASSERT_EQ (tracker.size (), 0);
	ASSERT_TRUE (tracker.empty (1));
	ASSERT_EQ (tracker.size (1), 0);

	// Create elections
	auto election1 = context.random_election ();
	auto election2 = context.random_election ();
	auto election3 = context.random_election ();

	// Test insertion
	ASSERT_TRUE (tracker.insert (election1, 1, 100));
	ASSERT_FALSE (tracker.empty ());
	ASSERT_EQ (tracker.size (), 1);
	ASSERT_EQ (tracker.size (1), 1);
	ASSERT_FALSE (tracker.empty (1));
	ASSERT_TRUE (tracker.contains (election1));

	// Test duplicate insertion (should fail)
	ASSERT_FALSE (tracker.insert (election1, 1, 200));
	ASSERT_FALSE (tracker.insert (election1, 2, 100));
	ASSERT_EQ (tracker.size (), 1);

	// Insert more elections
	ASSERT_TRUE (tracker.insert (election2, 1, 200));
	ASSERT_TRUE (tracker.insert (election3, 2, 150));
	ASSERT_EQ (tracker.size (), 3);
	ASSERT_EQ (tracker.size (1), 2);
	ASSERT_EQ (tracker.size (2), 1);
	ASSERT_TRUE (tracker.contains (election2));
	ASSERT_TRUE (tracker.contains (election3));

	// Test erase
	ASSERT_TRUE (tracker.erase (election1));
	ASSERT_EQ (tracker.size (), 2);
	ASSERT_EQ (tracker.size (1), 1);
	ASSERT_FALSE (tracker.contains (election1));

	// Test erase non-existent (should fail)
	ASSERT_FALSE (tracker.erase (election1));
	ASSERT_EQ (tracker.size (), 2);

	// Clear all
	tracker.clear ();
	ASSERT_TRUE (tracker.empty ());
	ASSERT_EQ (tracker.size (), 0);
	ASSERT_FALSE (tracker.contains (election2));
	ASSERT_FALSE (tracker.contains (election3));
}

TEST (election_tracker, worst_priority)
{
	test_context context{ 10 };
	nano::scheduler::election_tracker tracker;

	// Test empty bucket
	auto worst = tracker.worst (1);
	ASSERT_FALSE (worst.has_value ());

	// Create elections with different priorities
	auto election1 = context.random_election ();
	auto election2 = context.random_election ();
	auto election3 = context.random_election ();
	auto election4 = context.random_election ();
	auto election5 = context.random_election ();

	// Add to bucket 1 with different priorities
	tracker.insert (election1, 1, 100);
	tracker.insert (election2, 1, 500); // Worst priority (highest value)
	tracker.insert (election3, 1, 300);
	tracker.insert (election4, 1, 200);

	// Add to bucket 2
	tracker.insert (election5, 2, 600);

	// Test worst for bucket 1 (should be election2 with priority 500)
	worst = tracker.worst (1);
	ASSERT_TRUE (worst.has_value ());
	ASSERT_EQ (worst->election, election2);
	ASSERT_EQ (worst->priority, 500);
	ASSERT_EQ (worst->bucket, 1);

	// Test worst for bucket 2
	worst = tracker.worst (2);
	ASSERT_TRUE (worst.has_value ());
	ASSERT_EQ (worst->election, election5);
	ASSERT_EQ (worst->priority, 600);
	ASSERT_EQ (worst->bucket, 2);

	// Test worst for empty bucket
	worst = tracker.worst (3);
	ASSERT_FALSE (worst.has_value ());

	// Remove the worst priority election and check again
	ASSERT_TRUE (tracker.erase (election2));
	worst = tracker.worst (1);
	ASSERT_TRUE (worst.has_value ());
	ASSERT_EQ (worst->election, election3);
	ASSERT_EQ (worst->priority, 300);
}

TEST (election_tracker, multiple_buckets)
{
	test_context context{ 20 };
	nano::scheduler::election_tracker tracker;

	std::vector<std::shared_ptr<nano::election>> elections;

	// Create and insert elections across multiple buckets
	for (int bucket = 0; bucket < 5; ++bucket)
	{
		for (int i = 0; i < 3; ++i)
		{
			auto election = context.random_election ();
			elections.push_back (election);
			nano::priority_timestamp priority = (bucket * 100) + (i * 10);
			ASSERT_TRUE (tracker.insert (election, bucket, priority));
		}
	}

	// Verify total size
	ASSERT_EQ (tracker.size (), 15);

	// Verify sizes per bucket
	for (int bucket = 0; bucket < 5; ++bucket)
	{
		ASSERT_EQ (tracker.size (bucket), 3);
		ASSERT_FALSE (tracker.empty (bucket));
	}

	// Test sizes() method
	auto sizes = tracker.sizes ();
	ASSERT_EQ (sizes.size (), 5);
	for (int bucket = 0; bucket < 5; ++bucket)
	{
		ASSERT_EQ (sizes[bucket], 3);
	}

	// Remove elections from bucket 2
	for (int i = 6; i < 9; ++i)
	{
		ASSERT_TRUE (tracker.erase (elections[i]));
	}

	// Verify bucket 2 is empty
	ASSERT_TRUE (tracker.empty (2));
	ASSERT_EQ (tracker.size (2), 0);
	ASSERT_EQ (tracker.size (), 12);

	// Test worst priority for each remaining bucket
	for (int bucket : { 0, 1, 3, 4 })
	{
		auto worst = tracker.worst (bucket);
		ASSERT_TRUE (worst.has_value ());
		// The worst priority in each bucket should be (bucket * 100 + 20)
		ASSERT_EQ (worst->priority, (bucket * 100) + 20);
	}
}

TEST (election_tracker, priority_ordering_with_equal_priorities)
{
	test_context context{ 10 };
	nano::scheduler::election_tracker tracker;

	// Create elections
	auto election1 = context.random_election ();
	auto election2 = context.random_election ();
	auto election3 = context.random_election ();

	// Insert with same priority - should use qualified_root as tiebreaker
	tracker.insert (election1, 1, 100);
	tracker.insert (election2, 1, 100);
	tracker.insert (election3, 1, 100);

	ASSERT_EQ (tracker.size (), 3);
	ASSERT_EQ (tracker.size (1), 3);

	// The worst should be deterministic based on qualified_root ordering
	auto worst = tracker.worst (1);
	ASSERT_TRUE (worst.has_value ());
	ASSERT_EQ (worst->priority, 100);

	// Remove the worst and check the next
	auto worst_election = worst->election;
	ASSERT_TRUE (tracker.erase (worst_election));

	auto next_worst = tracker.worst (1);
	ASSERT_TRUE (next_worst.has_value ());
	ASSERT_NE (next_worst->election, worst_election);
}