#pragma once

#include <nano/lib/locks.hpp>

#include <chrono>
#include <cstddef>

namespace nano
{
struct rate_limit
{
	std::size_t rate{ 0 }; // Tokens per second; 0 means unlimited.
	double burst_ratio{ 1.0 }; // Bucket capacity multiplier.

	bool operator== (rate_limit const &) const = default;
};
}

namespace nano::rate
{
using clock = std::chrono::steady_clock;
using timestamp = clock::time_point;

class token_bucket
{
public:
	// Creates a bucket with the given capacity and refill rate.
	token_bucket (std::size_t capacity, std::size_t refill_rate, timestamp now = clock::now ());

	// Returns true when the requested tokens are currently available.
	[[nodiscard]] bool can_consume (std::size_t tokens_required = 1, timestamp now = clock::now ());
	// Deducts tokens if they are currently available.
	[[nodiscard]] bool try_consume (std::size_t tokens_required = 1, timestamp now = clock::now ());
	// Deducts tokens and asserts if they are not currently available.
	void consume_checked (std::size_t tokens_required = 1, timestamp now = clock::now ());

	// Replaces bucket capacity and refill rate.
	void reset (std::size_t capacity, std::size_t refill_rate, timestamp now = clock::now ());

	// Returns the currently available token count.
	[[nodiscard]] std::size_t available (timestamp now = clock::now ()) const;

private:
	void refill (timestamp now) const;

private:
	std::size_t capacity{ 0 };
	std::size_t refill_rate{ 0 };
	mutable std::size_t current_size{ 0 };
	mutable timestamp last_refill{};
};
}

namespace nano
{
class rate_limiter final
{
public:
	// Creates a limiter; a zero rate is unlimited.
	explicit rate_limiter (nano::rate_limit limit, nano::rate::timestamp now = nano::rate::clock::now ());

	// Returns true when the requested tokens are currently available.
	[[nodiscard]] bool can_consume (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());
	// Deducts tokens if they are currently available.
	[[nodiscard]] bool try_consume (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());
	// Deducts tokens and asserts if they are not currently available.
	void consume_checked (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());

	// Replaces limiter configuration.
	void reset (nano::rate_limit limit, nano::rate::timestamp now = nano::rate::clock::now ());

	// Returns the currently available token count.
	[[nodiscard]] std::size_t available (nano::rate::timestamp now = nano::rate::clock::now ()) const;

	// Returns the configured long-term rate and burst ratio.
	[[nodiscard]] nano::rate_limit get_limit (nano::rate::timestamp now = nano::rate::clock::now ()) const;

private:
	nano::rate::token_bucket bucket;
	nano::rate_limit configured_limit;
	bool unlimited{ false };
};

class rate_limiter_mt final
{
public:
	// Creates a synchronized limiter; a zero rate is unlimited.
	explicit rate_limiter_mt (nano::rate_limit limit, nano::rate::timestamp now = nano::rate::clock::now ());

	// Returns true when the requested tokens are currently available.
	[[nodiscard]] bool can_consume (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());
	// Deducts tokens if they are currently available.
	[[nodiscard]] bool try_consume (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());
	// Deducts tokens and asserts if they are not currently available.
	void consume_checked (std::size_t token_count = 1, nano::rate::timestamp now = nano::rate::clock::now ());

	// Replaces limiter configuration.
	void reset (nano::rate_limit limit, nano::rate::timestamp now = nano::rate::clock::now ());

	// Returns the currently available token count.
	[[nodiscard]] std::size_t available (nano::rate::timestamp now = nano::rate::clock::now ()) const;

	// Returns the configured long-term rate and burst ratio.
	[[nodiscard]] nano::rate_limit get_limit (nano::rate::timestamp now = nano::rate::clock::now ()) const;

private:
	mutable nano::mutex mutex;
	nano::rate_limiter limiter;
};
}
