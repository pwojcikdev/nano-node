#include <nano/lib/assert.hpp>
#include <nano/lib/rate_limiting.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
std::size_t bucket_size (nano::rate_limit limit)
{
	release_assert (limit.burst_ratio >= 0);
	release_assert (std::isfinite (limit.burst_ratio));
	release_assert (limit.rate == 0 || limit.burst_ratio > 0);

	if (limit.rate == 0)
	{
		return 0;
	}

	auto const size = static_cast<long double> (limit.rate) * limit.burst_ratio;
	release_assert (size <= static_cast<long double> (std::numeric_limits<std::size_t>::max ()));

	return std::max (static_cast<std::size_t> (size), std::size_t{ 1 });
}

std::size_t elapsed_tokens (nano::rate::timestamp from, nano::rate::timestamp to, std::size_t refill_rate)
{
	auto const elapsed = std::chrono::duration<long double> (to - from).count ();
	auto const tokens = elapsed * refill_rate;
	if (tokens >= static_cast<long double> (std::numeric_limits<std::size_t>::max ()))
	{
		return std::numeric_limits<std::size_t>::max ();
	}
	return static_cast<std::size_t> (tokens);
}
}

/*
 * token_bucket
 */

nano::rate::token_bucket::token_bucket (std::size_t capacity, std::size_t refill_rate, timestamp now)
{
	reset (capacity, refill_rate, now);
}

bool nano::rate::token_bucket::can_consume (std::size_t tokens_required, timestamp now)
{
	refill (now);
	return current_size >= tokens_required;
}

bool nano::rate::token_bucket::try_consume (std::size_t tokens_required, timestamp now)
{
	refill (now);
	if (current_size < tokens_required)
	{
		return false;
	}

	current_size -= tokens_required;
	return true;
}

void nano::rate::token_bucket::consume_checked (std::size_t tokens_required, timestamp now)
{
	auto const consumed = try_consume (tokens_required, now);
	release_assert (consumed);
}

void nano::rate::token_bucket::reset (std::size_t capacity, std::size_t refill_rate, timestamp now)
{
	this->capacity = capacity;
	this->refill_rate = refill_rate;
	current_size = capacity;
	last_refill = now;
}

std::size_t nano::rate::token_bucket::available (timestamp now) const
{
	refill (now);
	return current_size;
}

void nano::rate::token_bucket::refill (timestamp now) const
{
	if (refill_rate == 0 || now <= last_refill)
	{
		return;
	}

	auto const tokens_to_add = elapsed_tokens (last_refill, now, refill_rate);
	if (tokens_to_add == 0)
	{
		return;
	}

	auto const tokens_added = std::min (tokens_to_add, capacity - current_size);
	current_size += tokens_added;
	if (current_size == capacity)
	{
		last_refill = now;
	}
	else
	{
		auto const refill_time = std::chrono::duration_cast<clock::duration> (std::chrono::duration<long double> (static_cast<long double> (tokens_added) / refill_rate));
		last_refill = refill_time > clock::duration::zero () ? last_refill + refill_time : now;
	}
}

/*
 * rate_limiter
 */

nano::rate_limiter::rate_limiter (nano::rate_limit limit, nano::rate::timestamp now) :
	bucket{ bucket_size (limit), limit.rate, now },
	configured_limit{ limit },
	unlimited{ limit.rate == 0 }
{
}

bool nano::rate_limiter::can_consume (std::size_t token_count, nano::rate::timestamp now)
{
	return unlimited || bucket.can_consume (token_count, now);
}

bool nano::rate_limiter::try_consume (std::size_t token_count, nano::rate::timestamp now)
{
	return unlimited || bucket.try_consume (token_count, now);
}

void nano::rate_limiter::consume_checked (std::size_t token_count, nano::rate::timestamp now)
{
	if (!unlimited)
	{
		bucket.consume_checked (token_count, now);
	}
}

void nano::rate_limiter::reset (nano::rate_limit limit, nano::rate::timestamp now)
{
	configured_limit = limit;
	unlimited = limit.rate == 0;
	bucket.reset (bucket_size (limit), limit.rate, now);
}

std::size_t nano::rate_limiter::available (nano::rate::timestamp now) const
{
	return unlimited ? std::numeric_limits<std::size_t>::max () : bucket.available (now);
}

nano::rate_limit nano::rate_limiter::get_limit (nano::rate::timestamp) const
{
	return configured_limit;
}

/*
 * rate_limiter_mt
 */

nano::rate_limiter_mt::rate_limiter_mt (nano::rate_limit limit, nano::rate::timestamp now) :
	limiter{ limit, now }
{
}

bool nano::rate_limiter_mt::can_consume (std::size_t token_count, nano::rate::timestamp now)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return limiter.can_consume (token_count, now);
}

bool nano::rate_limiter_mt::try_consume (std::size_t token_count, nano::rate::timestamp now)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return limiter.try_consume (token_count, now);
}

void nano::rate_limiter_mt::consume_checked (std::size_t token_count, nano::rate::timestamp now)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	limiter.consume_checked (token_count, now);
}

void nano::rate_limiter_mt::reset (nano::rate_limit limit, nano::rate::timestamp now)
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	limiter.reset (limit, now);
}

std::size_t nano::rate_limiter_mt::available (nano::rate::timestamp now) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return limiter.available (now);
}

nano::rate_limit nano::rate_limiter_mt::get_limit (nano::rate::timestamp now) const
{
	nano::lock_guard<nano::mutex> guard{ mutex };
	return limiter.get_limit (now);
}
