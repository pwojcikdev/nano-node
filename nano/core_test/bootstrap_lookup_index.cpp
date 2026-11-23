#include <nano/lib/numbers.hpp>
#include <nano/node/bootstrap/lookup_index.hpp>

#include <gtest/gtest.h>

#include <chrono>

using namespace std::chrono_literals;

namespace
{
using index_t = nano::bootstrap::lookup_index;

struct test_context
{
	nano::lookup_config config;
	nano::bootstrap::lookup_index index;

	test_context () :
		index{ config }
	{
		config.max_blocks = 4;
		config.max_retries = 2;
		config.retry_delay = 250ms;
		config.retry_backoff = 2;
	}
};

nano::block_hash make_hash (uint64_t value)
{
	return nano::block_hash{ value };
}

nano::account make_peer (uint64_t value)
{
	return nano::account{ value };
}
}

/*
 * A missing block is due for a lookup as soon as it is added, with no peer to avoid yet.
 */
TEST (bootstrap_lookup_index, insert_due_right_away)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// Nothing is due while the index is empty
	ASSERT_FALSE (ctx.index.next (now));

	// A new block is tracked
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.contains (hash));
	ASSERT_EQ (ctx.index.size (), 1);

	// And offered for a lookup at once
	auto const request = ctx.index.next (now);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->hash, hash);
	ASSERT_TRUE (request->exclude.empty ());
}

/*
 * Adding a tracked block again changes nothing, whatever state its lookup is in.
 */
TEST (bootstrap_lookup_index, insert_duplicate)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// A waiting block stays a single entry
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::duplicate);
	ASSERT_EQ (ctx.index.size (), 1);

	// So does one with a lookup in flight, which keeps waiting for that lookup
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::duplicate);
	ASSERT_EQ (ctx.index.size (), 1);
	ASSERT_FALSE (ctx.index.next (now));
}

/*
 * The index is bounded: once full it refuses new blocks until one leaves.
 */
TEST (bootstrap_lookup_index, insert_overfill)
{
	test_context ctx;
	auto const now = std::chrono::steady_clock::now ();

	// Fill the index
	for (uint64_t value = 1; value <= ctx.config.max_blocks; ++value)
	{
		ASSERT_EQ (ctx.index.insert (make_hash (value), now), index_t::insert_result::inserted);
	}
	ASSERT_EQ (ctx.index.size (), ctx.config.max_blocks);

	// A further block is refused and the tracked ones stay
	auto const extra = make_hash (100);
	ASSERT_EQ (ctx.index.insert (extra, now), index_t::insert_result::overfill);
	ASSERT_FALSE (ctx.index.contains (extra));
	ASSERT_EQ (ctx.index.size (), ctx.config.max_blocks);

	// Once a block leaves there is room again
	ASSERT_TRUE (ctx.index.erase (make_hash (1)));
	ASSERT_EQ (ctx.index.insert (extra, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.contains (extra));
}

/*
 * Blocks are offered in the order they became due, and a block with a lookup in flight is not offered again.
 */
TEST (bootstrap_lookup_index, next_oldest_due_first)
{
	test_context ctx;
	auto const first = make_hash (1);
	auto const second = make_hash (2);
	auto const now = std::chrono::steady_clock::now ();

	// The second block is added later than the first
	ASSERT_EQ (ctx.index.insert (second, now + 1ms), index_t::insert_result::inserted);
	ASSERT_EQ (ctx.index.insert (first, now), index_t::insert_result::inserted);

	// Only the first one is due at first
	auto request = ctx.index.next (now);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->hash, first);

	// Later both are, the first one still ahead
	request = ctx.index.next (now + 1ms);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->hash, first);

	// Once the first one is looked up the second one is next
	ASSERT_TRUE (ctx.index.dispatch (first, /* id */ 1, make_peer (1)));
	request = ctx.index.next (now + 1ms);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->hash, second);

	// With both looked up nothing is offered
	ASSERT_TRUE (ctx.index.dispatch (second, /* id */ 2, make_peer (1)));
	ASSERT_FALSE (ctx.index.next (now + 1h));
}

/*
 * A lookup can only be recorded for a tracked block that waits for one.
 */
TEST (bootstrap_lookup_index, dispatch_requires_waiting_block)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// An untracked block takes no lookup
	ASSERT_FALSE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));

	// A waiting block takes one
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));

	// But not a second one while the first is in flight
	ASSERT_FALSE (ctx.index.dispatch (hash, /* id */ 2, make_peer (2)));
}

/*
 * A failed lookup is retried at a peer that has not been asked yet, after a delay that grows with every failure,
 * until the retries run out and the block is forgotten.
 */
TEST (bootstrap_lookup_index, fail_retries_with_backoff)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);

	// The first lookup fails and the block waits the initial delay
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::retry);
	ASSERT_FALSE (ctx.index.next (now + ctx.config.retry_delay - 1ms));

	// Then it is offered again, avoiding the peer that failed
	auto request = ctx.index.next (now + ctx.config.retry_delay);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->exclude, std::vector<nano::account>{ make_peer (1) });

	// The second failure doubles the wait
	auto const second_failure = now + 1s;
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 2, make_peer (2)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 2, second_failure), index_t::fail_result::retry);
	ASSERT_FALSE (ctx.index.next (second_failure + 2 * ctx.config.retry_delay - 1ms));

	// And both failed peers are avoided
	request = ctx.index.next (second_failure + 2 * ctx.config.retry_delay);
	ASSERT_TRUE (request);
	ASSERT_EQ (request->exclude, (std::vector<nano::account>{ make_peer (1), make_peer (2) }));

	// The last retry fails too, and the block is forgotten
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 3, make_peer (3)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 3, second_failure + 1s), index_t::fail_result::exhausted);
	ASSERT_FALSE (ctx.index.contains (hash));
	ASSERT_EQ (ctx.index.size (), 0);
}

/*
 * A failure that does not match the lookup the block waits on changes nothing.
 */
TEST (bootstrap_lookup_index, fail_stale)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// An untracked block has no lookup to fail
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::stale);

	// Neither has a block that still waits for its lookup
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::stale);
	ASSERT_TRUE (ctx.index.next (now));

	// An answer to another lookup leaves the one in flight alone
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 2, make_peer (1)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::stale);
	ASSERT_FALSE (ctx.index.next (now + 1h));

	// The lookup in flight still counts
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 2, now), index_t::fail_result::retry);
}

/*
 * When every peer has failed, the block can be looked up at them again.
 */
TEST (bootstrap_lookup_index, reset_peers)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// One peer failed to find the block
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::retry);
	ASSERT_FALSE (ctx.index.next (now + 1h)->exclude.empty ());

	// After the reset no peer is avoided, and the block keeps its schedule
	ctx.index.reset_peers (hash);
	ASSERT_FALSE (ctx.index.next (now));
	auto const request = ctx.index.next (now + ctx.config.retry_delay);
	ASSERT_TRUE (request);
	ASSERT_TRUE (request->exclude.empty ());

	// Resetting an untracked block does nothing
	ctx.index.reset_peers (make_hash (2));
	ASSERT_FALSE (ctx.index.contains (make_hash (2)));
}

/*
 * With no retries the first failed lookup is the last one.
 */
TEST (bootstrap_lookup_index, no_retries)
{
	test_context ctx;
	ctx.config.max_retries = 0;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::exhausted);
	ASSERT_FALSE (ctx.index.contains (hash));
}

/*
 * A backoff of one keeps every retry at the initial delay, and a zero delay retries right away.
 */
TEST (bootstrap_lookup_index, retry_delay_without_growth)
{
	test_context ctx;
	ctx.config.max_retries = 5;
	ctx.config.retry_backoff = 1;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);

	// Every failure waits the same delay
	for (nano::bootstrap::id_t id = 1; id <= 3; ++id)
	{
		ASSERT_TRUE (ctx.index.dispatch (hash, id, make_peer (id)));
		ASSERT_EQ (ctx.index.fail (hash, id, now), index_t::fail_result::retry);
		ASSERT_FALSE (ctx.index.next (now + ctx.config.retry_delay - 1ms));
		ASSERT_TRUE (ctx.index.next (now + ctx.config.retry_delay));
	}

	// Without a delay the block is due again at once, however often it failed
	ctx.config.retry_delay = 0ms;
	ctx.config.retry_backoff = 2;
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 4, make_peer (4)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 4, now), index_t::fail_result::retry);
	ASSERT_TRUE (ctx.index.next (now));
}

/*
 * The wait stops growing at an hour, so a large initial delay or backoff cannot push the time arithmetic out of range.
 */
TEST (bootstrap_lookup_index, retry_delay_bounded)
{
	test_context ctx;
	ctx.config.max_retries = 100;
	ctx.config.retry_delay = 40min;
	ctx.config.retry_backoff = 1'000'000;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);

	// The first retry waits the initial delay
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::retry);
	ASSERT_FALSE (ctx.index.next (now + 40min - 1ms));
	ASSERT_TRUE (ctx.index.next (now + 40min));

	// Every later one waits an hour, however many failures came before
	for (nano::bootstrap::id_t id = 2; id <= 50; ++id)
	{
		ASSERT_TRUE (ctx.index.dispatch (hash, id, make_peer (id)));
		ASSERT_EQ (ctx.index.fail (hash, id, now), index_t::fail_result::retry);
		ASSERT_FALSE (ctx.index.next (now + 1h - 1ms));
		ASSERT_TRUE (ctx.index.next (now + 1h));
	}
}

/*
 * Erasing a block forgets it, along with any lookup in flight.
 */
TEST (bootstrap_lookup_index, erase)
{
	test_context ctx;
	auto const hash = make_hash (1);
	auto const now = std::chrono::steady_clock::now ();

	// Nothing to erase for an untracked block
	ASSERT_FALSE (ctx.index.erase (hash));

	// A block with a lookup in flight is erased
	ASSERT_EQ (ctx.index.insert (hash, now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.dispatch (hash, /* id */ 1, make_peer (1)));
	ASSERT_TRUE (ctx.index.erase (hash));
	ASSERT_FALSE (ctx.index.contains (hash));
	ASSERT_EQ (ctx.index.size (), 0);

	// The answer to that lookup no longer matters
	ASSERT_EQ (ctx.index.fail (hash, /* id */ 1, now), index_t::fail_result::stale);
}

/*
 * Clearing the index forgets every block.
 */
TEST (bootstrap_lookup_index, clear)
{
	test_context ctx;
	auto const now = std::chrono::steady_clock::now ();

	ASSERT_EQ (ctx.index.insert (make_hash (1), now), index_t::insert_result::inserted);
	ASSERT_EQ (ctx.index.insert (make_hash (2), now), index_t::insert_result::inserted);
	ASSERT_TRUE (ctx.index.dispatch (make_hash (2), /* id */ 1, make_peer (1)));

	ctx.index.clear ();
	ASSERT_EQ (ctx.index.size (), 0);
	ASSERT_FALSE (ctx.index.next (now + 1h));
}
