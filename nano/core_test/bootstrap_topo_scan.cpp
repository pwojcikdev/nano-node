#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
// Default test config: consideration_count / repair_consideration_count = 1 let a head
// finalize after a single response; head_count=2 gives one spear + one repair head;
// repair_activation_height=1 lets repair kick in as soon as the spear discovers anything
// (height >= 1) instead of waiting out the production default.
nano::topo_scan_config default_config ()
{
	nano::topo_scan_config cfg{};
	cfg.consideration_count = 1;
	cfg.repair_consideration_count = 1;
	cfg.head_count = 2;
	cfg.repair_activation_height = 1;
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

	// Repair head 1 re-scans from genesis: 100 is already tracked (deduped), 200 is new.
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
// A repair head idles while the spear anchor is still at genesis — there is nothing
// discovered to sweep yet.
TEST (bootstrap_topo_scan, repair_idle_when_anchor_zero)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_FALSE (scan.next (1).has_value ());
}

// Once the spear advances, repair head 1 sweeps UPWARD from genesis (topo order) over
// single responses.
TEST (bootstrap_topo_scan, repair_sweeps_up_from_genesis)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (100, 1) })); // spear anchor -> (100, 1)

	auto p1 = scan.next (1, now);
	ASSERT_TRUE (p1);
	ASSERT_EQ (*p1, nano::topo_key{}); // head 1 sweeps the full range, starting at genesis

	// Single response advances the cursor UP to the highest kept key.
	ASSERT_TRUE (scan.process (1, *p1, entries ({ key (20, 20), key (40, 40) })));
	ASSERT_EQ (scan.count_members (), 3); // (100,1) spear + (20) + (40) repair

	auto p2 = scan.next (1, now);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, key (40, 40)); // continues upward from the highest kept key
}

// Reaching the anchor (top of its range) restarts the sweep at the floor (genesis for
// head 1).
TEST (bootstrap_topo_scan, repair_wraps_at_anchor)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (3, 3) })); // anchor (3, 3)
	auto p1 = scan.next (1, now);
	ASSERT_EQ (*p1, nano::topo_key{}); // starts at genesis
	scan.process (1, *p1, entries ({ key (1, 1), key (3, 3) })); // sweep up to the anchor

	// Cursor reached the anchor -> restart at the floor (genesis).
	auto p2 = scan.next (1, now);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, nano::topo_key{});
}

// Head 1 sweeps the full range from genesis; each subsequent head's lower bound (start)
// is a fixed fraction of the spear (anchor - anchor/2^(h-1)), INDEPENDENT of the other
// heads' positions — head 2 covers the top half [anchor/2, anchor].
TEST (bootstrap_topo_scan, repair_head_floor_is_spear_relative)
{
	auto cfg = default_config ();
	cfg.head_count = 3; // spear + repair head 1 + repair head 2
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (100, 1) })); // anchor (100, 1)

	// Move head 1 up a bit — head 2's lower bound must NOT depend on it.
	auto p1 = scan.next (1, now);
	ASSERT_EQ (*p1, nano::topo_key{}); // head 1 starts at genesis
	scan.process (1, *p1, entries ({ key (20, 20) })); // head 1 cursor -> 20

	// Head 2's lower bound = anchor - anchor/2 = 50; it starts there, not at genesis.
	auto p2 = scan.next (2, now);
	ASSERT_TRUE (p2);
	ASSERT_EQ (p2->topo_height, 50u); // spear-relative floor (independent of head 1's 20)
	scan.process (2, *p2, entries ({ key (80, 80) })); // sweep up to 80

	// 80 < anchor (100), so head 2 keeps going up (not wrapped yet).
	auto p2c = scan.next (2, now);
	ASSERT_TRUE (p2c);
	ASSERT_EQ (*p2c, key (80, 80));

	scan.process (2, *p2c, entries ({ key (100, 1) })); // reach the anchor
	auto p2d = scan.next (2, now);
	ASSERT_TRUE (p2d);
	ASSERT_EQ (p2d->topo_height, 50u); // wrapped back to the floor (50), NOT genesis
}

// A repair head will not re-fire an unanswered cursor within the cooldown (anti-spin),
// but fires again once it elapses.
TEST (bootstrap_topo_scan, repair_no_spin_cooldown)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (100, 1) })); // anchor (100, 1)

	ASSERT_TRUE (scan.next (1, now).has_value ()); // fires at genesis
	ASSERT_FALSE (scan.next (1, now).has_value ()); // unanswered cursor within cooldown -> refused
	ASSERT_TRUE (scan.next (1, now + cfg.cooldown + 1s).has_value ()); // cooldown elapsed -> retry
}

// Each head unions its own target before finalizing: the spear consideration_count (2 here),
// a repair head the smaller repair_consideration_count (1 here, so it finalizes on the first).
TEST (bootstrap_topo_scan, repair_single_response_finalizes)
{
	auto cfg = default_config ();
	cfg.consideration_count = 2;
	cfg.repair_consideration_count = 1;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	auto sp = scan.next (0, now);
	ASSERT_TRUE (sp);
	ASSERT_FALSE (scan.process (0, *sp, entries ({ key (5, 500) }))); // spear: 1st of 2, no advance
	ASSERT_TRUE (scan.process (0, *sp, entries ({ key (5, 500) }))); // spear: quorum reached

	auto rp = scan.next (1, now);
	ASSERT_TRUE (rp);
	ASSERT_EQ (*rp, nano::topo_key{}); // head 1 starts at genesis
	ASSERT_TRUE (scan.process (1, *rp, entries ({ key (1, 100) }))); // repair: single response advances
	ASSERT_EQ (scan.count_members (), 2); // (5,500) + (1,100)
}

// A repair head also unions a redundant quorum when repair_consideration_count > 1: it bursts
// that many requests per page and only advances once that many responses arrive.
TEST (bootstrap_topo_scan, repair_quorum_redundant)
{
	auto cfg = default_config ();
	cfg.repair_consideration_count = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (100, 1) })); // spear anchor -> (100, 1)

	// The repair head bursts up to 2 requests before the cooldown (not just one).
	ASSERT_TRUE (scan.next (1, now));
	ASSERT_TRUE (scan.next (1, now)); // 2nd of the quorum, same cooldown window
	ASSERT_FALSE (scan.next (1, now)); // quorum issued -> wait for cooldown

	// First response does NOT advance; the second (quorum reached) does.
	ASSERT_FALSE (scan.process (1, nano::topo_key{}, entries ({ key (20, 20) })));
	ASSERT_TRUE (scan.process (1, nano::topo_key{}, entries ({ key (40, 40) })));
	ASSERT_EQ (scan.count_members (), 3); // (100,1) spear + (20) + (40) repair
}

// A repair page with nothing past the cursor makes no progress and does not assert; the
// cursor is retried.
TEST (bootstrap_topo_scan, repair_empty_page_retries)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.process (0, nano::topo_key{}, entries ({ key (5, 500) })); // anchor (5, 500)
	auto p = scan.next (1, now);
	ASSERT_TRUE (p);
	ASSERT_EQ (*p, nano::topo_key{}); // genesis
	ASSERT_FALSE (scan.process (1, *p, entries ({}))); // empty page: no advance, no assert

	auto p2 = scan.next (1, now + cfg.cooldown + 1s);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, nano::topo_key{}); // same position, retried
}

// Repair heads stay idle until the spear frontier passes `repair_activation_height`,
// then kick in — this skips the dense dependency-free low layer at the start.
TEST (bootstrap_topo_scan, repair_activation_gate)
{
	auto cfg = default_config ();
	cfg.repair_activation_height = 50; // idle until the spear passes height 50
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	// Spear at height 10 (< 50): repair stays idle.
	scan.process (0, nano::topo_key{}, entries ({ key (10, 10) }));
	ASSERT_FALSE (scan.next (1, now).has_value ());

	// Spear climbs past the threshold: repair kicks in.
	scan.process (0, key (10, 10), entries ({ key (60, 60) }));
	ASSERT_TRUE (scan.next (1, now).has_value ());
}

// A head freezes its ceiling at the spear frontier when it (re)starts a sweep and holds
// it fixed: it wraps at the frozen ceiling instead of chasing a fast-moving spear.
TEST (bootstrap_topo_scan, repair_ceiling_frozen)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	// Spear at 100; head 1 freezes its ceiling at 100 and starts at genesis.
	scan.process (0, nano::topo_key{}, entries ({ key (100, 1) }));
	auto p1 = scan.next (1, now);
	ASSERT_EQ (*p1, nano::topo_key{});

	// Spear races far ahead while head 1 is mid-sweep.
	scan.process (0, key (100, 1), entries ({ key (100000, 2) })); // spear -> 100000

	// Head 1 advances to its FROZEN ceiling (100) and wraps back to the floor — it does
	// NOT chase the spear up to 100000 (which the live-anchor design would have done).
	scan.process (1, *p1, entries ({ key (100, 1) })); // head 1 cursor -> 100 (== frozen ceiling)
	auto p2 = scan.next (1, now);
	ASSERT_TRUE (p2);
	ASSERT_EQ (*p2, nano::topo_key{}); // wrapped at the frozen ceiling, not chasing
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
	ASSERT_FALSE (scan.next (1, now).has_value ());
}

// A gap in already-confirmed territory (a repair head re-finding a hole below the watermark)
// must not wedge the prune of confirmed members above it, nor count against the discovery
// window — so a handful of gaps cannot stall the spear below gap_threshold.
TEST (bootstrap_topo_scan, low_gap_does_not_stall_spear)
{
	auto cfg = default_config ();
	cfg.max_blocks_queued = 3; // tiny window, so a wedged backlog would trip backpressure
	cfg.gap_threshold = 100; // high, so the repair_wait latch stays off
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Spear confirms a prefix; the watermark advances to height 3.
	scan.process (0, nano::topo_key{}, entries ({ key (1, 100), key (2, 200), key (3, 300) }));
	submit_member (scan, 100);
	submit_member (scan, 200);
	submit_member (scan, 300);
	scan.block_indexed (nano::block_hash{ 100 });
	scan.block_indexed (nano::block_hash{ 200 });
	scan.block_indexed (nano::block_hash{ 300 });
	ASSERT_EQ (scan.count_members (), 0);
	ASSERT_EQ (scan.cursor (), key (3, 300));

	// A repair head re-discovers four keys below the watermark: 10 is a genuine hole (gaps),
	// 20/30/40 are redundant re-confirms.
	scan.process (1, nano::topo_key{}, entries ({ key (1, 10), key (1, 20), key (1, 30), key (1, 40) }));
	submit_member (scan, 10);
	submit_member (scan, 20);
	submit_member (scan, 30);
	submit_member (scan, 40);
	scan.block_gapped (nano::block_hash{ 10 });
	scan.block_indexed (nano::block_hash{ 20 });
	scan.block_indexed (nano::block_hash{ 30 });
	scan.block_indexed (nano::block_hash{ 40 });

	// The confirmed re-discoveries are released despite the lower gap; only the unresolved
	// hole is retained, so the active window stays below max_blocks_queued and the spear is
	// free to keep extending the frontier (would be backpressure-stalled before this fix).
	ASSERT_EQ (scan.count_gapped (), 1);
	ASSERT_EQ (scan.count_in_ledger (), 0);
	ASSERT_EQ (scan.count_members (), 1);
	ASSERT_TRUE (scan.next (0).has_value ());
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

// The spear quorum adapts to the reachable peer count: with consideration_count=4 but only
// 2 capable peers, the page finalizes after 2 responses instead of stalling waiting for 4.
TEST (bootstrap_topo_scan, spear_quorum_adapts_to_peer_count)
{
	auto cfg = default_config ();
	cfg.consideration_count = 4;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.freeze_target (0, 2); // only 2 capable peers -> round target 2

	auto sp = scan.next (0, now);
	ASSERT_TRUE (sp);
	ASSERT_FALSE (scan.process (0, *sp, entries ({ key (1, 100) }))); // 1st of 2
	ASSERT_TRUE (scan.process (0, *sp, entries ({ key (1, 100) }))); // adaptive target reached
	ASSERT_EQ (scan.count_members (), 1);
}

// freeze_target sizes the round to min(consideration_count, capable peers); record_query
// tracks distinct node ids (deduped); new_round reopens the pool but keeps the target.
TEST (bootstrap_topo_scan, distinct_peers_round)
{
	auto cfg = default_config ();
	cfg.consideration_count = 4;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.freeze_target (0, 2); // 2 capable peers -> target 2
	// freeze is a no-op once set (a later, larger pool does not raise the frozen target).
	scan.freeze_target (0, 10);

	ASSERT_FALSE (scan.round_full (0));
	scan.record_query (0, nano::node_id{ 1 });
	ASSERT_FALSE (scan.round_full (0));
	ASSERT_EQ (scan.seen_peers (0).size (), 1);

	scan.record_query (0, nano::node_id{ 2 });
	ASSERT_TRUE (scan.round_full (0)); // 2 distinct queried == target
	ASSERT_EQ (scan.seen_peers (0).size (), 2);

	scan.record_query (0, nano::node_id{ 2 }); // duplicate node id not double-counted
	ASSERT_EQ (scan.seen_peers (0).size (), 2);

	scan.new_round (0); // cooldown re-fire reopens the pool
	ASSERT_EQ (scan.seen_peers (0).size (), 0);
	ASSERT_FALSE (scan.round_full (0));
}

// freeze_target floors the round at 1 even with zero reachable peers: a target of 0 would
// make the empty-tip threshold (target*2) zero and instantly declare a false tip.
TEST (bootstrap_topo_scan, freeze_target_floors_at_one)
{
	auto cfg = default_config ();
	cfg.consideration_count = 4;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.freeze_target (0, 0); // no capable peers
	ASSERT_FALSE (scan.round_full (0)); // target floored to 1, 0 queried < 1
	scan.record_query (0, nano::node_id{ 7 });
	ASSERT_TRUE (scan.round_full (0)); // 1 queried >= floored target

	// cap_target with nothing reached is a no-op (never sets target to 0).
	nano::bootstrap::topo_scan_index fresh{ cfg };
	fresh.freeze_target (0, 4);
	fresh.cap_target (0); // no peers recorded
	ASSERT_FALSE (fresh.round_full (0)); // target unchanged at 4, 0 < 4
}

// When fewer peers are reachable than the frozen target, cap_target shrinks the round to the
// peers actually reached so the gather can finalize instead of stalling.
TEST (bootstrap_topo_scan, distinct_peers_cap_target)
{
	auto cfg = default_config ();
	cfg.consideration_count = 4;
	nano::bootstrap::topo_scan_index scan{ cfg };
	auto const now = std::chrono::steady_clock::now ();

	scan.freeze_target (0, 4); // target 4
	scan.record_query (0, nano::node_id{ 1 });
	scan.record_query (0, nano::node_id{ 2 });
	ASSERT_FALSE (scan.round_full (0)); // 2 < 4

	scan.cap_target (0); // pool exhausted at 2 reachable peers
	ASSERT_TRUE (scan.round_full (0)); // 2 >= capped target

	auto sp = scan.next (0, now);
	ASSERT_TRUE (sp);
	ASSERT_FALSE (scan.process (0, *sp, entries ({ key (1, 100) }))); // 1st of capped 2
	ASSERT_TRUE (scan.process (0, *sp, entries ({ key (1, 100) }))); // capped target reached
}

// The empty-tip detection uses the adaptive target: with target 2 the tip concludes after
// 2*2 = 4 empty responses (distinct rounds), not consideration_count*2 = 8.
TEST (bootstrap_topo_scan, spear_tip_uses_adaptive_target)
{
	auto cfg = default_config ();
	cfg.consideration_count = 4;
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.freeze_target (0, 2); // target 2 -> tip needs 4 empties

	for (int i = 0; i < 3; ++i)
	{
		ASSERT_FALSE (scan.process (0, nano::topo_key{}, entries ({})));
	}
	ASSERT_FALSE (scan.caught_up ());
	ASSERT_TRUE (scan.process (0, nano::topo_key{}, entries ({}))); // 4th empty -> tip
	ASSERT_TRUE (scan.caught_up ());
}
