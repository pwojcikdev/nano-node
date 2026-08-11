#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/numbers.hpp>
#include <nano/test_common/parallel.hpp>

#include <gtest/gtest.h>

TEST (random_pool, multithreading)
{
	nano::test::parallel_for (100, [] (size_t) {
		nano::uint256_union number;
		nano::random_pool::generate_block (number.bytes.data (), number.bytes.size ());
	});
}

// Test that random 64bit numbers are within the given range
TEST (random_pool, generate_word64)
{
	int occurrences[10] = { 0 };
	for (auto i = 0; i < 1000; ++i)
	{
		auto random = nano::random_pool::generate_word64 (1, 9);
		ASSERT_TRUE (random >= 1 && random <= 9);
		occurrences[random] += 1;
	}

	for (auto i = 1; i < 10; ++i)
	{
		ASSERT_GT (occurrences[i], 0);
	}
}

// Test random numbers > uint32 max
TEST (random_pool, generate_word64_big_number)
{
	uint64_t min = static_cast<uint64_t> (std::numeric_limits<uint32_t>::max ()) + 1;
	uint64_t max = std::numeric_limits<uint64_t>::max ();
	auto big_random = nano::random_pool::generate_word64 (min, max);
	ASSERT_GE (big_random, min);
}
