#include <nano/lib/blocks.hpp>
#include <nano/node/scheduler/priority_pool.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (priority_pool, basic_operations)
{
	nano::scheduler::priority_pool pool{ 10, 3 };

	// Test empty pool
	ASSERT_TRUE (pool.empty ());
	ASSERT_EQ (pool.size (), 0);
	ASSERT_TRUE (pool.empty (1));
	ASSERT_EQ (pool.size (1), 0);
	ASSERT_FALSE (pool.any (1));

	// Create and push single block
	auto block1 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block1, 1, 100));
	ASSERT_FALSE (pool.empty ());
	ASSERT_EQ (pool.size (), 1);
	ASSERT_EQ (pool.size (1), 1);
	ASSERT_TRUE (pool.any (1));
	ASSERT_TRUE (pool.contains (block1->hash ()));

	// Pop the block
	auto result = pool.pop (1);
	ASSERT_TRUE (result.has_value ());
	ASSERT_EQ (result->block, block1);
	ASSERT_EQ (result->bucket, 1);
	ASSERT_EQ (result->priority, 100);
	ASSERT_TRUE (pool.empty ());
	ASSERT_FALSE (pool.contains (block1->hash ()));

	// Push multiple blocks to same bucket
	auto block2 = nano::test::random_block ();
	auto block3 = nano::test::random_block ();

	ASSERT_TRUE (pool.push (block1, 1, 100));
	ASSERT_TRUE (pool.push (block2, 1, 200));
	ASSERT_TRUE (pool.push (block3, 1, 150));
	ASSERT_EQ (pool.size (), 3);
	ASSERT_EQ (pool.size (1), 3);

	// Pop should return best priority (lowest value) first
	result = pool.pop (1);
	ASSERT_TRUE (result.has_value ());
	ASSERT_EQ (result->block, block1);
	ASSERT_EQ (result->priority, 100);

	result = pool.pop (1);
	ASSERT_TRUE (result.has_value ());
	ASSERT_EQ (result->block, block3);
	ASSERT_EQ (result->priority, 150);

	result = pool.pop (1);
	ASSERT_TRUE (result.has_value ());
	ASSERT_EQ (result->block, block2);
	ASSERT_EQ (result->priority, 200);

	// Empty bucket
	result = pool.pop (1);
	ASSERT_FALSE (result.has_value ());
	ASSERT_TRUE (pool.empty ());
}

TEST (priority_pool, priority_ordering)
{
	nano::scheduler::priority_pool pool{ 20, 5 };

	// Add blocks with different priorities to same bucket
	auto block1 = nano::test::random_block ();
	auto block2 = nano::test::random_block ();
	auto block3 = nano::test::random_block ();
	auto block4 = nano::test::random_block ();
	auto block5 = nano::test::random_block ();

	ASSERT_TRUE (pool.push (block1, 1, 500));
	ASSERT_TRUE (pool.push (block2, 1, 100));
	ASSERT_TRUE (pool.push (block3, 1, 300));
	ASSERT_TRUE (pool.push (block4, 1, 200));
	ASSERT_TRUE (pool.push (block5, 1, 400));

	// Test top() returns best priority without removing
	auto top_result = pool.top (1);
	ASSERT_TRUE (top_result.has_value ());
	ASSERT_EQ (top_result->block, block2);
	ASSERT_EQ (top_result->priority, 100);
	ASSERT_EQ (pool.size (), 5); // Size unchanged

	// Pop all blocks and verify order (lower priority value = better)
	ASSERT_EQ (pool.pop (1)->block, block2); // 100
	ASSERT_EQ (pool.pop (1)->block, block4); // 200
	ASSERT_EQ (pool.pop (1)->block, block3); // 300
	ASSERT_EQ (pool.pop (1)->block, block5); // 400
	ASSERT_EQ (pool.pop (1)->block, block1); // 500

	// Test ordering across multiple buckets with multiple entries per bucket
	auto block6 = nano::test::random_block ();
	auto block7 = nano::test::random_block ();
	auto block8 = nano::test::random_block ();
	auto block9 = nano::test::random_block ();
	auto block10 = nano::test::random_block ();
	auto block11 = nano::test::random_block ();
	auto block12 = nano::test::random_block ();
	auto block13 = nano::test::random_block ();
	auto block14 = nano::test::random_block ();

	// Bucket 1 - already empty from previous pops, add 3 blocks
	ASSERT_TRUE (pool.push (block6, 1, 150));
	ASSERT_TRUE (pool.push (block7, 1, 250));
	ASSERT_TRUE (pool.push (block8, 1, 50));

	// Bucket 2 - add 3 blocks
	ASSERT_TRUE (pool.push (block9, 2, 300));
	ASSERT_TRUE (pool.push (block10, 2, 100));
	ASSERT_TRUE (pool.push (block11, 2, 200));

	// Bucket 3 - add 3 blocks
	ASSERT_TRUE (pool.push (block12, 3, 450));
	ASSERT_TRUE (pool.push (block13, 3, 350));
	ASSERT_TRUE (pool.push (block14, 3, 550));

	// Each bucket maintains its own ordering
	ASSERT_EQ (pool.top (1)->priority, 50); // block8
	ASSERT_EQ (pool.top (2)->priority, 100); // block10
	ASSERT_EQ (pool.top (3)->priority, 350); // block13

	// Verify complete ordering in bucket 1
	ASSERT_EQ (pool.pop (1)->block, block8); // 50
	ASSERT_EQ (pool.pop (1)->block, block6); // 150
	ASSERT_EQ (pool.pop (1)->block, block7); // 250

	// Verify complete ordering in bucket 2
	ASSERT_EQ (pool.pop (2)->block, block10); // 100
	ASSERT_EQ (pool.pop (2)->block, block11); // 200
	ASSERT_EQ (pool.pop (2)->block, block9); // 300

	// Verify complete ordering in bucket 3
	ASSERT_EQ (pool.pop (3)->block, block13); // 350
	ASSERT_EQ (pool.pop (3)->block, block12); // 450
	ASSERT_EQ (pool.pop (3)->block, block14); // 550

	// All buckets should be empty now
	ASSERT_TRUE (pool.empty ());
	ASSERT_FALSE (pool.top (1).has_value ());
	ASSERT_FALSE (pool.top (2).has_value ());
	ASSERT_FALSE (pool.top (3).has_value ());
}

TEST (priority_pool, duplicate_handling)
{
	nano::scheduler::priority_pool pool{ 10, 3 };

	auto block1 = nano::test::random_block ();
	auto block2 = nano::test::random_block ();

	// First insertion should succeed
	ASSERT_TRUE (pool.push (block1, 1, 100));
	ASSERT_EQ (pool.size (), 1);
	ASSERT_TRUE (pool.contains (block1->hash ()));

	// Duplicate insertion should fail (same hash)
	ASSERT_FALSE (pool.push (block1, 1, 200)); // Different priority, same block
	ASSERT_FALSE (pool.push (block1, 2, 100)); // Different bucket, same block
	ASSERT_EQ (pool.size (), 1);

	// Different block should succeed
	ASSERT_TRUE (pool.push (block2, 1, 150));
	ASSERT_EQ (pool.size (), 2);

	// Pop block1 and verify we can re-add it
	auto result = pool.pop (1);
	ASSERT_EQ (result->block, block1);
	ASSERT_FALSE (pool.contains (block1->hash ()));
	ASSERT_EQ (pool.size (), 1);

	// Re-adding should now succeed
	ASSERT_TRUE (pool.push (block1, 1, 300));
	ASSERT_TRUE (pool.contains (block1->hash ()));
	ASSERT_EQ (pool.size (), 2);

	// Verify ordering after re-add (block2 has priority 150, block1 has 300)
	result = pool.pop (1);
	ASSERT_EQ (result->block, block2);
	ASSERT_EQ (result->priority, 150);
	result = pool.pop (1);
	ASSERT_EQ (result->block, block1);
	ASSERT_EQ (result->priority, 300);
}

TEST (priority_pool, capacity_and_eviction)
{
	nano::scheduler::priority_pool pool{ 5, 2 }; // Small pool for testing

	// Fill pool to max_size
	auto block1 = nano::test::random_block ();
	auto block2 = nano::test::random_block ();
	auto block3 = nano::test::random_block ();
	auto block4 = nano::test::random_block ();
	auto block5 = nano::test::random_block ();

	ASSERT_TRUE (pool.push (block1, 1, 100));
	ASSERT_TRUE (pool.push (block2, 1, 200));
	ASSERT_TRUE (pool.push (block3, 1, 300));
	ASSERT_TRUE (pool.push (block4, 2, 400));
	ASSERT_TRUE (pool.push (block5, 2, 500));
	ASSERT_EQ (pool.size (), 5);

	// Add block when at capacity - should trigger eviction
	auto block6 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block6, 1, 150)); // Better than some existing blocks

	// Pool should stay at max_size after eviction
	ASSERT_EQ (pool.size (), 5);

	// Bucket 1 exceeded reserved (had 3, reserved is 2), so worst block from bucket 1 should be evicted
	ASSERT_TRUE (pool.contains (block1->hash ())); // 100 - best
	ASSERT_TRUE (pool.contains (block6->hash ())); // 150 - added
	ASSERT_TRUE (pool.contains (block2->hash ())); // 200
	ASSERT_FALSE (pool.contains (block3->hash ())); // 300 - evicted (worst in bucket 1)
	ASSERT_TRUE (pool.contains (block4->hash ())); // 400
	ASSERT_TRUE (pool.contains (block5->hash ())); // 500

	// Test self-eviction scenario - add worse block to bucket that's over reserved
	auto block7 = nano::test::random_block ();
	ASSERT_FALSE (pool.push (block7, 1, 250)); // Worse than all blocks in bucket 1, and bucket 1 is over reserved
	ASSERT_EQ (pool.size (), 5);
	ASSERT_FALSE (pool.contains (block7->hash ()));

	// Add very good block - should succeed and evict worst
	auto block8 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block8, 1, 50)); // Best priority
	ASSERT_EQ (pool.size (), 5);
	ASSERT_TRUE (pool.contains (block8->hash ()));
	ASSERT_FALSE (pool.contains (block2->hash ())); // 200 was worst in bucket 1, now evicted
}

TEST (priority_pool, bucket_reserved_capacity)
{
	nano::scheduler::priority_pool pool{ 6, 2 }; // Max 6, reserved 2 per bucket

	// Fill bucket 1 to reserved capacity
	auto block1 = nano::test::random_block ();
	auto block2 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block1, 1, 100));
	ASSERT_TRUE (pool.push (block2, 1, 200));
	ASSERT_EQ (pool.size (1), 2);

	// Fill bucket 2 to reserved capacity
	auto block3 = nano::test::random_block ();
	auto block4 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block3, 2, 300));
	ASSERT_TRUE (pool.push (block4, 2, 400));
	ASSERT_EQ (pool.size (2), 2);

	// Fill bucket 3 to reserved capacity
	auto block5 = nano::test::random_block ();
	auto block6 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block5, 3, 500));
	ASSERT_TRUE (pool.push (block6, 3, 600));
	ASSERT_EQ (pool.size (3), 2);
	ASSERT_EQ (pool.size (), 6); // At max capacity

	// All buckets at reserved - pool should overflow
	auto block7 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block7, 4, 700)); // New bucket
	ASSERT_EQ (pool.size (), 7); // Exceeds max_size!
	ASSERT_EQ (pool.size (4), 1);

	// Add to existing bucket that's at reserved - should evict worst from that bucket
	auto block8 = nano::test::random_block ();
	ASSERT_TRUE (pool.push (block8, 1, 50)); // Better priority than existing in bucket 1
	ASSERT_EQ (pool.size (), 7); // Stays same size
	ASSERT_FALSE (pool.contains (block2->hash ())); // block2 (200) was worst in bucket 1
	ASSERT_TRUE (pool.contains (block1->hash ())); // block1 (100) remains
	ASSERT_TRUE (pool.contains (block8->hash ())); // block8 (50) added
	ASSERT_EQ (pool.size (1), 2);

	// Now bucket 1 is at reserved, attempt to add worse block should fail
	auto block9 = nano::test::random_block ();
	ASSERT_FALSE (pool.push (block9, 1, 150));
	ASSERT_EQ (pool.size (), 7);
	ASSERT_EQ (pool.size (1), 2);

	// Test direct evict() method
	auto evicted = pool.evict (1);
	ASSERT_EQ (evicted, block1->hash ()); // block1 (100) was worst in bucket 1
	ASSERT_FALSE (pool.contains (block1->hash ()));
	ASSERT_EQ (pool.size (1), 1);
	ASSERT_EQ (pool.size (), 6);

	// Evict from empty bucket should return zero hash
	auto empty_evict = pool.evict (99);
	ASSERT_EQ (empty_evict, nano::block_hash{ 0 });
}

TEST (priority_pool, same_priority_hash_ordering)
{
	nano::scheduler::priority_pool pool{ 10, 5 };

	// Create multiple blocks - they will have different hashes
	auto block1 = nano::test::random_block ();
	auto block2 = nano::test::random_block ();
	auto block3 = nano::test::random_block ();
	auto block4 = nano::test::random_block ();
	auto block5 = nano::test::random_block ();

	// Add all blocks with identical bucket and priority
	constexpr nano::bucket_index bucket = 1;
	constexpr nano::priority_timestamp priority = 100;

	ASSERT_TRUE (pool.push (block1, bucket, priority));
	ASSERT_TRUE (pool.push (block2, bucket, priority));
	ASSERT_TRUE (pool.push (block3, bucket, priority));
	ASSERT_TRUE (pool.push (block4, bucket, priority));
	ASSERT_TRUE (pool.push (block5, bucket, priority));

	ASSERT_EQ (pool.size (), 5);
	ASSERT_EQ (pool.size (bucket), 5);

	// Collect all blocks and their hashes
	std::vector<std::pair<nano::block_hash, std::shared_ptr<nano::block>>> blocks_with_hashes;
	blocks_with_hashes.push_back ({ block1->hash (), block1 });
	blocks_with_hashes.push_back ({ block2->hash (), block2 });
	blocks_with_hashes.push_back ({ block3->hash (), block3 });
	blocks_with_hashes.push_back ({ block4->hash (), block4 });
	blocks_with_hashes.push_back ({ block5->hash (), block5 });

	// Sort by hash to determine expected order
	std::sort (blocks_with_hashes.begin (), blocks_with_hashes.end (), [] (auto const & a, auto const & b) {
		return a.first < b.first;
	});

	// Pop all blocks and verify they come out in hash order
	for (auto const & [hash, block] : blocks_with_hashes)
	{
		auto result = pool.pop (bucket);
		ASSERT_TRUE (result.has_value ());
		ASSERT_EQ (result->block->hash (), hash);
		ASSERT_EQ (result->block, block);
		ASSERT_EQ (result->priority, priority);
	}

	// Pool should be empty
	ASSERT_TRUE (pool.empty ());
	ASSERT_FALSE (pool.pop (bucket).has_value ());
}