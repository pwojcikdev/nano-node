#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
// Default test config: consideration_count=1 lets a head finalize after a
// single response; head_count=2 gives one spear + one repair head.
nano::topo_scan_config default_config ()
{
	nano::topo_scan_config cfg{};
	cfg.consideration_count = 1;
	cfg.head_count = 2;
	return cfg;
}

nano::topo_key key (uint64_t height, uint64_t hash_value)
{
	return nano::topo_key{ height, nano::block_hash{ hash_value } };
}

std::deque<nano::topo_key> entries (std::initializer_list<nano::topo_key> keys)
{
	return std::deque<nano::topo_key>{ keys };
}

// Drive a member all the way to `submitted` (discovered -> fetched -> received
// -> submitted), so a test can then resolve / gap / terminate it.
void submit_member (nano::bootstrap::topo_scan_index & scan, uint64_t hash_value)
{
	scan.next_blocks (1000);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ hash_value }, stub);
	scan.next_submit (1000);
}
}

TEST (bootstrap_topo_scan, construction)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_EQ (scan.head_count (), 2);
	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.caught_up ());
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
}

// The spear's first query offers the genesis cursor; processing a response
// queues members and emits a chunk, but the reported cursor (watermark) only
// moves on confirmation.
TEST (bootstrap_topo_scan, spear_discovery)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next (0);
	ASSERT_TRUE (pos);
	ASSERT_EQ (*pos, nano::topo_key{});

	ASSERT_TRUE (scan.process (0, *pos, entries ({ key (1, 100), key (2, 200) })));
	ASSERT_EQ (scan.count_pending (), 2);
	ASSERT_EQ (scan.count_members (), 2);
	ASSERT_EQ (scan.count_chunks (), 1);
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // nothing confirmed yet
}

// A re-scan that re-encounters an already-tracked key does not add a duplicate
// member — only previously-unseen keys are queued.
TEST (bootstrap_topo_scan, dedup_on_rescan)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100) }));
	ASSERT_EQ (scan.count_members (), 1);

	// A second head re-scans from genesis: 100 is already tracked (deduped), 200 is new.
	scan.process (1, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	ASSERT_EQ (scan.count_members (), 2);
}

// Full pipeline: discovered -> fetched -> received -> submitted -> in_ledger,
// and confirming both members prunes them and advances the watermark.
TEST (bootstrap_topo_scan, fetch_submit_confirm)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));

	auto to_fetch = scan.next_blocks (10);
	ASSERT_EQ (to_fetch.size (), 2);
	ASSERT_EQ (scan.count_in_flight (), 2);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	ASSERT_EQ (scan.count_received (), 2);

	ASSERT_EQ (scan.next_submit (10).size (), 2);
	ASSERT_EQ (scan.count_submitted (), 2);

	scan.block_indexed (nano::block_hash{ 100 });
	scan.block_indexed (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_members (), 0); // pruned
	ASSERT_EQ (scan.cursor (), key (2, 200)); // watermark advanced to the tip of the prefix
}

// Submission gate: an un-fetched member at the front stops the contiguous
// drain — its dependents must not be submitted before it.
TEST (bootstrap_topo_scan, submit_gate_stops_at_unfetched)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200), key (3, 300) }));
	scan.next_blocks (10); // all -> in_flight

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub); // 100 still in_flight

	// Front (100) is un-fetched -> nothing submits.
	ASSERT_TRUE (scan.next_submit (10).empty ());

	// Once the front is fetched, the whole contiguous prefix submits in order.
	scan.block_received (nano::block_hash{ 100 }, stub);
	ASSERT_EQ (scan.next_submit (10).size (), 3);
}

// The watermark only advances over a contiguous confirmed prefix: a confirmed
// member above an unresolved one is held (not pruned) until the gap fills.
TEST (bootstrap_topo_scan, watermark_waits_for_contiguous)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	scan.next_blocks (10);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.next_submit (10);

	// Confirm the HIGH member first: watermark can't advance past the low one.
	scan.block_indexed (nano::block_hash{ 200 });
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
	ASSERT_EQ (scan.count_members (), 2); // both held (100 submitted, 200 in_ledger)

	// Confirm the low member: prefix becomes contiguous, both prune.
	scan.block_indexed (nano::block_hash{ 100 });
	ASSERT_EQ (scan.cursor (), key (2, 200));
	ASSERT_EQ (scan.count_members (), 0);
}

// A gap retains the member and records its key; a repair head homes its cursor
// below the gap height.
TEST (bootstrap_topo_scan, gap_homes_repair_head)
{
	auto cfg = default_config ();
	cfg.rollback_min = 100;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (5000, 42) }));
	submit_member (scan, 42);
	scan.block_gapped (nano::block_hash{ 42 });
	ASSERT_EQ (scan.count_gapped (), 1);

	// Repair head 1 rolls back below the gap (5000 - rollback_min).
	auto pos = scan.next (1);
	ASSERT_TRUE (pos);
	ASSERT_EQ (pos->topo_height, 4900);
}

// While a gap persists, a repair head that scans up to the gap height without
// resolving it escalates its rollback (doubling).
TEST (bootstrap_topo_scan, repair_escalates_rollback)
{
	auto cfg = default_config ();
	cfg.rollback_min = 100;
	cfg.rollback_max = 100000;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (5000, 42) }));
	submit_member (scan, 42);
	scan.block_gapped (nano::block_hash{ 42 });

	auto pos1 = scan.next (1);
	ASSERT_EQ (pos1->topo_height, 4900); // 5000 - 100

	// Scan up to/over the gap height without resolving it.
	scan.process (1, *pos1, entries ({ key (5001, 7) }));

	// The gap is still there -> escalate: 5000 - 200.
	auto pos2 = scan.next (1);
	ASSERT_EQ (pos2->topo_height, 4800);
}

// A terminal result (fork / bad signature) is not a hole: the member is
// resolved and the watermark advances past it.
TEST (bootstrap_topo_scan, terminal_is_not_a_hole)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	submit_member (scan, 100);
	submit_member (scan, 200);

	scan.block_terminal (nano::block_hash{ 100 });
	scan.block_indexed (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_EQ (scan.count_gapped (), 0);
	ASSERT_EQ (scan.cursor (), key (2, 200));
}

// A queued member already in our ledger is confirmed without a fetch.
TEST (bootstrap_topo_scan, mark_redundant_confirms)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100) }));
	scan.mark_redundant (nano::block_hash{ 100 });
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_EQ (scan.cursor (), key (1, 100));
}

// The spear reaching the tip (a sustained empty quorum) with no outstanding
// members means the whole topology is synced.
TEST (bootstrap_topo_scan, caught_up_on_empty_tip)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_FALSE (scan.caught_up ());

	// consideration_count*2 empty responses conclude the tip.
	ASSERT_FALSE (scan.process (0, nano::topo_key{}, entries ({})));
	ASSERT_FALSE (scan.caught_up ());
	ASSERT_TRUE (scan.process (0, nano::topo_key{}, entries ({})));
	ASSERT_TRUE (scan.caught_up ());
	ASSERT_FALSE (scan.indexing ());

	// Poll mode re-arms the spear to look for new tip blocks.
	scan.repoll ();
	ASSERT_FALSE (scan.caught_up ());
	ASSERT_TRUE (scan.indexing ());
}

// Discovery backpressure: the spear pauses while the held member window is full.
TEST (bootstrap_topo_scan, spear_backpressure)
{
	auto cfg = default_config ();
	cfg.max_blocks_queued = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	ASSERT_EQ (scan.count_members (), 2);

	// Window full -> spear offers no new cursor.
	ASSERT_FALSE (scan.next (0).has_value ());
}

// Repair heads idle until there is a gap to chase.
TEST (bootstrap_topo_scan, repair_head_idle_without_gap)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_FALSE (scan.next (1).has_value ());
}

// reset returns to a clean initial state.
TEST (bootstrap_topo_scan, reset_clears_state)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	submit_member (scan, 100);
	scan.block_gapped (nano::block_hash{ 100 });
	ASSERT_GT (scan.count_members (), 0);

	scan.reset ();
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_EQ (scan.count_gapped (), 0);
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.caught_up ());
}
