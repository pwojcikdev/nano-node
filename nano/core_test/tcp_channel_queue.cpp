#include <nano/lib/asio.hpp>
#include <nano/transport/tcp_channel.hpp>
#include <nano/transport/traffic_type.hpp>

#include <gtest/gtest.h>

#include <string>

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
