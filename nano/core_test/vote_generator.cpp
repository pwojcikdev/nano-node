#include <nano/node/vote_generator.hpp>
#include <nano/secure/voting_policy.hpp>

#include <gtest/gtest.h>

TEST (vote_broadcast_index, fifo_order_and_dedup)
{
	nano::vote_broadcast_index index;
	auto permit = nano::vote_permit::dummy (nano::vote_type::normal);

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };

	ASSERT_TRUE (index.push (root1, permit));
	ASSERT_TRUE (index.push (root2, permit));
	ASSERT_TRUE (index.push (root3, permit));
	ASSERT_FALSE (index.push (root1, permit)); // Duplicate rejected
	ASSERT_EQ (3, index.size ());

	auto batch = index.next_batch (2);
	ASSERT_EQ (2, batch.size ());
	ASSERT_EQ (root1, batch[0].qualified_root ());
	ASSERT_EQ (root2, batch[1].qualified_root ());
	ASSERT_EQ (1, index.size ());
}

TEST (vote_broadcast_index, erase_and_reinsert)
{
	nano::vote_broadcast_index index;
	auto permit = nano::vote_permit::dummy (nano::vote_type::normal);

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };

	index.push (root1, permit);
	index.push (root2, permit);
	index.push (root3, permit);

	ASSERT_TRUE (index.erase (root2));
	ASSERT_FALSE (index.erase (root2)); // Already gone

	// Re-insert goes to the back
	ASSERT_TRUE (index.push (root2, permit));

	auto batch = index.next_batch (10);
	ASSERT_EQ (3, batch.size ());
	ASSERT_EQ (root1, batch[0].qualified_root ());
	ASSERT_EQ (root3, batch[1].qualified_root ());
	ASSERT_EQ (root2, batch[2].qualified_root ());
}
