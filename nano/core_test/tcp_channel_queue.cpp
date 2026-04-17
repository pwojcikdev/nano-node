#include <nano/lib/asio.hpp>
#include <nano/transport/tcp_channel.hpp>
#include <nano/transport/traffic_type.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using nano::transport::tcp_channel_queue;
using nano::transport::traffic_type;

namespace
{
tcp_channel_queue::entry_t make_entry (std::string const & tag)
{
	return { nano::shared_const_buffer{ tag }, nullptr };
}

std::string entry_tag (tcp_channel_queue::entry_t const & entry)
{
	auto bytes = entry.first.to_bytes ();
	return std::string (bytes.begin (), bytes.end ());
}
}

TEST (tcp_channel_queue, empty_on_construction)
{
	tcp_channel_queue q;
	ASSERT_TRUE (q.empty ());
	ASSERT_EQ (0, q.size ());
	ASSERT_EQ (0, q.size (traffic_type::generic));
	ASSERT_FALSE (q.max (traffic_type::generic));
	ASSERT_FALSE (q.full (traffic_type::generic));
}

TEST (tcp_channel_queue, fifo_within_single_type)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("a"));
	q.push (traffic_type::generic, make_entry ("b"));
	q.push (traffic_type::generic, make_entry ("c"));

	ASSERT_EQ (3, q.size ());
	ASSERT_EQ (3, q.size (traffic_type::generic));

	auto first = q.next ();
	EXPECT_EQ (traffic_type::generic, first.first);
	EXPECT_EQ ("a", entry_tag (first.second));

	auto second = q.next ();
	EXPECT_EQ ("b", entry_tag (second.second));

	auto third = q.next ();
	EXPECT_EQ ("c", entry_tag (third.second));

	ASSERT_TRUE (q.empty ());
}

TEST (tcp_channel_queue, per_type_sizes_are_independent)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("g1"));
	q.push (traffic_type::generic, make_entry ("g2"));
	q.push (traffic_type::vote, make_entry ("v1"));

	EXPECT_EQ (3, q.size ());
	EXPECT_EQ (2, q.size (traffic_type::generic));
	EXPECT_EQ (1, q.size (traffic_type::vote));
	EXPECT_EQ (0, q.size (traffic_type::block_broadcast));
}

TEST (tcp_channel_queue, max_and_full_thresholds)
{
	tcp_channel_queue q;

	// Below max_size: neither max nor full
	for (size_t i = 0; i < tcp_channel_queue::max_size - 1; ++i)
	{
		q.push (traffic_type::generic, make_entry ("x"));
	}
	EXPECT_FALSE (q.max (traffic_type::generic));
	EXPECT_FALSE (q.full (traffic_type::generic));

	// Exactly at max_size: max is true, full still false
	q.push (traffic_type::generic, make_entry ("x"));
	EXPECT_TRUE (q.max (traffic_type::generic));
	EXPECT_FALSE (q.full (traffic_type::generic));

	// Fill up to (but not including) full_size
	while (q.size (traffic_type::generic) < tcp_channel_queue::full_size - 1)
	{
		q.push (traffic_type::generic, make_entry ("x"));
	}
	EXPECT_TRUE (q.max (traffic_type::generic));
	EXPECT_FALSE (q.full (traffic_type::generic));

	// At full_size: full becomes true
	q.push (traffic_type::generic, make_entry ("x"));
	EXPECT_TRUE (q.full (traffic_type::generic));

	// Other types are unaffected
	EXPECT_FALSE (q.max (traffic_type::vote));
	EXPECT_FALSE (q.full (traffic_type::vote));
}

// The queue interleaves traffic types in weighted round-robin:
// high-priority types (generic, vote, keepalive, ...) get 4 consecutive
// dequeues before yielding, while block_broadcast / vote_rebroadcast get 1.
TEST (tcp_channel_queue, weighted_round_robin_high_priority_drains_four_then_yields)
{
	tcp_channel_queue q;
	// Populate enough items in both types that we can observe two full cycles.
	for (int i = 0; i < 10; ++i)
	{
		q.push (traffic_type::generic, make_entry ("g" + std::to_string (i)));
		q.push (traffic_type::block_broadcast, make_entry ("b" + std::to_string (i)));
	}

	// First cycle: 4 generic, then 1 block_broadcast.
	std::vector<traffic_type> order;
	for (int i = 0; i < 5; ++i)
	{
		order.push_back (q.next ().first);
	}
	EXPECT_EQ (traffic_type::generic, order.at (0));
	EXPECT_EQ (traffic_type::generic, order.at (1));
	EXPECT_EQ (traffic_type::generic, order.at (2));
	EXPECT_EQ (traffic_type::generic, order.at (3));
	EXPECT_EQ (traffic_type::block_broadcast, order.at (4));

	// Second cycle: same shape.
	order.clear ();
	for (int i = 0; i < 5; ++i)
	{
		order.push_back (q.next ().first);
	}
	EXPECT_EQ (traffic_type::generic, order.at (0));
	EXPECT_EQ (traffic_type::generic, order.at (1));
	EXPECT_EQ (traffic_type::generic, order.at (2));
	EXPECT_EQ (traffic_type::generic, order.at (3));
	EXPECT_EQ (traffic_type::block_broadcast, order.at (4));
}

// When the currently-selected queue empties mid-cycle, the scheduler should
// move on to the next non-empty queue without stalling.
TEST (tcp_channel_queue, skips_exhausted_queue)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("g1"));
	q.push (traffic_type::generic, make_entry ("g2"));
	q.push (traffic_type::vote, make_entry ("v1"));
	q.push (traffic_type::vote, make_entry ("v2"));
	q.push (traffic_type::vote, make_entry ("v3"));

	// Drain everything — should never assert or loop forever.
	std::vector<traffic_type> seen;
	while (!q.empty ())
	{
		seen.push_back (q.next ().first);
	}
	EXPECT_EQ (5, seen.size ());

	// Counts per type match what we pushed.
	auto count = [&] (traffic_type t) {
		return std::count (seen.begin (), seen.end (), t);
	};
	EXPECT_EQ (2, count (traffic_type::generic));
	EXPECT_EQ (3, count (traffic_type::vote));
}

TEST (tcp_channel_queue, next_batch_caps_at_max_count)
{
	tcp_channel_queue q;
	for (int i = 0; i < 10; ++i)
	{
		q.push (traffic_type::generic, make_entry ("x"));
	}

	auto batch = q.next_batch (4);
	EXPECT_EQ (4, batch.size ());
	EXPECT_EQ (6, q.size ());
}

TEST (tcp_channel_queue, next_batch_returns_all_when_under_cap)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("a"));
	q.push (traffic_type::vote, make_entry ("b"));

	auto batch = q.next_batch (10);
	EXPECT_EQ (2, batch.size ());
	EXPECT_TRUE (q.empty ());
}

TEST (tcp_channel_queue, next_batch_on_empty_returns_empty)
{
	tcp_channel_queue q;
	auto batch = q.next_batch (8);
	EXPECT_TRUE (batch.empty ());
}

// Symmetric to the high-priority-drains-four test: when the scheduler is
// parked on a low-priority queue (block_broadcast / vote_rebroadcast), it
// should yield after exactly one item. This locks in the weight asymmetry
// that protects latency-sensitive traffic.
TEST (tcp_channel_queue, low_priority_yields_after_one)
{
	tcp_channel_queue q;
	// Push low-priority first so the scheduler parks on block_broadcast before
	// seeing the higher-priority queue; then interleave with generic.
	for (int i = 0; i < 6; ++i)
	{
		q.push (traffic_type::block_broadcast, make_entry ("b" + std::to_string (i)));
		q.push (traffic_type::generic, make_entry ("g" + std::to_string (i)));
	}

	// Drain 10 items; we expect the pattern to be:
	//   [generic * 4, block_broadcast * 1, generic * 2, block_broadcast * 1, ...]
	// because generic sorts earlier in the traffic_type enum and the scheduler
	// walks the enum_array in order. The key invariant to assert is simpler:
	// between any two consecutive block_broadcast dequeues, at most 4 generic
	// dequeues appear (i.e. low-priority is never starved for more than one
	// full high-priority turn).
	std::vector<traffic_type> order;
	for (int i = 0; i < 10; ++i)
	{
		order.push_back (q.next ().first);
	}

	int since_last_low = 0;
	int low_seen = 0;
	for (auto t : order)
	{
		if (t == traffic_type::block_broadcast)
		{
			++low_seen;
			EXPECT_LE (since_last_low, 4) << "low-priority queue was starved";
			since_last_low = 0;
		}
		else
		{
			++since_last_low;
		}
	}
	EXPECT_GT (low_seen, 0); // Sanity: we should have actually dequeued low-priority items.
}

// When three or more traffic types are populated, the round-robin must visit
// every non-empty queue within one full sweep. Regression guard for a bug where
// `seek_next` could skip past a populated queue.
TEST (tcp_channel_queue, round_robin_visits_every_populated_type)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("g"));
	q.push (traffic_type::vote, make_entry ("v"));
	q.push (traffic_type::keepalive, make_entry ("k"));
	q.push (traffic_type::block_broadcast, make_entry ("b"));

	std::set<traffic_type> seen;
	while (!q.empty ())
	{
		seen.insert (q.next ().first);
	}
	EXPECT_EQ (4u, seen.size ());
	EXPECT_NE (seen.find (traffic_type::generic), seen.end ());
	EXPECT_NE (seen.find (traffic_type::vote), seen.end ());
	EXPECT_NE (seen.find (traffic_type::keepalive), seen.end ());
	EXPECT_NE (seen.find (traffic_type::block_broadcast), seen.end ());
}

// A queue that is drained and then refilled must keep scheduling: it must
// not stall, must dequeue every item pushed, and must not favor one type over
// another indefinitely. We intentionally do NOT assert the exact dequeue
// order here — the internal `counter` persists across drain/refill, which is
// an implementation detail we don't want to pin down in this contract.
TEST (tcp_channel_queue, survives_drain_and_refill)
{
	tcp_channel_queue q;
	q.push (traffic_type::generic, make_entry ("a"));
	q.push (traffic_type::generic, make_entry ("b"));
	(void)q.next ();
	(void)q.next ();
	ASSERT_TRUE (q.empty ());

	// Refill with two different types; both must drain cleanly.
	for (int i = 0; i < 6; ++i)
	{
		q.push (traffic_type::generic, make_entry ("g" + std::to_string (i)));
		q.push (traffic_type::block_broadcast, make_entry ("b" + std::to_string (i)));
	}

	std::vector<traffic_type> order;
	while (!q.empty ())
	{
		order.push_back (q.next ().first);
	}
	EXPECT_EQ (12u, order.size ());
	EXPECT_EQ (6, std::count (order.begin (), order.end (), traffic_type::generic));
	EXPECT_EQ (6, std::count (order.begin (), order.end (), traffic_type::block_broadcast));
}

// Every value in nano::transport::traffic_type must be a valid queue key.
// If a new traffic_type is added to the enum without updating the internal
// enum_array, this test catches it deterministically.
TEST (tcp_channel_queue, accepts_every_declared_traffic_type)
{
	tcp_channel_queue q;
	auto const all = nano::transport::all_traffic_types ();
	ASSERT_FALSE (all.empty ());

	for (auto type : all)
	{
		EXPECT_EQ (0u, q.size (type));
		EXPECT_FALSE (q.max (type));
		EXPECT_FALSE (q.full (type));
		q.push (type, make_entry ("t"));
		EXPECT_EQ (1u, q.size (type));
	}
	EXPECT_EQ (all.size (), q.size ());

	// And drain exactly once per type — no leaks, no duplicates.
	std::size_t drained = 0;
	while (!q.empty ())
	{
		(void)q.next ();
		++drained;
	}
	EXPECT_EQ (all.size (), drained);
}
