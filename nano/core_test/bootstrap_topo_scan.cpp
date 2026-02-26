#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/node/bootstrap/topo_scan.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
struct test_context
{
	nano::stats stats;
	nano::topo_scan_config config;
	nano::bootstrap::topo_scan topo_scan;

	explicit test_context (nano::topo_scan_config config_a = {}) :
		stats{ nano::default_logger () },
		config{ config_a },
		topo_scan{ config, stats }
	{
	}
};
}

TEST (bootstrap_topo_scan, construction)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// Initially indexing, no blocks pending
	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());
}

TEST (bootstrap_topo_scan, next_discovery)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// First next() should return a zero query (start from beginning)
	auto query = scan.next ();
	ASSERT_TRUE (query.has_value ());
	ASSERT_TRUE (query->is_zero ());
}

TEST (bootstrap_topo_scan, process_basic)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	auto query = scan.next ();
	ASSERT_TRUE (query.has_value ());

	// Create response with entries
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });

	// Process - short response since entries.size() < index_batch_size
	bool done = scan.process (query.value (), entries);
	ASSERT_TRUE (done); // Short response = end of topo space

	// Scan should no longer be indexing
	ASSERT_FALSE (scan.indexing ());

	// Blocks should be pending
	ASSERT_TRUE (scan.has_blocks_pending ());
}

TEST (bootstrap_topo_scan, process_advances_cursor)
{
	nano::topo_scan_config config;
	config.index_batch_size = 3; // Exactly match entries size to avoid "short response" detection
	test_context ctx{ config };
	auto & scan = ctx.topo_scan;

	auto query = scan.next ();
	ASSERT_TRUE (query.has_value ());
	ASSERT_TRUE (query->is_zero ());

	// Response with exactly batch_size entries = not done
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });

	bool done = scan.process (query.value (), entries);
	ASSERT_FALSE (done); // Full batch = more to fetch

	// Still indexing
	ASSERT_TRUE (scan.indexing ());

	// Next query should start from cursor position (last entry)
	auto next_query = scan.next ();
	ASSERT_TRUE (next_query.has_value ());
	ASSERT_EQ (next_query->start_index, 3);
	ASSERT_EQ (next_query->start_hash, nano::block_hash{ 300 });
}

TEST (bootstrap_topo_scan, next_blocks_basic)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// Feed some index entries
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (query.value (), entries);

	// Get blocks to fetch
	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 3);
	ASSERT_EQ (blocks[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks[1], nano::block_hash{ 200 });
	ASSERT_EQ (blocks[2], nano::block_hash{ 300 });

	// Calling again should return empty (all in-flight)
	auto blocks2 = scan.next_blocks (10);
	ASSERT_TRUE (blocks2.empty ());
}

TEST (bootstrap_topo_scan, next_blocks_respects_max_count)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// Feed some index entries
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	entries.push_back ({ 4, nano::block_hash{ 400 } });
	entries.push_back ({ 5, nano::block_hash{ 500 } });
	scan.process (query.value (), entries);

	// Request only 2 blocks
	auto blocks1 = scan.next_blocks (2);
	ASSERT_EQ (blocks1.size (), 2);
	ASSERT_EQ (blocks1[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks1[1], nano::block_hash{ 200 });

	// Next call should return the remaining 3
	auto blocks2 = scan.next_blocks (10);
	ASSERT_EQ (blocks2.size (), 3);
	ASSERT_EQ (blocks2[0], nano::block_hash{ 300 });
}

TEST (bootstrap_topo_scan, received_individual)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// Feed entries and get blocks
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (query.value (), entries);

	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 3);
	ASSERT_TRUE (scan.has_blocks_pending ());

	// Mark blocks received one by one
	scan.received (nano::block_hash{ 100 });
	ASSERT_TRUE (scan.has_blocks_pending ()); // Still 2 in-flight

	scan.received (nano::block_hash{ 200 });
	ASSERT_TRUE (scan.has_blocks_pending ()); // Still 1 in-flight

	scan.received (nano::block_hash{ 300 });
	ASSERT_FALSE (scan.has_blocks_pending ()); // All done
}

TEST (bootstrap_topo_scan, cooldown)
{
	nano::topo_scan_config config;
	config.cooldown = 250ms;
	config.index_batch_size = 3; // So responses aren't considered "short"
	test_context ctx{ config };
	auto & scan = ctx.topo_scan;

	// First call should succeed
	auto first = scan.next ();
	ASSERT_TRUE (first.has_value ());
	ASSERT_TRUE (first->is_zero ()); // Valid discovery query

	// Process a full batch so scan isn't done
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (first.value (), entries);

	// Next call should succeed (requests reset after process)
	auto second = scan.next ();
	ASSERT_TRUE (second.has_value ());
	ASSERT_FALSE (second->is_zero ());

	// Immediate second call should fail (request pending, in cooldown)
	auto third = scan.next ();
	ASSERT_FALSE (third.has_value ());

	// After cooldown, should succeed again
	std::this_thread::sleep_for (500ms);
	auto fourth = scan.next ();
	ASSERT_TRUE (fourth.has_value ());
	ASSERT_FALSE (fourth->is_zero ());
}

TEST (bootstrap_topo_scan, reset)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	// Feed entries
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	scan.process (query.value (), entries);
	ASSERT_FALSE (scan.indexing ()); // Done (short response)
	ASSERT_TRUE (scan.has_blocks_pending ());

	// Reset
	scan.reset ();

	// Should be back to initial state
	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());
	auto next_query = scan.next ();
	ASSERT_TRUE (next_query.has_value ());
	ASSERT_TRUE (next_query->is_zero ());
}

TEST (bootstrap_topo_scan, empty_response_signals_end)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	auto query = scan.next ();

	// Empty response
	std::deque<nano::bootstrap::topo_scan::index_entry> empty;
	bool done = scan.process (query.value (), empty);
	ASSERT_TRUE (done);
	ASSERT_FALSE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());

	// No more queries should be returned
	auto next_query = scan.next ();
	ASSERT_FALSE (next_query.has_value ());
}

TEST (bootstrap_topo_scan, duplicate_entries_ignored)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 100 } }); // Duplicate hash
	scan.process (query.value (), entries);

	// Should only have 2 unique blocks
	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 2);
}

TEST (bootstrap_topo_scan, block_retry)
{
	nano::topo_scan_config config;
	config.block_retry = 250ms;
	test_context ctx{ config };
	auto & scan = ctx.topo_scan;

	// Feed entries
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (query.value (), entries);

	// Get all blocks (marks them in-flight)
	auto blocks1 = scan.next_blocks (10);
	ASSERT_EQ (blocks1.size (), 3);

	// Immediately requesting again should return empty (all in-flight, not timed out)
	auto blocks2 = scan.next_blocks (10);
	ASSERT_TRUE (blocks2.empty ());

	// Mark one as received
	scan.received (nano::block_hash{ 200 });

	// Still empty (remaining 2 in-flight but not timed out)
	auto blocks3 = scan.next_blocks (10);
	ASSERT_TRUE (blocks3.empty ());

	// Wait for retry period to expire
	std::this_thread::sleep_for (500ms);

	// Now the 2 non-received in-flight blocks should be retried
	auto blocks4 = scan.next_blocks (10);
	ASSERT_EQ (blocks4.size (), 2);
	ASSERT_EQ (blocks4[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks4[1], nano::block_hash{ 300 });
}

TEST (bootstrap_topo_scan, backpressure_index)
{
	nano::topo_scan_config config;
	config.index_batch_size = 3;
	config.max_blocks_queued = 5; // Index pauses when 5 blocks outstanding
	test_context ctx{ config };
	auto & scan = ctx.topo_scan;

	// First index batch: 3 entries
	auto q1 = scan.next ();
	ASSERT_TRUE (q1.has_value ());
	std::deque<nano::bootstrap::topo_scan::index_entry> entries1;
	entries1.push_back ({ 1, nano::block_hash{ 100 } });
	entries1.push_back ({ 2, nano::block_hash{ 200 } });
	entries1.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (q1.value (), entries1);
	ASSERT_EQ (scan.count_outstanding (), 3);

	// Second index batch: 3 more entries (total would be 6, over limit of 5)
	auto q2 = scan.next ();
	ASSERT_TRUE (q2.has_value ());
	std::deque<nano::bootstrap::topo_scan::index_entry> entries2;
	entries2.push_back ({ 4, nano::block_hash{ 400 } });
	entries2.push_back ({ 5, nano::block_hash{ 500 } });
	entries2.push_back ({ 6, nano::block_hash{ 600 } });
	scan.process (q2.value (), entries2);
	ASSERT_EQ (scan.count_outstanding (), 6);

	// Index should now pause (6 >= 5)
	auto q3 = scan.next ();
	ASSERT_FALSE (q3.has_value ());

	// Complete some blocks to get under limit
	scan.next_blocks (3);
	scan.received (nano::block_hash{ 100 });
	scan.received (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_outstanding (), 4);

	// Index can resume
	auto q4 = scan.next ();
	ASSERT_TRUE (q4.has_value ());
}

TEST (bootstrap_topo_scan, backpressure_blocks)
{
	nano::topo_scan_config config;
	config.max_blocks_outstanding = 2; // Block fetching pauses when 2 in-flight
	test_context ctx{ config };
	auto & scan = ctx.topo_scan;

	// Feed 5 entries
	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	entries.push_back ({ 4, nano::block_hash{ 400 } });
	entries.push_back ({ 5, nano::block_hash{ 500 } });
	scan.process (query.value (), entries);

	// First batch: only 2 (capped by max_blocks_outstanding)
	auto blocks1 = scan.next_blocks (10);
	ASSERT_EQ (blocks1.size (), 2);

	// Already at limit, no more
	auto blocks2 = scan.next_blocks (10);
	ASSERT_TRUE (blocks2.empty ());

	// Complete one, freeing capacity for one more
	scan.received (nano::block_hash{ 100 });
	auto blocks3 = scan.next_blocks (10);
	ASSERT_EQ (blocks3.size (), 1);
}

TEST (bootstrap_topo_scan, container_info)
{
	test_context ctx{};
	auto & scan = ctx.topo_scan;

	auto query = scan.next ();
	std::deque<nano::bootstrap::topo_scan::index_entry> entries;
	entries.push_back ({ 1, nano::block_hash{ 100 } });
	entries.push_back ({ 2, nano::block_hash{ 200 } });
	entries.push_back ({ 3, nano::block_hash{ 300 } });
	scan.process (query.value (), entries);

	// Get some blocks and mark one as received
	scan.next_blocks (2);
	scan.received (nano::block_hash{ 100 });

	auto info = scan.container_info ();
	// Should have meaningful data
	ASSERT_GT (info.entries ().size (), 0);
}
