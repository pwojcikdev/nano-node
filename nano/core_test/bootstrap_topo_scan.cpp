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

// Simulate the cycle successfully reaching peers and fetching a block: issue
// the queued hashes (-> in_flight) and deliver one (-> received). This marks
// the cycle as having network reach, so a subsequent stall is treated as a
// poisoned anchor rather than a connectivity outage. The block stays
// `received`, keeping the queue outstanding.
void fetch_one (nano::bootstrap::topo_scan_index & scan, uint64_t hash_value, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now ())
{
	scan.next_blocks (1000, now);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ hash_value }, stub);
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
	// Closed-loop: process() queues entries but doesn't advance the cursor.
	// Only `mark_indexed` (ledger feedback) advances it.
	ASSERT_EQ (scan.cursor (), nano::topo_key{});

	// Simulate ledger feedback for the queued entries.
	scan.mark_indexed (nano::block_hash{ 100 }, 1);
	scan.mark_indexed (nano::block_hash{ 200 }, 2);
	scan.mark_indexed (nano::block_hash{ 300 }, 3);
	ASSERT_EQ (scan.cursor (), key (3, 300));

	auto next = scan.next ();
	ASSERT_TRUE (next.has_value ());
	ASSERT_EQ (*next, key (3, 300));

	// If a response carries more entries than cfg.candidates, the trim drops the surplus
	std::deque<nano::topo_key> oversize{ key (4, 400), key (5, 500), key (6, 600), key (7, 700), key (8, 800) };
	ASSERT_TRUE (scan.process (*next, oversize));
	ASSERT_EQ (scan.count_pending (), 6); // 3 from the first batch + 3 kept from the oversize batch
	ASSERT_EQ (scan.cursor (), key (3, 300)); // Cursor still at the last mark_indexed

	scan.mark_indexed (nano::block_hash{ 400 }, 4);
	scan.mark_indexed (nano::block_hash{ 500 }, 5);
	scan.mark_indexed (nano::block_hash{ 600 }, 6);
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

	// Advance the indexed cursor too, so reset is verified to clear it.
	scan.mark_indexed (nano::block_hash{ 100 }, 1);
	ASSERT_EQ (scan.cursor (), key (1, 100));

	// Drive indexing to completion (consideration_count*2 empty responses) so we reset from a fully-done state
	std::deque<nano::topo_key> empty;
	scan.process (*scan.next (t0 + 200ms), empty);
	ASSERT_TRUE (scan.process (*scan.next (t0 + 400ms), empty));
	ASSERT_FALSE (scan.indexing ());

	scan.reset ();

	ASSERT_TRUE (scan.indexing ());
	ASSERT_FALSE (scan.has_blocks_pending ());
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // indexed cleared

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

	// Receive the middle block first - drain stops because the head is not received.
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

	// Third response triggers a queue commit (discovery progresses).
	ASSERT_TRUE (scan.process (*p3, entries));
	ASSERT_EQ (scan.count_pending (), 3);
	// Closed-loop: cursor advances only via mark_indexed, not via process().
	ASSERT_EQ (scan.cursor (), nano::topo_key{});

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

	// Closed-loop: cursor advances only via mark_indexed, not via process().
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
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

	// Discovery cursor advances open-loop to the highest queued entry.
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

	// Advance the cursor via ledger feedback so the previously-issued p1 becomes
	// stale relative to the current `indexed`.
	scan.mark_indexed (nano::block_hash{ 300 }, 3);

	// Re-delivering at p1 must be rejected — its `start` no longer matches the
	// current cursor, so the response is treated as stale.
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
	scan.block_received (nano::block_hash{ 200 }, stub); // 200 -> received
	ASSERT_EQ (scan.count_in_flight (), 2);

	// Well past the retry cutoff: only the in_flight entries (100, 300) should reset.
	ASSERT_EQ (scan.cleanup (t0 + 1s), 2);
	ASSERT_EQ (scan.count_pending (), 2);
	ASSERT_EQ (scan.count_in_flight (), 0);
	ASSERT_EQ (scan.count_received (), 1);
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

// --- Closed-loop cursor (`mark_indexed`) ---

// mark_indexed: monotonic advance to (height, hash), exposed via cursor().
TEST (bootstrap_topo_scan, mark_indexed_monotonic)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	ASSERT_EQ (scan.cursor (), nano::topo_key{});

	scan.mark_indexed (nano::block_hash{ 100 }, 1);
	ASSERT_EQ (scan.cursor (), key (1, 100));

	scan.mark_indexed (nano::block_hash{ 200 }, 2);
	ASSERT_EQ (scan.cursor (), key (2, 200));

	// Lower height: ignored (cursor never moves backward)
	scan.mark_indexed (nano::block_hash{ 150 }, 1);
	ASSERT_EQ (scan.cursor (), key (2, 200));

	// Same key: ignored (strict less-than required for advance)
	scan.mark_indexed (nano::block_hash{ 200 }, 2);
	ASSERT_EQ (scan.cursor (), key (2, 200));

	// Same height, larger hash: advances (tie-broken by hash)
	scan.mark_indexed (nano::block_hash{ 250 }, 2);
	ASSERT_EQ (scan.cursor (), key (2, 250));
}

// `indexed` is closed-loop and independent of the open-loop discovery cursor:
// `process` advances discovery (visible via `next`) but never `cursor()`;
// `mark_indexed` advances `cursor()` and `indexed` only catches up as blocks
// confirm.
TEST (bootstrap_topo_scan, hybrid_end_to_end)
{
	auto cfg = default_config ();
	cfg.candidates = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Discovery round 1 — peers' response advances the discovery cursor.
	auto pos1 = scan.next ();
	ASSERT_EQ (*pos1, nano::topo_key{});
	scan.process (*pos1, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	ASSERT_EQ (*scan.next (), key (3, 300)); // Discovery cursor advanced
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // Indexed still at origin

	// Fetch and drain blocks.
	auto fetched = scan.next_blocks (10);
	ASSERT_EQ (fetched.size (), 3);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);
	auto drained = scan.next_ordered_blocks (10);
	ASSERT_EQ (drained.size (), 3);

	// Block processor confirms each block — `indexed` catches up.
	scan.block_done (nano::block_hash{ 100 });
	scan.mark_indexed (nano::block_hash{ 100 }, 1);
	scan.block_done (nano::block_hash{ 200 });
	scan.mark_indexed (nano::block_hash{ 200 }, 2);
	scan.block_done (nano::block_hash{ 300 });
	scan.mark_indexed (nano::block_hash{ 300 }, 3);

	ASSERT_EQ (scan.cursor (), key (3, 300)); // Indexed reached discovery cursor
	ASSERT_FALSE (scan.has_blocks_pending ());
}

// --- Redundant short-circuit (already-in-ledger blocks) ---

// mark_redundant only TAGS the entry — it must not advance the cursor (that's
// deferred to the in-order drain). The entry stays queued (counts toward
// backpressure) but won't be fetched.
TEST (bootstrap_topo_scan, mark_redundant_tags_no_cursor_advance)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });
	ASSERT_EQ (scan.count_outstanding (), 2);

	scan.mark_redundant (nano::block_hash{ 100 }, 1);

	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // NOT advanced here
	ASSERT_EQ (scan.count_redundant (), 1);
	ASSERT_EQ (scan.count_outstanding (), 2); // still queued
}

// A redundant entry is never handed out for fetching.
TEST (bootstrap_topo_scan, redundant_not_fetched)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });

	scan.mark_redundant (nano::block_hash{ 100 }, 1);

	auto to_fetch = scan.next_blocks (10);
	ASSERT_EQ (to_fetch.size (), 1);
	ASSERT_EQ (to_fetch[0], nano::block_hash{ 200 }); // 100 skipped (redundant)
}

// A contiguous `redundant` prefix at the head is drained in topological order,
// advancing the cursor to the last prefix entry's ledger topo_height.
TEST (bootstrap_topo_scan, redundant_prefix_drains_in_order)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });

	// First two are already in our ledger (peer height ≠ ledger height is fine).
	scan.mark_redundant (nano::block_hash{ 100 }, 11);
	scan.mark_redundant (nano::block_hash{ 200 }, 22);
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // still deferred

	auto drained = scan.next_ordered_blocks (10);
	ASSERT_TRUE (drained.empty ()); // nothing to submit (3rd is still pending)
	ASSERT_EQ (scan.cursor (), key (22, 200)); // advanced through the prefix
	ASSERT_EQ (scan.count_outstanding (), 1); // only the pending 3rd remains
}

// A redundant entry sitting BEHIND an unconfirmed (pending) entry must NOT
// advance the cursor — gap-safety. It is deferred until it reaches the
// contiguous head once the blocker clears.
TEST (bootstrap_topo_scan, interior_redundant_deferred_until_head)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });
	scan.mark_redundant (nano::block_hash{ 200 }, 22); // interior, behind pending 100

	// Head is pending(100) → prefix drain can't touch the interior redundant.
	auto d1 = scan.next_ordered_blocks (10);
	ASSERT_TRUE (d1.empty ());
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // NOT advanced — gap-safe

	// Resolve the blocker: fetch, receive, drain, confirm 100.
	scan.next_blocks (10);
	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	auto d2 = scan.next_ordered_blocks (10);
	ASSERT_EQ (d2.size (), 1); // 100 submitted
	scan.block_done (nano::block_hash{ 100 });

	// Now the redundant 200 is at the head → it advances the cursor in order.
	auto d3 = scan.next_ordered_blocks (10);
	ASSERT_TRUE (d3.empty ());
	ASSERT_EQ (scan.cursor (), key (22, 200));
	ASSERT_EQ (scan.count_outstanding (), 0);
}

// Draining the redundant prefix refreshes the drain heartbeat AND advances the
// cursor — so a subsequent stall is treated as real progress (gentle reset),
// not an escalating rewind.
TEST (bootstrap_topo_scan, redundant_prefix_is_progress_and_refreshes_clock)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 100;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 42 }, 5000);
	scan.reset_discovery (); // baseline indexed_at_reset = (5000, 42)

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (6001, 1), key (6002, 2) });
	scan.mark_redundant (nano::block_hash{ 1 }, 6000); // head redundant

	// Prefix drain at t0: advances cursor to (6000,1), refreshes drained_at.
	scan.next_ordered_blocks (10, t0);
	ASSERT_EQ (scan.cursor (), key (6000, 1));
	ASSERT_GT (scan.count_outstanding (), 0u); // pending 2 remains

	// Stall: real progress was made (cursor advanced past baseline) AND the
	// clock was refreshed → gentle reset, cursor preserved (NOT rewound).
	ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
	ASSERT_EQ (scan.cursor (), key (6000, 1));
	ASSERT_EQ (scan.count_outstanding (), 0);
}

// mark_redundant on a hash we don't track is a no-op (a concurrent live
// block_done may have evicted it).
TEST (bootstrap_topo_scan, mark_redundant_untracked_noop)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	scan.mark_redundant (nano::block_hash{ 777 }, 50);
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
	ASSERT_EQ (scan.count_redundant (), 0);
	ASSERT_EQ (scan.count_outstanding (), 0);
}

// A pre-topo-index redundant block (ledger topo_height == 0) is still evicted
// by the prefix drain but cannot anchor the cursor.
TEST (bootstrap_topo_scan, redundant_zero_height_evicts_only)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100) });
	scan.mark_redundant (nano::block_hash{ 100 }, 0);

	scan.next_ordered_blocks (10);
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // no anchor
	ASSERT_EQ (scan.count_outstanding (), 0); // still evicted
}

// --- Poisoning safeguard ---

// reset_discovery rolls discovery back to `indexed` and drops queued blocks
// while preserving the indexed cursor.
TEST (bootstrap_topo_scan, reset_discovery_rolls_back_to_indexed)
{
	auto cfg = default_config ();
	cfg.candidates = 5;
	nano::bootstrap::topo_scan_index scan{ cfg };

	// Discover and queue some entries; advance indexed only partway.
	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300), key (4, 400), key (5, 500) });
	ASSERT_EQ (scan.count_outstanding (), 5);
	scan.mark_indexed (nano::block_hash{ 100 }, 1);
	scan.mark_indexed (nano::block_hash{ 200 }, 2);
	ASSERT_EQ (scan.cursor (), key (2, 200));

	scan.reset_discovery ();

	// Indexed preserved, head + queue rewound.
	ASSERT_EQ (scan.cursor (), key (2, 200));
	ASSERT_EQ (scan.count_outstanding (), 0);
	ASSERT_FALSE (scan.has_blocks_pending ());

	// next() now offers the indexed cursor — fresh discovery starts from there.
	auto fresh = scan.next ();
	ASSERT_TRUE (fresh.has_value ());
	ASSERT_EQ (*fresh, key (2, 200));
}

// check_poisoning is a no-op when there's no outstanding work, regardless of
// how long it's been since the last drain.
TEST (bootstrap_topo_scan, check_poisoning_noop_when_idle)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();
	ASSERT_EQ (scan.count_outstanding (), 0);
	ASSERT_FALSE (scan.check_poisoning (t0 + 10s));
	ASSERT_EQ (scan.cursor (), nano::topo_key{});
}

// Outstanding work that hasn't yet exceeded the timeout must not trip the
// safeguard — a healthy fast-discovery / slow-processing window is fine.
TEST (bootstrap_topo_scan, check_poisoning_waits_for_timeout)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	ASSERT_EQ (scan.count_outstanding (), 3);

	// Within the timeout window since construction (the initial drain clock): no rollback.
	ASSERT_FALSE (scan.check_poisoning (t0 + 500ms));
	ASSERT_EQ (scan.count_outstanding (), 3);
}

// No-progress stall: rewind `indexed`, clamping at genesis when the rollback
// distance exceeds the current height; queue is purged; then held off because
// nothing is outstanding.
TEST (bootstrap_topo_scan, check_poisoning_rolls_back_on_stall)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 1000;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 7 }, 500); // below rollback_min
	scan.reset_discovery (); // baseline so the stall counts as no-progress

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (600, 1), key (601, 2) });
	fetch_one (scan, 1, t0); // peers reachable, but the chain won't connect
	ASSERT_EQ (scan.count_outstanding (), 2);

	// Past the timeout, nothing drained, no progress → rewind clamps at genesis.
	ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
	ASSERT_EQ (scan.cursor (), nano::topo_key{}); // 500 - 1000 clamped to 0
	ASSERT_EQ (scan.count_outstanding (), 0); // queue purged
	ASSERT_FALSE (scan.has_blocks_pending ());

	// Nothing outstanding → held off.
	ASSERT_FALSE (scan.check_poisoning (t0 + 2s + 500ms));
}

// A tracked block draining (block_done) refreshes the poisoning clock; while
// the queue keeps draining the safeguard never fires.
TEST (bootstrap_topo_scan, check_poisoning_held_off_by_drain)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	scan.next_blocks (10, t0);

	// A tracked block drains within the window — clock resets to t0+800ms.
	scan.block_done (nano::block_hash{ 100 }, t0 + 800ms);
	ASSERT_FALSE (scan.check_poisoning (t0 + 1500ms)); // 700ms since drain < 1s timeout
	ASSERT_EQ (scan.count_outstanding (), 2); // Two still queued, no rollback
}

// A ghost completion (block_done for a hash we don't track — e.g. a pre-reset
// submission) must NOT refresh the clock or mark the cycle productive.
TEST (bootstrap_topo_scan, check_poisoning_ignores_ghost_block_done)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });
	fetch_one (scan, 100, t0); // peers reachable
	ASSERT_EQ (scan.count_outstanding (), 2);

	// Ghost: hash 999 isn't in our queue. Even though it lands inside the
	// window, it must not refresh the drain clock.
	scan.block_done (nano::block_hash{ 999 }, t0 + 800ms);

	// Still stuck (clock unchanged since construction ~ t0) → fires.
	ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
	ASSERT_EQ (scan.count_outstanding (), 0);
}

// A connectivity outage (peers unreachable: requests issued, nothing
// delivered) must NOT rewind `indexed`, no matter how long it lasts —
// otherwise a transient outage would discard correct progress and keep
// doubling the rewind for its whole duration. Recovery is left to `cleanup`'s
// in-flight retry once the network returns.
TEST (bootstrap_topo_scan, check_poisoning_ignores_connectivity_stall)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 100;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 42 }, 10000);
	scan.reset_discovery (); // baseline (10000, 42) — a rewind would be visible

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (10001, 1), key (10002, 2) });
	scan.next_blocks (10, t0); // requests issued (-> in_flight), but no peer responds

	// Outage persists across many timeout windows: never rolls back, `indexed`
	// and the queue are left intact for `cleanup` to retry.
	for (int i = 2; i <= 16; i += 2)
	{
		ASSERT_FALSE (scan.check_poisoning (t0 + std::chrono::seconds (i)));
		ASSERT_EQ (scan.cursor (), key (10000, 42));
		ASSERT_EQ (scan.count_outstanding (), 2);
	}

	// Network returns: a block is delivered but the chain still won't connect.
	// The guard is not latched — a genuine fetched-but-stuck stall now rolls back.
	fetch_one (scan, 1, t0 + 16s);
	ASSERT_TRUE (scan.check_poisoning (t0 + 18s));
	ASSERT_EQ (scan.cursor (), key (9900, 0)); // 10000 - rollback_min
}

// --- Submitted state (drained-but-not-confirmed handoff) ---

// `next_ordered_blocks` moves `received` entries to `submitted` (not erase),
// so they still count toward `count_outstanding` and keep discovery
// backpressured until `block_done` evicts them.
TEST (bootstrap_topo_scan, submitted_state_holds_backpressure_until_done)
{
	auto cfg = default_config ();
	cfg.max_blocks_queued = 3;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	scan.next_blocks (10);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);
	ASSERT_EQ (scan.count_received (), 3);

	ASSERT_EQ (scan.next_ordered_blocks (10).size (), 3);

	// Drained → submitted, but still outstanding and at the cap.
	ASSERT_EQ (scan.count_received (), 0);
	ASSERT_EQ (scan.count_submitted (), 3);
	ASSERT_EQ (scan.count_outstanding (), 3);
	ASSERT_TRUE (scan.has_blocks_pending ());
	ASSERT_FALSE (scan.next ().has_value ()); // backpressure held by submitted

	// `block_done` (inspect callback) is the only thing that evicts/relieves.
	scan.block_done (nano::block_hash{ 100 });
	ASSERT_EQ (scan.count_submitted (), 2);
	ASSERT_EQ (scan.count_outstanding (), 2);
	ASSERT_TRUE (scan.next ().has_value ());
}

// A second drain call must not re-emit blocks that have already transitioned
// to `submitted` — the strategy would otherwise re-submit them to the
// processor on every loop iteration.
TEST (bootstrap_topo_scan, next_ordered_blocks_skips_submitted)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200) });
	scan.next_blocks (10);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.block_received (nano::block_hash{ 200 }, stub);

	auto first = scan.next_ordered_blocks (10);
	ASSERT_EQ (first.size (), 2);

	// Both are submitted now; nothing else received — drain yields nothing.
	auto second = scan.next_ordered_blocks (10);
	ASSERT_TRUE (second.empty ());
	ASSERT_EQ (scan.count_submitted (), 2);
}

// Drain logic must walk past `submitted` entries (already in the processor)
// to find subsequent `received` ones — once a topological predecessor is
// submitted it's safe to submit successors.
TEST (bootstrap_topo_scan, next_ordered_blocks_drains_past_submitted)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100), key (2, 200), key (3, 300) });
	scan.next_blocks (10);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	auto first = scan.next_ordered_blocks (10);
	ASSERT_EQ (first.size (), 1); // Only 100 was received
	ASSERT_EQ (scan.count_submitted (), 1);

	// Later blocks complete in order; drain must skip over the head's
	// `submitted` slot to reach them.
	scan.block_received (nano::block_hash{ 200 }, stub);
	scan.block_received (nano::block_hash{ 300 }, stub);
	auto second = scan.next_ordered_blocks (10);
	ASSERT_EQ (second.size (), 2);
	ASSERT_EQ (scan.count_submitted (), 3);
	ASSERT_EQ (scan.count_received (), 0);
}

// In-flight retry must not pick up a block that's already been drained —
// `submitted` is a terminal state until `block_done` fires.
TEST (bootstrap_topo_scan, submitted_blocks_not_refetched)
{
	auto cfg = default_config ();
	cfg.block_retry = 100ms;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100) });
	scan.next_blocks (10, t0);

	std::shared_ptr<nano::block> stub;
	scan.block_received (nano::block_hash{ 100 }, stub);
	scan.next_ordered_blocks (10); // 100 → submitted
	ASSERT_EQ (scan.count_submitted (), 1);

	// Well past the retry cutoff: `next_blocks` must not re-issue a fetch.
	auto retry = scan.next_blocks (10, t0 + 5s);
	ASSERT_TRUE (retry.empty ());
	ASSERT_EQ (scan.count_submitted (), 1);
	ASSERT_EQ (scan.count_in_flight (), 0);

	// `cleanup` (in-flight retry path) must also leave submitted entries alone.
	ASSERT_EQ (scan.cleanup (t0 + 5s), 0);
	ASSERT_EQ (scan.count_submitted (), 1);
}

// A late `block_received` (e.g. delayed peer response after we already drained
// the block from another peer) must not demote the entry back to `received`.
TEST (bootstrap_topo_scan, block_received_no_op_on_submitted)
{
	auto cfg = default_config ();
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto pos = scan.next ();
	scan.process (*pos, std::deque<nano::topo_key>{ key (1, 100) });
	scan.next_blocks (10);

	std::shared_ptr<nano::block> stub_first, stub_second;
	scan.block_received (nano::block_hash{ 100 }, stub_first);
	scan.next_ordered_blocks (10);
	ASSERT_EQ (scan.count_submitted (), 1);

	// Second arrival: must be ignored.
	scan.block_received (nano::block_hash{ 100 }, stub_second);
	ASSERT_EQ (scan.count_submitted (), 1);
	ASSERT_EQ (scan.count_received (), 0);
}

// --- Escalating rollback ---

// Consecutive unproductive stalls rewind `indexed` by a doubling distance.
TEST (bootstrap_topo_scan, escalating_rollback_doubles)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 100;
	cfg.rollback_max = 100000;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	// Park the indexed cursor up high, then baseline the cycle so this position
	// is "no progress yet" (a stall here must escalate, not be seen as advance).
	scan.mark_indexed (nano::block_hash{ 42 }, 10000);
	scan.reset_discovery ();
	ASSERT_EQ (scan.cursor (), key (10000, 42));

	auto seed = [&] (std::chrono::steady_clock::time_point t) {
		auto pos = scan.next (t);
		ASSERT_TRUE (pos.has_value ());
		scan.process (*pos, std::deque<nano::topo_key>{ key (10001, 1), key (10002, 2) });
		fetch_one (scan, 1, t); // peers reachable; the chain just won't connect
		ASSERT_GT (scan.count_outstanding (), 0u);
	};

	// Stall 1: no `indexed` advance → rewind by rollback_min (100): 10000 -> 9900.
	seed (t0);
	ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
	ASSERT_EQ (scan.cursor (), key (9900, 0));

	// Stall 2: still no advance → distance doubled to 200: 9900 -> 9700.
	seed (t0 + 2s);
	ASSERT_TRUE (scan.check_poisoning (t0 + 4s));
	ASSERT_EQ (scan.cursor (), key (9700, 0));

	// Stall 3: still no advance → distance doubled to 400: 9700 -> 9300.
	seed (t0 + 4s);
	ASSERT_TRUE (scan.check_poisoning (t0 + 6s));
	ASSERT_EQ (scan.cursor (), key (9300, 0));
}

// A cycle that genuinely advances `indexed` makes the next stall a *gentle*
// reset: indexed is preserved and the escalation step is re-armed. Progress is
// `mark_indexed` advancing the frontier — NOT merely a block draining.
TEST (bootstrap_topo_scan, advancing_cycle_is_gentle_and_rearms)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 100;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 42 }, 5000);
	scan.reset_discovery (); // baseline at (5000, 42)

	// First stall makes no frontier progress → escalate-rewind (5000 -> 4900),
	// distance doubles to 200.
	{
		auto pos = scan.next (t0);
		scan.process (*pos, std::deque<nano::topo_key>{ key (5001, 1) });
		fetch_one (scan, 1, t0); // peers reachable; chain won't connect
		ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
		ASSERT_EQ (scan.cursor (), key (4900, 0));
	}

	// Next cycle queues work AND the frontier genuinely advances (a new block
	// extends `indexed` past the cycle baseline of 4900).
	auto pos = scan.next (t0 + 2s);
	scan.process (*pos, std::deque<nano::topo_key>{ key (4901, 10), key (4902, 11) });
	scan.mark_indexed (nano::block_hash{ 50 }, 4950);

	// It then stalls again (the rest is stuck).
	ASSERT_TRUE (scan.check_poisoning (t0 + 5s));
	// Gentle: indexed preserved at the advanced position, queue cleared.
	ASSERT_EQ (scan.cursor (), key (4950, 50));
	ASSERT_EQ (scan.count_outstanding (), 0);

	// Escalation step re-armed: the next no-progress stall rewinds by
	// rollback_min (100) from the advanced position, not the doubled distance.
	auto pos2 = scan.next (t0 + 5s);
	scan.process (*pos2, std::deque<nano::topo_key>{ key (4951, 20) });
	fetch_one (scan, 20, t0 + 5s); // peers reachable; chain won't connect
	ASSERT_TRUE (scan.check_poisoning (t0 + 7s));
	ASSERT_EQ (scan.cursor (), key (4850, 0)); // 4950 - 100, not - 200
}

// A stuck cycle that re-chews already-known blocks as `old` moves the queue
// (block_done fires) but never advances `indexed`. That is not progress: the
// escalation must keep doubling, not snap back to rollback_min.
TEST (bootstrap_topo_scan, old_reprocessing_keeps_escalating)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 100;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 42 }, 10000);
	scan.reset_discovery (); // baseline at (10000, 42)

	// One stuck cycle: queue work, one entry re-processes as `old`
	// (block_done evicts it — queue moves, drained_at refreshes) but `indexed`
	// never advances. Returns the post-rollback cursor height.
	auto stuck_with_old_reprocessing = [&] (std::chrono::steady_clock::time_point t_seed,
									   std::chrono::steady_clock::time_point t_drain,
									   std::chrono::steady_clock::time_point t_check) {
		auto pos = scan.next (t_seed);
		ASSERT_TRUE (pos.has_value ());
		scan.process (*pos, std::deque<nano::topo_key>{ key (99990001, 1), key (99990002, 2) });
		fetch_one (scan, 1, t_seed); // peers reachable; block 1 delivered
		// Block 1 re-processes as `old`: tracked → block_done evicts it,
		// refreshing the drain heartbeat. NO mark_indexed (height <= indexed).
		scan.block_done (nano::block_hash{ 1 }, t_drain);
		// Block 2 stays stuck. Heartbeat goes stale `poisoning_timeout` after
		// t_drain → check_poisoning fires.
		ASSERT_TRUE (scan.check_poisoning (t_check));
	};

	// Each consecutive stuck cycle rewinds by a doubling distance; the `old`
	// block_done must not re-arm the step.
	stuck_with_old_reprocessing (t0, t0 + 500ms, t0 + 2s);
	ASSERT_EQ (scan.cursor (), key (9900, 0)); // 10000 - 100

	stuck_with_old_reprocessing (t0 + 2s, t0 + 2500ms, t0 + 4s);
	ASSERT_EQ (scan.cursor (), key (9700, 0)); // 9900 - 200 (doubled, NOT re-armed)

	stuck_with_old_reprocessing (t0 + 4s, t0 + 4500ms, t0 + 6s);
	ASSERT_EQ (scan.cursor (), key (9300, 0)); // 9700 - 400 (doubled again)
}

// The escalation step survives the empty-queue gap between a rollback and the
// next re-queue: a fresh stall still rewinds by the doubled distance.
TEST (bootstrap_topo_scan, check_poisoning_rearms)
{
	auto cfg = default_config ();
	cfg.poisoning_timeout = 1s;
	cfg.rollback_min = 10;
	nano::bootstrap::topo_scan_index scan{ cfg };

	auto const t0 = std::chrono::steady_clock::now ();

	scan.mark_indexed (nano::block_hash{ 9 }, 1000);
	scan.reset_discovery (); // baseline at (1000, 9)

	auto pos = scan.next (t0);
	scan.process (*pos, std::deque<nano::topo_key>{ key (1001, 1) });
	fetch_one (scan, 1, t0); // peers reachable
	ASSERT_TRUE (scan.check_poisoning (t0 + 2s));
	ASSERT_EQ (scan.cursor (), key (990, 0)); // 1000 - 10
	ASSERT_EQ (scan.count_outstanding (), 0);

	// Empty queue → no fire even past the timeout.
	ASSERT_FALSE (scan.check_poisoning (t0 + 4s));

	// Repopulate; no progress.
	auto pos2 = scan.next (t0 + 4s);
	ASSERT_TRUE (pos2.has_value ());
	scan.process (*pos2, std::deque<nano::topo_key>{ key (991, 2) });
	fetch_one (scan, 2, t0 + 4s); // peers reachable
	ASSERT_EQ (scan.count_outstanding (), 1);

	// Fresh timeout window → second rollback (distance doubled to 20).
	ASSERT_TRUE (scan.check_poisoning (t0 + 6s));
	ASSERT_EQ (scan.cursor (), key (970, 0)); // 990 - 20
	ASSERT_EQ (scan.count_outstanding (), 0);
}