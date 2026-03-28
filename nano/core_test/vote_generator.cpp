#include <nano/node/vote_generator.hpp>
#include <nano/secure/voting_policy.hpp>
#include <nano/test_common/random.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <set>

/*
 * vote_generator_index
 */

TEST (vote_generator_index, empty_state)
{
	nano::vote_generator_index index{ 128 };
	ASSERT_TRUE (index.empty ());
	ASSERT_EQ (index.size (), 0);

	// next_batch on empty returns nothing
	auto batch = index.next_batch (10);
	ASSERT_TRUE (batch.empty ());

	// next_batch(0) after push returns nothing but does not consume
	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	ASSERT_TRUE (index.push (root1, nano::block_hash{ 10 }, 0));
	auto batch0 = index.next_batch (0);
	ASSERT_TRUE (batch0.empty ());
	ASSERT_EQ (index.size (), 1);
}

TEST (vote_generator_index, push_and_retrieve)
{
	nano::vote_generator_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };
	auto hash3 = nano::block_hash{ 30 };

	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_TRUE (index.push (root2, hash2, 0));
	ASSERT_TRUE (index.push (root3, hash3, 0));
	ASSERT_EQ (index.size (), 3);
	ASSERT_FALSE (index.empty ());

	// Request more than available
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 3);

	// Verify all entries present
	auto find = [&] (nano::qualified_root const & root) {
		return std::find_if (batch.begin (), batch.end (), [&] (auto const & e) { return e.first == root; });
	};
	auto it1 = find (root1);
	ASSERT_NE (it1, batch.end ());
	ASSERT_EQ (it1->second, hash1);
	auto it2 = find (root2);
	ASSERT_NE (it2, batch.end ());
	ASSERT_EQ (it2->second, hash2);
	auto it3 = find (root3);
	ASSERT_NE (it3, batch.end ());
	ASSERT_EQ (it3->second, hash3);

	ASSERT_TRUE (index.empty ());
	ASSERT_EQ (index.size (), 0);
}

TEST (vote_generator_index, duplicate_detection)
{
	nano::vote_generator_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto hash1 = nano::block_hash{ 10 };

	ASSERT_TRUE (index.push (root1, hash1, 0));
	ASSERT_EQ (index.size (), 1);

	// Exact duplicate, same bucket
	ASSERT_FALSE (index.push (root1, hash1, 0));
	ASSERT_EQ (index.size (), 1);

	// Exact duplicate, different bucket — dedup is by root, not bucket
	ASSERT_FALSE (index.push (root1, hash1, 1));
	ASSERT_EQ (index.size (), 1);

	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 1);
	ASSERT_EQ (batch[0].first, root1);
	ASSERT_EQ (batch[0].second, hash1);
}

TEST (vote_generator_index, replacement_and_staleness)
{
	nano::vote_generator_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto hash_a = nano::block_hash{ 10 };
	auto hash_b = nano::block_hash{ 20 };
	auto hash_c = nano::block_hash{ 30 };

	// Push and replace twice
	ASSERT_TRUE (index.push (root1, hash_a, 0));
	ASSERT_TRUE (index.push (root1, hash_b, 0));
	ASSERT_EQ (index.size (), 1);
	ASSERT_TRUE (index.push (root1, hash_c, 0));
	ASSERT_EQ (index.size (), 1);

	// Push a separate root
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto hash_d = nano::block_hash{ 40 };
	ASSERT_TRUE (index.push (root2, hash_d, 0));
	ASSERT_EQ (index.size (), 2);

	// Extract: stale entries for hash_a and hash_b should be skipped
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 2);

	auto find = [&] (nano::qualified_root const & root) {
		return std::find_if (batch.begin (), batch.end (), [&] (auto const & e) { return e.first == root; });
	};
	auto it1 = find (root1);
	ASSERT_NE (it1, batch.end ());
	ASSERT_EQ (it1->second, hash_c);
	auto it2 = find (root2);
	ASSERT_NE (it2, batch.end ());
	ASSERT_EQ (it2->second, hash_d);

	ASSERT_TRUE (index.empty ());
}

TEST (vote_generator_index, queue_full)
{
	nano::vote_generator_index index{ 2 }; // max 2 per bucket

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };

	// Fill bucket 0
	ASSERT_TRUE (index.push (root1, nano::block_hash{ 10 }, 0));
	ASSERT_TRUE (index.push (root2, nano::block_hash{ 20 }, 0));
	ASSERT_EQ (index.size (), 2);

	// Bucket 0 full
	ASSERT_FALSE (index.push (root3, nano::block_hash{ 30 }, 0));
	ASSERT_EQ (index.size (), 2);

	// Different bucket still accepts
	ASSERT_TRUE (index.push (root3, nano::block_hash{ 30 }, 1));
	ASSERT_EQ (index.size (), 3);

	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 3);
	ASSERT_TRUE (index.empty ());
}

TEST (vote_generator_index, multi_bucket_fairness)
{
	nano::vote_generator_index index{ 128 };

	// 3 entries in bucket 0
	for (uint64_t i = 1; i <= 3; ++i)
	{
		auto root = nano::qualified_root{ nano::root{ i }, nano::block_hash{ i } };
		ASSERT_TRUE (index.push (root, nano::block_hash{ i * 10 }, 0));
	}
	// 3 entries in bucket 1
	for (uint64_t i = 4; i <= 6; ++i)
	{
		auto root = nano::qualified_root{ nano::root{ i }, nano::block_hash{ i } };
		ASSERT_TRUE (index.push (root, nano::block_hash{ i * 10 }, 1));
	}
	ASSERT_EQ (index.size (), 6);

	// Extract 4: fair round-robin with priority=1 should give 2 from each bucket
	auto batch = index.next_batch (4);
	ASSERT_EQ (batch.size (), 4);

	// Roots 1-3 were pushed to bucket 0, roots 4-6 to bucket 1
	std::set<nano::qualified_root> bucket0_roots, bucket1_roots;
	for (uint64_t i = 1; i <= 3; ++i)
	{
		bucket0_roots.insert (nano::qualified_root{ nano::root{ i }, nano::block_hash{ i } });
	}
	for (uint64_t i = 4; i <= 6; ++i)
	{
		bucket1_roots.insert (nano::qualified_root{ nano::root{ i }, nano::block_hash{ i } });
	}
	int bucket0_count = 0;
	int bucket1_count = 0;
	for (auto const & [root, hash] : batch)
	{
		if (bucket0_roots.count (root))
		{
			++bucket0_count;
		}
		else if (bucket1_roots.count (root))
		{
			++bucket1_count;
		}
	}
	ASSERT_EQ (bucket0_count, 2);
	ASSERT_EQ (bucket1_count, 2);

	// Extract remaining
	auto rest = index.next_batch (10);
	ASSERT_EQ (rest.size (), 2);
	ASSERT_TRUE (index.empty ());
}

TEST (vote_generator_index, replacement_when_bucket_full)
{
	nano::vote_generator_index index{ 2 }; // max 2 per bucket

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto hash_a = nano::block_hash{ 10 };
	auto hash_b = nano::block_hash{ 20 };
	auto hash_c = nano::block_hash{ 30 };

	// Fill bucket 0
	ASSERT_TRUE (index.push (root1, hash_a, 0));
	ASSERT_TRUE (index.push (root2, hash_b, 0));

	// Replace root1: dedup updates to hash_c, but queue.push fails (bucket full with stale + valid entries)
	ASSERT_FALSE (index.push (root1, hash_c, 0));

	// Dedup was updated despite push failure: root1 now maps to hash_c
	// Queue still has: root1/hash_a(stale), root2/hash_b(valid)
	ASSERT_EQ (index.size (), 2);

	// Extract: root1/hash_a is stale (dedup says hash_c) → skipped; root2/hash_b → valid
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 1);
	ASSERT_EQ (batch[0].first, root2);
	ASSERT_EQ (batch[0].second, hash_b);

	// root1 is a zombie: exists in dedup but not in queue
	ASSERT_EQ (index.size (), 1);
	ASSERT_FALSE (index.empty ());
	auto empty_batch = index.next_batch (10);
	ASSERT_TRUE (empty_batch.empty ());
}

/*
 * vote_broadcast_index
 */

TEST (vote_broadcast_index, empty_state)
{
	nano::vote_broadcast_index index{ 128 };
	ASSERT_TRUE (index.empty ());
	ASSERT_EQ (index.size (), 0);

	auto batch = index.next_batch (10);
	ASSERT_TRUE (batch.empty ());

	ASSERT_FALSE (index.erase (nano::qualified_root{ 1 }));
}

TEST (vote_broadcast_index, push_and_fifo_extraction)
{
	nano::vote_broadcast_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };
	auto hash3 = nano::block_hash{ 30 };

	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root3, nano::vote_permit::dummy (root3, hash3, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 3);

	// Partial extraction — FIFO order
	auto batch1 = index.next_batch (2);
	ASSERT_EQ (batch1.size (), 2);
	ASSERT_EQ (batch1[0].hash (), hash1);
	ASSERT_EQ (batch1[1].hash (), hash2);
	ASSERT_EQ (index.size (), 1);

	// Request more than remaining
	auto batch2 = index.next_batch (10);
	ASSERT_EQ (batch2.size (), 1);
	ASSERT_EQ (batch2[0].hash (), hash3);
	ASSERT_TRUE (index.empty ());
}

TEST (vote_broadcast_index, duplicate_detection)
{
	nano::vote_broadcast_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };

	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 1);

	// Exact duplicate
	ASSERT_FALSE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 1);

	// Push another root, verify FIFO: root1 still before root2
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2, nano::vote_type::normal)));
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 2);
	ASSERT_EQ (batch[0].hash (), hash1);
	ASSERT_EQ (batch[1].hash (), hash2);
}

TEST (vote_broadcast_index, replacement_reorders_to_back)
{
	nano::vote_broadcast_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };
	auto hash3 = nano::block_hash{ 30 };
	auto hash1_new = nano::block_hash{ 11 };

	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root3, nano::vote_permit::dummy (root3, hash3, nano::vote_type::normal)));

	// Replace root1 with different hash — moves to back
	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1_new, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 3);

	// FIFO: root2, root3, root1(new)
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 3);
	ASSERT_EQ (batch[0].qualified_root (), root2);
	ASSERT_EQ (batch[0].hash (), hash2);
	ASSERT_EQ (batch[1].qualified_root (), root3);
	ASSERT_EQ (batch[1].hash (), hash3);
	ASSERT_EQ (batch[2].qualified_root (), root1);
	ASSERT_EQ (batch[2].hash (), hash1_new);
}

TEST (vote_broadcast_index, max_size_enforcement)
{
	nano::vote_broadcast_index index{ 3 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };
	auto root4 = nano::qualified_root{ nano::root{ 4 }, nano::block_hash{ 4 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };
	auto hash3 = nano::block_hash{ 30 };
	auto hash4 = nano::block_hash{ 40 };
	auto hash2_new = nano::block_hash{ 21 };

	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root3, nano::vote_permit::dummy (root3, hash3, nano::vote_type::normal)));

	// Full — new root rejected
	ASSERT_FALSE (index.push (root4, nano::vote_permit::dummy (root4, hash4, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 3);

	// Replacement at capacity works (erase + push_back keeps size at max)
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2_new, nano::vote_type::normal)));
	ASSERT_EQ (index.size (), 3);

	// FIFO after replacement: root1, root3, root2(new)
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 3);
	ASSERT_EQ (batch[0].qualified_root (), root1);
	ASSERT_EQ (batch[1].qualified_root (), root3);
	ASSERT_EQ (batch[2].qualified_root (), root2);
	ASSERT_EQ (batch[2].hash (), hash2_new);
}

TEST (vote_broadcast_index, erase)
{
	nano::vote_broadcast_index index{ 128 };

	auto root1 = nano::qualified_root{ nano::root{ 1 }, nano::block_hash{ 1 } };
	auto root2 = nano::qualified_root{ nano::root{ 2 }, nano::block_hash{ 2 } };
	auto root3 = nano::qualified_root{ nano::root{ 3 }, nano::block_hash{ 3 } };
	auto hash1 = nano::block_hash{ 10 };
	auto hash2 = nano::block_hash{ 20 };
	auto hash3 = nano::block_hash{ 30 };

	ASSERT_TRUE (index.push (root1, nano::vote_permit::dummy (root1, hash1, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root2, nano::vote_permit::dummy (root2, hash2, nano::vote_type::normal)));
	ASSERT_TRUE (index.push (root3, nano::vote_permit::dummy (root3, hash3, nano::vote_type::normal)));

	// Erase middle entry
	ASSERT_TRUE (index.erase (root2));
	ASSERT_EQ (index.size (), 2);

	// Non-existent root
	ASSERT_FALSE (index.erase (nano::qualified_root{ nano::root{ 99 }, nano::block_hash{ 99 } }));
	ASSERT_EQ (index.size (), 2);

	// Double erase
	ASSERT_FALSE (index.erase (root2));

	// FIFO: root1, root3
	auto batch = index.next_batch (10);
	ASSERT_EQ (batch.size (), 2);
	ASSERT_EQ (batch[0].qualified_root (), root1);
	ASSERT_EQ (batch[1].qualified_root (), root3);
}
