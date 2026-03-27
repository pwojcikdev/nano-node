#include <nano/node/vote_generator.hpp>
#include <nano/secure/voting_policy.hpp>

#include <gtest/gtest.h>

namespace
{
auto make_permit (nano::qualified_root const & root, nano::block_hash const & hash = { 0 })
{
	return nano::vote_permit::dummy (root, hash, nano::vote_type::normal);
}
}

/*
 * vote_generator_index
 */

TEST (vote_generator_index, basic_push_and_batch)
{
	nano::vote_generator_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };
	nano::block_hash hash1{ 10 };
	nano::block_hash hash2{ 20 };
	nano::block_hash hash3{ 30 };

	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_TRUE (index.push (root2, hash2, 0));
	ASSERT_TRUE (index.push (root3, hash3, 0));
	ASSERT_EQ (3, index.size ());

	auto batch = index.next_batch (2);
	ASSERT_EQ (2, batch.size ());
	ASSERT_EQ (1, index.size ());

	batch = index.next_batch (10);
	ASSERT_EQ (1, batch.size ());
	ASSERT_TRUE (index.empty ());
}

TEST (vote_generator_index, duplicate_rejected)
{
	nano::vote_generator_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::block_hash hash1{ 10 };

	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_FALSE (index.push (root1, hash1, 0)); // Same root+hash
	ASSERT_EQ (1, index.size ());
}

TEST (vote_generator_index, replacement_makes_old_stale)
{
	nano::vote_generator_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::block_hash hash_a{ 10 };
	nano::block_hash hash_b{ 20 };

	ASSERT_TRUE (index.push (root1, hash_a, 0));
	ASSERT_TRUE (index.push (root1, hash_b, 0)); // Replace
	ASSERT_EQ (1, index.size ());

	auto batch = index.next_batch (10);
	// Only the latest hash should appear (old one is stale and skipped)
	ASSERT_EQ (1, batch.size ());
	ASSERT_EQ (root1, batch[0].first);
	ASSERT_EQ (hash_b, batch[0].second);
}

TEST (vote_generator_index, max_size_per_bucket)
{
	nano::vote_generator_index index{ 2 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };
	nano::block_hash hash1{ 10 };
	nano::block_hash hash2{ 20 };
	nano::block_hash hash3{ 30 };

	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_TRUE (index.push (root2, hash2, 0));
	ASSERT_FALSE (index.push (root3, hash3, 0)); // Bucket full
	ASSERT_EQ (2, index.size ());

	// Different bucket still has room
	ASSERT_TRUE (index.push (root3, hash3, 1));
	ASSERT_EQ (3, index.size ());
}

TEST (vote_generator_index, fair_queue_across_buckets)
{
	nano::vote_generator_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };
	nano::qualified_root root4{ 4, 0 };
	nano::block_hash hash1{ 10 };
	nano::block_hash hash2{ 20 };
	nano::block_hash hash3{ 30 };
	nano::block_hash hash4{ 40 };

	// Two entries in bucket 0, two in bucket 1
	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_TRUE (index.push (root2, hash2, 0));
	ASSERT_TRUE (index.push (root3, hash3, 1));
	ASSERT_TRUE (index.push (root4, hash4, 1));

	// Fair queue should interleave between buckets
	auto batch = index.next_batch (4);
	ASSERT_EQ (4, batch.size ());
}

/*
 * vote_broadcast_index
 */

TEST (vote_broadcast_index, fifo_order_and_dedup)
{
	nano::vote_broadcast_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };

	ASSERT_TRUE (index.push (root1, make_permit (root1)));
	ASSERT_TRUE (index.push (root2, make_permit (root2)));
	ASSERT_TRUE (index.push (root3, make_permit (root3)));
	ASSERT_FALSE (index.push (root1, make_permit (root1))); // Duplicate rejected
	ASSERT_EQ (3, index.size ());

	auto batch = index.next_batch (2);
	ASSERT_EQ (2, batch.size ());
	ASSERT_EQ (root1, batch[0].qualified_root ());
	ASSERT_EQ (root2, batch[1].qualified_root ());
	ASSERT_EQ (1, index.size ());
}

TEST (vote_broadcast_index, erase_and_reinsert)
{
	nano::vote_broadcast_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };

	index.push (root1, make_permit (root1));
	index.push (root2, make_permit (root2));
	index.push (root3, make_permit (root3));

	ASSERT_TRUE (index.erase (root2));
	ASSERT_FALSE (index.erase (root2)); // Already gone
	ASSERT_EQ (2, index.size ());

	// Re-insert goes to the back
	ASSERT_TRUE (index.push (root2, make_permit (root2)));

	auto batch = index.next_batch (10);
	ASSERT_EQ (3, batch.size ());
	ASSERT_EQ (root1, batch[0].qualified_root ());
	ASSERT_EQ (root3, batch[1].qualified_root ());
	ASSERT_EQ (root2, batch[2].qualified_root ());
}

TEST (vote_broadcast_index, conflict_replaces_old_entry)
{
	nano::vote_broadcast_index index{ 1024 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::block_hash hash_a{ 10 };
	nano::block_hash hash_b{ 20 };

	ASSERT_TRUE (index.push (root1, make_permit (root1, hash_a)));
	ASSERT_TRUE (index.push (root2, make_permit (root2, hash_a)));
	ASSERT_EQ (2, index.size ());

	// Same root, different hash — old entry dropped, new one appended at back
	ASSERT_TRUE (index.push (root1, make_permit (root1, hash_b)));
	ASSERT_EQ (2, index.size ());

	auto batch = index.next_batch (10);
	ASSERT_EQ (2, batch.size ());
	ASSERT_EQ (root2, batch[0].qualified_root ());
	ASSERT_EQ (hash_a, batch[0].hash ());
	ASSERT_EQ (root1, batch[1].qualified_root ());
	ASSERT_EQ (hash_b, batch[1].hash ());
}

TEST (vote_broadcast_index, max_size)
{
	nano::vote_broadcast_index index{ 2 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::qualified_root root3{ 3, 0 };

	ASSERT_TRUE (index.push (root1, make_permit (root1)));
	ASSERT_TRUE (index.push (root2, make_permit (root2)));
	ASSERT_FALSE (index.push (root3, make_permit (root3))); // Full
	ASSERT_EQ (2, index.size ());

	// Draining one slot allows a new push
	auto batch = index.next_batch (1);
	ASSERT_EQ (1, batch.size ());
	ASSERT_TRUE (index.push (root3, make_permit (root3)));
	ASSERT_EQ (2, index.size ());
}

TEST (vote_broadcast_index, conflict_when_full)
{
	nano::vote_broadcast_index index{ 2 };

	nano::qualified_root root1{ 1, 0 };
	nano::qualified_root root2{ 2, 0 };
	nano::block_hash hash_a{ 10 };
	nano::block_hash hash_b{ 20 };

	ASSERT_TRUE (index.push (root1, make_permit (root1, hash_a)));
	ASSERT_TRUE (index.push (root2, make_permit (root2, hash_a)));

	// Conflict for existing root succeeds even when full (replaces, net size unchanged)
	ASSERT_TRUE (index.push (root1, make_permit (root1, hash_b)));
	ASSERT_EQ (2, index.size ());
}
