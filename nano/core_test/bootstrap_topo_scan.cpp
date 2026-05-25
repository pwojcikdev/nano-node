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

// Below the gap threshold the spear keeps discovering and submission walks PAST a
// gap to make progress on an independent higher chain.
TEST (bootstrap_topo_scan, spear_tolerates_gaps_below_threshold)
{
	auto cfg = default_config ();
	cfg.gap_threshold = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	scan.next_blocks (10);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.next_submit (10);
	scan.block_gapped (nano::block_hash{ 100 });
	ASSERT_EQ (scan.count_gapped (), 1); // 1 < 3: not paused

	// Discover and receive an independent higher key above the gap.
	scan.process (0, key (2, 200), entries ({ key (3, 300) }));
	scan.next_blocks (10);
	scan.block_received (nano::block_hash{ 300 }, stub);

	// Submission walks past the sub-threshold gap to submit the higher chain.
	ASSERT_EQ (scan.next_submit (10).size (), 1); // 300

	// And the spear keeps discovering.
	ASSERT_TRUE (scan.next (0).has_value ());
}

// Reaching the gap threshold pauses the spear; clearing every gap resumes it.
TEST (bootstrap_topo_scan, spear_pauses_at_threshold)
{
	auto cfg = default_config ();
	cfg.gap_threshold = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200) }));
	scan.next_blocks (10);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.next_submit (10);

	scan.block_gapped (nano::block_hash{ 100 });
	scan.block_gapped (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_gapped (), 2);
	ASSERT_FALSE (scan.next (0, now).has_value ()); // threshold reached: paused

	// Resolving the gaps back to zero resumes the spear.
	scan.block_indexed (nano::block_hash{ 100 });
	scan.block_indexed (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_gapped (), 0);
	ASSERT_TRUE (scan.next (0, now).has_value ());
}

// The pause latch has hysteresis: once paused at the threshold, dropping to a
// non-zero gap count does NOT resume; only reaching zero does.
TEST (bootstrap_topo_scan, gap_threshold_hysteresis)
{
	auto cfg = default_config ();
	cfg.gap_threshold = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200), key (3, 300) }));
	scan.next_blocks (10);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);
	scan.next_submit (10);

	scan.block_gapped (nano::block_hash{ 100 });
	scan.block_gapped (nano::block_hash{ 200 });
	scan.block_gapped (nano::block_hash{ 300 });
	ASSERT_EQ (scan.count_gapped (), 3); // threshold reached
	ASSERT_FALSE (scan.next (0, now).has_value ());

	// Down to 1 gap: latch still held.
	scan.block_indexed (nano::block_hash{ 100 });
	scan.block_indexed (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_gapped (), 1);
	ASSERT_FALSE (scan.next (0, now).has_value ());

	// Last gap cleared: resume.
	scan.block_indexed (nano::block_hash{ 300 });
	ASSERT_EQ (scan.count_gapped (), 0);
	ASSERT_TRUE (scan.next (0, now).has_value ());
}

// While paused, submission stops at the first gap — no dependents are submitted
// past it (gap_threshold = 1 pauses at the first gap).
TEST (bootstrap_topo_scan, submit_stops_at_gap_when_paused)
{
	auto cfg = default_config ();
	cfg.gap_threshold = 1;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200), key (3, 300) }));
	scan.next_blocks (10); // all -> in_flight
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.next_submit (10); // submit 100 (stops at the in_flight 200)
	scan.block_gapped (nano::block_hash{ 100 }); // 1 gap == threshold -> paused

	// 200, 300 arrive but sit above the gap.
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);

	// Paused: nothing above the gap submits.
	ASSERT_TRUE (scan.next_submit (10).empty ());
}

// Filling a dependency that sits BELOW the watermark (a repair of a previously
// false-advanced region) must not drag the reported watermark backward.
TEST (bootstrap_topo_scan, watermark_monotonic)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.process (0, nano::topo_key{}, entries ({ key (5, 500) }));
	submit_member (scan, 500);
	scan.block_indexed (nano::block_hash{ 500 });
	ASSERT_EQ (scan.cursor (), key (5, 500));

	// A repair head re-discovers and fills a dependency below the watermark.
	scan.process (1, nano::topo_key{}, entries ({ key (2, 200) }));
	submit_member (scan, 200);
	scan.block_indexed (nano::block_hash{ 200 });

	ASSERT_EQ (scan.cursor (), key (5, 500)); // not dragged back to (2, 200)
	ASSERT_EQ (scan.count_members (), 0);
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

// A repair head idles while its bound (the head ahead's cursor) is still at
// genesis — there is nothing discovered to sweep yet.
TEST (bootstrap_topo_scan, repair_idle_when_bound_zero)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_FALSE (scan.next (1).has_value ());
}

// Once the spear advances, repair head 1 sweeps its range starting at genesis and
// moves forward over single responses.
TEST (bootstrap_topo_scan, repair_sweeps_after_spear_advances)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (5, 500) })); // spear bound -> height 5

	auto p1 = scan.next (1, now);
	ASSERT_TRUE (p1);
	ASSERT_EQ (*p1, nano::topo_key{}); // sweep starts at genesis

	// Single response advances the repair cursor (no quorum wait).
	ASSERT_TRUE (scan.process (1, *p1, entries ({ key (1, 100), key (2, 200) })));
	ASSERT_EQ (scan.count_members (), 3); // 500 (spear) + 100 + 200 (repair)

	auto p2 = scan.next (1, now);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, key (2, 200)); // continues forward from the last kept key
}

// When a repair head reaches its bound it wraps back to genesis (after one
// cooldown, so a small range can't hot-spin).
TEST (bootstrap_topo_scan, repair_wrap_around)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (3, 300) })); // bound height 3
	auto p1 = scan.next (1, now);
	ASSERT_EQ (*p1, nano::topo_key{});
	scan.process (1, *p1, entries ({ key (3, 300) })); // sweep up to the bound

	// Cursor now == bound: the query wraps (and is throttled this tick).
	ASSERT_FALSE (scan.next (1, now).has_value ());

	// After a cooldown the wrapped sweep fires again at genesis.
	auto p2 = scan.next (1, now + cfg.cooldown + 1s);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, nano::topo_key{});
}

// A repair head will not re-fire the same cursor within the cooldown (anti-spin),
// but fires again once it elapses.
TEST (bootstrap_topo_scan, repair_no_spin_cooldown)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (5, 500) }));

	ASSERT_TRUE (scan.next (1, now).has_value ());
	ASSERT_FALSE (scan.next (1, now).has_value ()); // within cooldown -> refused
	ASSERT_TRUE (scan.next (1, now + cfg.cooldown + 1s).has_value ()); // cooldown elapsed
}

// A repair head finalizes a page on the FIRST response, while the spear still
// needs the full consideration_count quorum.
TEST (bootstrap_topo_scan, repair_single_response_finalizes)
{
	auto cfg = default_config ();
	cfg.consideration_count = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	auto sp = scan.next (0, now);
	ASSERT_TRUE (sp);
	ASSERT_FALSE (scan.process (0, *sp, entries ({ key (5, 500) }))); // spear: 1st of 2, no advance
	ASSERT_TRUE (scan.process (0, *sp, entries ({ key (5, 500) }))); // spear: quorum reached

	auto rp = scan.next (1, now);
	ASSERT_TRUE (rp);
	ASSERT_TRUE (scan.process (1, *rp, entries ({ key (1, 100) }))); // repair: single response advances
	ASSERT_EQ (scan.count_members (), 2); // 500 + 100
}

// A repair page with nothing past the cursor makes no progress, does not assert,
// and the cursor can be retried.
TEST (bootstrap_topo_scan, repair_empty_progress_no_assert)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (5, 500) }));
	auto p = scan.next (1, now);
	ASSERT_TRUE (p);
	ASSERT_FALSE (scan.process (1, *p, entries ({}))); // empty page: no advance, no assert

	auto p2 = scan.next (1, now + cfg.cooldown + 1s);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, nano::topo_key{}); // same position, retried
}

// Quiescence: once the spear is done and no gap remains, the repair head idles so
// `members` can drain to empty and `caught_up` can be reached.
TEST (bootstrap_topo_scan, caught_up_quiesce)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (1, 100) }));
	ASSERT_TRUE (scan.next (1, now).has_value ()); // repair active while indexing

	// Resolve the member and drive the spear to the tip.
	submit_member (scan, 100);
	scan.block_indexed (nano::block_hash{ 100 });
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_FALSE (scan.process (0, key (1, 100), entries ({})));
	ASSERT_TRUE (scan.process (0, key (1, 100), entries ({})));

	ASSERT_TRUE (scan.caught_up ());
	ASSERT_FALSE (scan.indexing ());
	// Repair quiesces: spear done, no gaps.
	ASSERT_FALSE (scan.next (1, now + cfg.cooldown + 1s).has_value ());
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
