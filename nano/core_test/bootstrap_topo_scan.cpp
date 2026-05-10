#include <nano/lib/blocks.hpp>
#include <nano/node/bootstrap/topo_scan_index.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace
{
// Default test config: consideration_count=1 keeps tests simple by letting the cursor advance after a single response.
// Convergence-specific tests override.
nano::topo_scan_config default_config ()
{
	nano::topo_scan_config cfg{};
	cfg.consideration_count = 1;
	return cfg;
}

nano::topo_key key (uint64_t height, uint64_t hash_value)
{
	return nano::topo_key{ height, nano::block_hash{ hash_value } };
}
}

TEST (bootstrap_topo_scan, construction)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());
	ASSERT_EQ (scan.count_outstanding (), 0);
	ASSERT_EQ (scan.count_pending (), 0);
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
}

TEST (bootstrap_topo_scan, next_discovery)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_EQ (scan.cursor (), nano::topo_key{});

	auto pos = scan.next ();
	ASSERT_TRUE (pos.has_value ());
	ASSERT_EQ (*pos, nano::topo_key{});
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // next() doesn't move the cursor
}

TEST (bootstrap_topo_scan, process_basic)
{
	auto cfg = default_config ();
	cfg.cooldown = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	ASSERT_TRUE (pos.has_value ());

	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	ASSERT_TRUE (scan.process (*pos, entries));

	ASSERT_TRUE (scan.indexing ());
	ASSERT_TRUE (scan.has_blocks_pending ());
	ASSERT_EQ (scan.count_pending (), 3);

	// Indexing must still be able to terminate. Feed empty responses from the
	// advanced cursor until the end-of-topology threshold is hit.
	std::deque<nano::topo_key> empty;
	ASSERT_FALSE (scan.process (*scan.next (t0 + 200ms), empty));
	ASSERT_TRUE (scan.indexing ());
	ASSERT_TRUE (scan.process (*scan.next (t0 + 400ms), empty));
	ASSERT_FALSE (scan.indexing ());
}

TEST (bootstrap_topo_scan, process)
{
	auto cfg = default_config ();
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	ASSERT_TRUE (pos.has_value ());
	ASSERT_EQ (*pos, nano::topo_key{});

	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	ASSERT_TRUE (scan.process (*pos, entries));
	ASSERT_TRUE (scan.indexing ()); // Full batch -> not done
	ASSERT_EQ (scan.cursor (), key (3, 300)); // Cursor moved to the highest kept entry

	auto next = scan.next ();
	ASSERT_TRUE (next.has_value ());
	ASSERT_EQ (*next, key (3, 300));

	// If a response carries more entries than cfg.candidates, the trim drops the surplus
	std::deque<nano::topo_key> oversize{ key (4, 400), key (5, 500), key (6, 600), key (7, 700), key (8, 800) };
	ASSERT_TRUE (scan.process (*next, oversize));
	ASSERT_EQ (scan.count_pending (), 6); // 3 from the first batch + 3 kept from the oversize batch
	ASSERT_EQ (scan.cursor (), key (6, 600));

	auto after = scan.next ();
	ASSERT_TRUE (after.has_value ());
	ASSERT_EQ (*after, key (6, 600));
}

TEST (bootstrap_topo_scan, next_blocks_basic)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	scan.process (*pos, entries);

	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 3);
	ASSERT_EQ (blocks[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks[1], nano::block_hash{ 200 });
	ASSERT_EQ (blocks[2], nano::block_hash{ 300 });

	auto blocks2 = scan.next_blocks (10);
	ASSERT_TRUE (blocks2.empty ());
}

TEST (bootstrap_topo_scan, next_blocks_respects_max_count)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300), key (4, 144), key (5, 244) };
	scan.process (*pos, entries);

	auto first = scan.next_blocks (2);
	ASSERT_EQ (first.size (), 2);
	ASSERT_EQ (first[0], nano::block_hash{ 100 });
	ASSERT_EQ (first[1], nano::block_hash{ 200 });

	auto rest = scan.next_blocks (10);
	ASSERT_EQ (rest.size (), 3);
}

TEST (bootstrap_topo_scan, block_done_individual)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	scan.process (*pos, entries);
	scan.next_blocks (10);

	scan.block_done (nano::block_hash{ 100 });
	ASSERT_TRUE (scan.has_blocks_pending ());
	scan.block_done (nano::block_hash{ 200 });
	ASSERT_TRUE (scan.has_blocks_pending ());
	scan.block_done (nano::block_hash{ 300 });
	ASSERT_FALSE (scan.has_blocks_pending ());
}

TEST (bootstrap_topo_scan, cooldown)
{
	auto cfg = default_config ();
	cfg.cooldown = 250ms;
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto first = scan.next (t0);
	ASSERT_TRUE (first.has_value ());

	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	scan.process (*first, entries);

	// After process, requests reset; immediate next() succeeds
	auto second = scan.next (t0);
	ASSERT_TRUE (second.has_value ());

	// Second consecutive call hits the cooldown gate (consideration_count=1 already used)
	auto third = scan.next (t0);
	ASSERT_FALSE (third.has_value ());

	auto fourth = scan.next (t0 + 500ms);
	ASSERT_TRUE (fourth.has_value ());
}

TEST (bootstrap_topo_scan, reset)
{
	auto cfg = default_config ();
	cfg.cooldown = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200) };
	scan.process (*pos, entries);
	ASSERT_TRUE (scan.indexing ());
	ASSERT_TRUE (scan.has_blocks_pending ());

	// Drive indexing to completion (consideration_count*2 empty responses) so we reset from a fully-done state
	std::deque<nano::topo_key> empty;
	scan.process (*scan.next (t0 + 200ms), empty);
	ASSERT_TRUE (scan.process (*scan.next (t0 + 400ms), empty));
	ASSERT_FALSE (scan.indexing ());

	scan.reset ();

	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());
	ASSERT_EQ (scan.cursor (), nano::topo_key{});

	auto pos2 = scan.next (t0 + 600ms);
	ASSERT_TRUE (pos2.has_value ());
	ASSERT_EQ (*pos2, nano::topo_key{});
}

TEST (bootstrap_topo_scan, empty_response_signals_end)
{
	// consideration_count = 1 (default_config) -> need 2 empty responses to declare topology end (consideration_count * 2)
	auto cfg = default_config ();
	cfg.cooldown = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();
	std::deque<nano::topo_key> empty;

	// First empty response: not enough evidence yet to declare end
	auto pos = scan.next (t0);
	ASSERT_TRUE (pos.has_value ());
	ASSERT_FALSE (scan.process (*pos, empty));
	ASSERT_TRUE (scan.indexing ());

	// Second empty response trips end
	ASSERT_TRUE (scan.process (*pos, empty));
	ASSERT_FALSE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());

	// Index is done; subsequent next() returns nothing regardless of cooldown
	ASSERT_FALSE (scan.next (t0 + 1s).has_value ());
}

TEST (bootstrap_topo_scan, duplicate_entries_ignored)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 100) };
	scan.process (*pos, entries);

	// Hash 100 dedupes via the block-fetch index (the topo_key set keeps both topologically distinct entries).
	auto blocks = scan.next_blocks (10);
	std::set<nano::block_hash> unique{ blocks.begin (), blocks.end () };
	ASSERT_EQ (unique.size (), 2);
}

TEST (bootstrap_topo_scan, block_retry)
{
	auto cfg = default_config ();
	cfg.block_retry = 250ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	// Seed the index with three pending entries.
	auto pos = scan.next (t0);
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	scan.process (*pos, entries);

	// First call issues all three; they transition pending -> in_flight at t0.
	auto first = scan.next_blocks (10, t0);
	ASSERT_EQ (first.size (), 3);

	// Same `now` as the issue time: nothing is pending and no in_flight entry has
	// crossed the retry cutoff yet, so the call yields nothing.
	auto second = scan.next_blocks (10, t0);
	ASSERT_TRUE (second.empty ());

	// Drop one entry from the index entirely. The remaining two are still in_flight
	// at t0 and still inside their retry window.
	scan.block_done (nano::block_hash{ 200 });
	auto third = scan.next_blocks (10, t0);
	ASSERT_TRUE (third.empty ());

	// Advance past block_retry: the in-loop retry path inside next_blocks now sees
	// the surviving in_flight entries as stale and re-issues them in topological
	// order. Block 200 was removed by block_done so it doesn't reappear.
	auto fourth = scan.next_blocks (10, t0 + 500ms);
	ASSERT_EQ (fourth.size (), 2);
	ASSERT_EQ (fourth[0], nano::block_hash{ 100 });
	ASSERT_EQ (fourth[1], nano::block_hash{ 300 });
}

TEST (bootstrap_topo_scan, backpressure_index)
{
	auto cfg = default_config ();
	cfg.candidates = 3;
	cfg.max_blocks_queued = 5;
	nano::bootstrap::topo_scan_index scan{ cfg };

	// First batch fills 3 of the 5-slot block-fetch queue
	auto p1 = scan.next ();
	scan.process (*p1, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	ASSERT_EQ (scan.count_outstanding (), 3);

	// Second batch is still under the cap (3 < 5), so next() returns a position
	// and the process call pushes the queue to 6 entries — the excess is not rejected, but the index discovery backpressure gate is now fully engaged.
	auto p2 = scan.next ();
	ASSERT_TRUE (p2.has_value ());
	scan.process (*p2, std::deque<nano::topo_key>{ key (4, 144), key (5, 244), key (6, 244 + 1) });
	ASSERT_EQ (scan.count_outstanding (), 6);

	// Now the queue is over `max_blocks_queued` so next() short-circuits — index
	// discovery pauses until the block-fetch side drains.
	auto p3 = scan.next ();
	ASSERT_FALSE (p3.has_value ());

	// Drain a couple of entries via the normal path: issue 3 to put them in_flight,
	// then block_done two of them to evict them from the queue entirely.
	scan.next_blocks (3);
	scan.block_done (nano::block_hash{ 100 });
	scan.block_done (nano::block_hash{ 200 });
	ASSERT_EQ (scan.count_outstanding (), 4);

	// Below the cap again — index discovery resumes.
	auto p4 = scan.next ();
	ASSERT_TRUE (p4.has_value ());
}

TEST (bootstrap_topo_scan, backpressure_blocks)
{
	auto cfg = default_config ();
	cfg.max_blocks_outstanding = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Seed the index with five pending entries
	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300), key (4, 144), key (5, 244) });

	// First call issues up to the max (2), leaving the rest pending
	auto first = scan.next_blocks (cfg.max_blocks_outstanding);
	ASSERT_EQ (first.size (), 2);
	ASSERT_EQ (scan.count_in_flight (), 2);

	// The second call sees the backpressure and yields nothing
	auto second = scan.next_blocks (10);
	ASSERT_TRUE (second.empty ());

	// Draining one block makes room for another to be issued, but not all pending entries
	scan.block_done (nano::block_hash{ 100 });
	auto third = scan.next_blocks (10);
	ASSERT_EQ (third.size (), 3);
}

TEST (bootstrap_topo_scan, block_received_and_drain)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Seed the index with three pending entries, then issue them all so they're in_flight
	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	scan.next_blocks (10);

	std::shared_ptr<nano::block> stub; // The drain-order semantics don't depend on actual block contents.

	// Receive the middle block first - drain stops because the head is not completed.
	scan.block_received (nano::block_hash{ 200 }, stub);
	auto drained = scan.next_ordered_blocks (10);
	ASSERT_TRUE (drained.empty ());

	// Now receive the head and tail too.
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);
	auto drained2 = scan.next_ordered_blocks (10);
	ASSERT_EQ (drained2.size (), 3);

	// Already drained; block_done() is a no-op.
	scan.block_done (nano::block_hash{ 100 });
}

TEST (bootstrap_topo_scan, convergence_basic)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 3; // Require 3 responses before the cursor advances
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };

	// Three calls return the same position; cursor doesn't advance yet.
	auto p1 = scan.next ();
	ASSERT_TRUE (p1.has_value ());
	ASSERT_FALSE (scan.process (*p1, entries));
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // partial quorum keeps cursor pinned

	auto p2 = scan.next ();
	ASSERT_TRUE (p2.has_value ());
	ASSERT_EQ (*p1, *p2);
	ASSERT_FALSE (scan.process (*p2, entries));
	ASSERT_EQ (scan.cursor (), nano::topo_key{});

	auto p3 = scan.next ();
	ASSERT_TRUE (p3.has_value ());
	ASSERT_EQ (*p1, *p3);

	// Third response triggers cursor advance.
	ASSERT_TRUE (scan.process (*p3, entries));
	ASSERT_EQ (scan.count_pending (), 3);
	ASSERT_EQ (scan.cursor (), key (3, 300));

	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 3);
	ASSERT_EQ (blocks[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks[1], nano::block_hash{ 200 });
	ASSERT_EQ (blocks[2], nano::block_hash{ 300 });
}

TEST (bootstrap_topo_scan, convergence_union)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 3;
	cfg.candidates = 10;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto p = scan.next ();
	ASSERT_TRUE (p.has_value ());

	// Three responses with overlapping but distinct entries.
	scan.process (*p, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });
	auto p2 = scan.next ();
	scan.process (*p2, std::deque<nano::topo_key>{ key (2, 200), key (3, 300) });
	auto p3 = scan.next ();
	ASSERT_TRUE (scan.process (*p3, std::deque<nano::topo_key>{ key (1, 100), key (3, 300), key (4, 144) }));

	// Union should contain all 4 unique entries.
	ASSERT_EQ (scan.count_pending (), 4);
	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 4);
	ASSERT_EQ (blocks[0], nano::block_hash{ 100 });
	ASSERT_EQ (blocks[1], nano::block_hash{ 200 });
	ASSERT_EQ (blocks[2], nano::block_hash{ 300 });
	ASSERT_EQ (blocks[3], nano::block_hash{ 144 });

	// Cursor lands on the last (highest) kept entry of the union.
	ASSERT_EQ (scan.cursor (), key (4, 144));
}

TEST (bootstrap_topo_scan, convergence_trim_to_batch_size)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 2;
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	std::deque<nano::topo_key> entries{
		key (1, 1), key (2, 2), key (3, 3), key (4, 4), key (5, 5)
	};

	auto p1 = scan.next ();
	scan.process (*p1, entries);
	auto p2 = scan.next ();
	ASSERT_TRUE (scan.process (*p2, entries));

	// Only first 3 (lowest topo_height) should be in the queue.
	ASSERT_EQ (scan.count_pending (), 3);
	auto blocks = scan.next_blocks (10);
	std::set<nano::block_hash> seen{ blocks.begin (), blocks.end () };
	ASSERT_TRUE (seen.count (nano::block_hash{ 1 }));
	ASSERT_TRUE (seen.count (nano::block_hash{ 2 }));
	ASSERT_TRUE (seen.count (nano::block_hash{ 3 }));

	// Cursor should advance to the 3rd entry.
	auto p3 = scan.next ();
	ASSERT_TRUE (p3.has_value ());
	ASSERT_EQ (*p3, key (3, 3));
}

TEST (bootstrap_topo_scan, convergence_poison_resistance)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 2;
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Honest peer reports the real next 3 entries;
	// Malicious peer agrees on those three but pads with two far-future fakes, hoping to push the cursor past real entries and skip them.
	std::deque<nano::topo_key> honest{ key (1, 1), key (2, 2), key (3, 3) };
	std::deque<nano::topo_key> malicious{ key (1, 1), key (2, 2), key (3, 3), key (100, 200), key (101, 201) };

	auto p1 = scan.next ();
	scan.process (*p1, honest);
	auto p2 = scan.next ();
	ASSERT_TRUE (scan.process (*p2, malicious));

	// Union has 5 entries, but trim keeps only the 3 lowest by topo_key, so the
	// fakes (height 100, 101) get dropped and don't enter the block-fetch queue.
	ASSERT_EQ (scan.count_pending (), 3);
	auto blocks = scan.next_blocks (10);
	std::set<nano::block_hash> seen{ blocks.begin (), blocks.end () };
	ASSERT_FALSE (seen.count (nano::block_hash{ 200 }));
	ASSERT_FALSE (seen.count (nano::block_hash{ 201 }));
}

TEST (bootstrap_topo_scan, convergence_requires_multiple_calls)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 3; // Require 3 responses before the cursor advances
	cfg.candidates = 5;
	nano::bootstrap::topo_scan_index scan{ cfg };

	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };

	// First response: counted toward quorum but the candidate set isn't yet
	// committed to the block-fetch queue, so next_blocks has nothing to issue.
	auto p1 = scan.next ();
	scan.process (*p1, entries);
	ASSERT_TRUE (scan.next_blocks (10).empty ());

	// Second response: still one short of the quorum.
	auto p2 = scan.next ();
	scan.process (*p2, entries);
	ASSERT_TRUE (scan.next_blocks (10).empty ());

	// Third response completes the quorum; the union is finalized and entries
	// move into the block-fetch queue, so next_blocks now returns them.
	auto p3 = scan.next ();
	scan.process (*p3, entries);

	auto blocks = scan.next_blocks (10);
	ASSERT_EQ (blocks.size (), 3);
}

TEST (bootstrap_topo_scan, convergence_cooldown)
{
	nano::topo_scan_config cfg;
	cfg.consideration_count = 2;
	cfg.cooldown = 250ms;
	cfg.candidates = 5;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	// At t0 the consideration_count budget is fresh, so two next() calls in a row
	// both succeed without hitting the cooldown gate.
	auto p1 = scan.next (t0);
	auto p2 = scan.next (t0);
	ASSERT_TRUE (p1.has_value ());
	ASSERT_TRUE (p2.has_value ());

	// Budget exhausted (requests == consideration_count) and no time has passed,
	// so the cooldown gate kicks in: a third call at t0 returns nothing. This
	// caps how often we'll hammer peers if responses are slow / never come.
	auto p3 = scan.next (t0);
	ASSERT_FALSE (p3.has_value ());

	// After cooldown elapses, next() is eligible again. No response ever arrived,
	// so the head state is unchanged and we get the same cursor back.
	auto p4 = scan.next (t0 + 500ms);
	ASSERT_TRUE (p4.has_value ());
	ASSERT_EQ (*p4, *p1);
}

TEST (bootstrap_topo_scan, convergence_all_empty_signals_end)
{
	// With consideration_count=3, declaring topology end requires 6 empty responses (consideration_count * 2).
	// The first quorum confirms a single "no candidates" result; the second quorum confirms it wasn't a fluke.
	nano::topo_scan_config cfg;
	cfg.consideration_count = 3;
	cfg.cooldown = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();
	std::deque<nano::topo_key> empty;

	// First quorum (3 calls fit the initial budget at t0): not advanced yet.
	for (int i = 0; i < 3; ++i)
	{
		auto pos = scan.next (t0);
		ASSERT_TRUE (pos.has_value ());
		ASSERT_FALSE (scan.process (*pos, empty));
	}
	ASSERT_TRUE (scan.indexing ());

	// Once the budget is exhausted, each follow-up call needs cooldown to elapse
	// since the previous one. The 4th and 5th still don't advance.
	for (int i = 1; i <= 2; ++i)
	{
		auto pos = scan.next (t0 + std::chrono::milliseconds (200 * i));
		ASSERT_TRUE (pos.has_value ());
		ASSERT_FALSE (scan.process (*pos, empty));
	}
	ASSERT_TRUE (scan.indexing ());

	// 6th empty response trips end.
	auto pos = scan.next (t0 + 600ms);
	ASSERT_TRUE (pos.has_value ());
	ASSERT_TRUE (scan.process (*pos, empty));
	ASSERT_FALSE (scan.indexing ());
}

TEST (bootstrap_topo_scan, convergence_stale_response)
{
	auto cfg = default_config ();
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto p1 = scan.next ();
	std::deque<nano::topo_key> entries{ key (1, 100), key (2, 200), key (3, 300) };
	ASSERT_TRUE (scan.process (*p1, entries));

	// p1 was at zero. Cursor has now advanced. Re-delivering at p1 is rejected.
	ASSERT_FALSE (scan.process (*p1, entries));
}

TEST (bootstrap_topo_scan, cleanup_resets_stale)
{
	auto cfg = default_config ();
	cfg.block_retry = 250ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });

	auto issued = scan.next_blocks (10, t0); // 100, 200, 300 -> in_flight
	ASSERT_EQ (issued.size (), 3);
	ASSERT_EQ (scan.count_in_flight (), 3);
	ASSERT_EQ (scan.count_pending (), 0);

	// Before retry cutoff: nothing is stale.
	ASSERT_EQ (scan.cleanup (t0 + 100ms), 0);
	ASSERT_EQ (scan.count_in_flight (), 3);
	ASSERT_EQ (scan.count_pending (), 0);

	// After retry cutoff: all three slots reclaimed.
	ASSERT_EQ (scan.cleanup (t0 + 500ms), 3);
	ASSERT_EQ (scan.count_in_flight (), 0);
	ASSERT_EQ (scan.count_pending (), 3);

	auto retried = scan.next_blocks (10, t0 + 600ms); // 100, 200, 300 re-issued in topo order
	ASSERT_EQ (retried.size (), 3);
	ASSERT_EQ (retried[0], nano::block_hash{ 100 });
	ASSERT_EQ (retried[1], nano::block_hash{ 200 });
	ASSERT_EQ (retried[2], nano::block_hash{ 300 });
	ASSERT_EQ (scan.count_in_flight (), 3);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 200 }, stub); // 200 -> completed
	ASSERT_EQ (scan.count_in_flight (), 2);

	// Well past the retry cutoff: only the in_flight entries (100, 300) should reset.
	ASSERT_EQ (scan.cleanup (t0 + 1s), 2);
	ASSERT_EQ (scan.count_pending (), 2);
	ASSERT_EQ (scan.count_in_flight (), 0);
	ASSERT_EQ (scan.count_completed (), 1);
}

TEST (bootstrap_topo_scan, cleanup_unjams_backpressure)
{
	auto cfg = default_config ();
	cfg.block_retry = 250ms;
	cfg.max_blocks_outstanding = 2;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300), key (4, 400) });

	// Saturate the in-flight budget. max_count caps a single batch to the budget.
	auto first = scan.next_blocks (cfg.max_blocks_outstanding, t0);
	ASSERT_EQ (first.size (), 2);
	ASSERT_EQ (scan.count_in_flight (), 2);

	// Backpressure pins us — even after block_retry timeout elapses the index is fetching is paused.
	ASSERT_TRUE (scan.next_blocks (10, t0 + 500ms).empty ());
	ASSERT_EQ (scan.count_in_flight (), 2);

	// External cleanup reclaims the stale slots; the count is reported back
	ASSERT_EQ (scan.cleanup (t0 + 500ms), 2);
	ASSERT_EQ (scan.count_in_flight (), 0);
	ASSERT_EQ (scan.count_pending (), 4);

	// With the gate open again, the next call drains everything pending in topological order
	auto unblocked = scan.next_blocks (10, t0 + 500ms);
	ASSERT_EQ (unblocked.size (), 4);
	ASSERT_EQ (unblocked[0], nano::block_hash{ 100 });
	ASSERT_EQ (unblocked[1], nano::block_hash{ 200 });
	ASSERT_EQ (unblocked[2], nano::block_hash{ 300 });
	ASSERT_EQ (unblocked[3], nano::block_hash{ 400 });
}