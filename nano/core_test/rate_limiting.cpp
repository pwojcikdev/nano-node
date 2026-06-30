#include <nano/lib/rate_limiting.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <future>
#include <limits>
#include <vector>

using namespace std::chrono_literals;

TEST (rate_token_bucket, consume_and_refill)
{
	nano::rate::timestamp const start{};
	nano::rate::token_bucket bucket{ 10, 10, start };

	ASSERT_TRUE (bucket.try_consume (10, start));
	ASSERT_FALSE (bucket.try_consume (1, start));

	ASSERT_TRUE (bucket.try_consume (3, start + 300ms));
	ASSERT_FALSE (bucket.try_consume (1, start + 300ms));
	ASSERT_EQ (bucket.available (start + 400ms), 1);
}

TEST (rate_token_bucket, can_consume_does_not_deduct)
{
	nano::rate::timestamp const start{};
	nano::rate::token_bucket bucket{ 2, 1, start };

	ASSERT_TRUE (bucket.can_consume (2, start));
	ASSERT_TRUE (bucket.can_consume (2, start));
	ASSERT_EQ (bucket.available (start), 2);

	bucket.consume_checked (2, start);
	ASSERT_EQ (bucket.available (start), 0);
	ASSERT_FALSE (bucket.can_consume (1, start));
}

TEST (rate_token_bucket, reset_replaces_capacity_and_refill_rate)
{
	nano::rate::timestamp const start{};
	nano::rate::token_bucket bucket{ 1, 1, start };

	ASSERT_TRUE (bucket.try_consume (1, start));
	ASSERT_FALSE (bucket.try_consume (1, start));

	bucket.reset (5, 10, start + 1s);
	ASSERT_EQ (bucket.available (start + 1s), 5);
	ASSERT_TRUE (bucket.try_consume (5, start + 1s));
	ASSERT_EQ (bucket.available (start + 1500ms), 5);
}

TEST (rate_token_bucket, zero_capacity_and_zero_refill_are_limited)
{
	nano::rate::timestamp const start{};
	nano::rate::token_bucket empty_bucket{ 0, 10, start };
	nano::rate::token_bucket finite_bucket{ 2, 0, start };

	ASSERT_FALSE (empty_bucket.try_consume (1, start + 1s));
	ASSERT_TRUE (finite_bucket.try_consume (2, start));
	ASSERT_FALSE (finite_bucket.try_consume (1, start + 1h));
}

TEST (rate_limiter, burst_ratio_sets_capacity)
{
	nano::rate::timestamp const start{};
	nano::rate_limiter limiter{ { 10, 2.0 }, start };

	ASSERT_EQ (limiter.available (start), 20);
	ASSERT_TRUE (limiter.try_consume (20, start));
	ASSERT_FALSE (limiter.try_consume (1, start));
	ASSERT_EQ (limiter.available (start + 1s), 10);
}

TEST (rate_limiter, reset_and_get_limit)
{
	nano::rate::timestamp const start{};
	nano::rate_limiter limiter{ { 10, 2.0 }, start };

	ASSERT_EQ (limiter.get_limit (start), (nano::rate_limit{ 10, 2.0 }));

	limiter.reset ({ 5, 3.0 }, start);
	ASSERT_EQ (limiter.get_limit (start), (nano::rate_limit{ 5, 3.0 }));
	ASSERT_EQ (limiter.available (start), 15);
}

TEST (rate_limiter, zero_limit_is_unlimited)
{
	nano::rate::timestamp const start{};
	nano::rate_limiter limiter{ { 0, 1.0 }, start };

	ASSERT_TRUE (limiter.try_consume (std::numeric_limits<std::size_t>::max (), start));
	ASSERT_TRUE (limiter.can_consume (std::numeric_limits<std::size_t>::max (), start + 1h));
	ASSERT_EQ (limiter.available (start + 1h), std::numeric_limits<std::size_t>::max ());
}

TEST (rate_limiter_mt, forwards_timestamp_to_inner_limiter)
{
	nano::rate::timestamp const start{};
	nano::rate_limiter_mt limiter{ { 4, 1.0 }, start };

	ASSERT_TRUE (limiter.try_consume (4, start));
	ASSERT_FALSE (limiter.try_consume (1, start));
	ASSERT_EQ (limiter.available (start + 500ms), 2);
	ASSERT_TRUE (limiter.try_consume (2, start + 500ms));
}

TEST (rate_limiter_mt, concurrent_consumption_is_synchronized)
{
	nano::rate::timestamp const start{};
	constexpr auto token_count = 1000;
	nano::rate_limiter_mt limiter{ { token_count, 1.0 }, start };
	std::atomic<int> consumed{ 0 };
	std::vector<std::future<void>> futures;

	for (auto thread = 0; thread < 8; ++thread)
	{
		futures.push_back (std::async (std::launch::async, [&] () {
			while (limiter.try_consume (1, start))
			{
				++consumed;
			}
		}));
	}
	for (auto & future : futures)
	{
		future.get ();
	}

	ASSERT_EQ (consumed, token_count);
	ASSERT_EQ (limiter.available (start), 0);
}
