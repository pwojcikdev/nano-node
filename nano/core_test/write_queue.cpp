#include <nano/store/write_queue.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST (write_queue, pessimistic_single)
{
	nano::store::write_queue queue;

	// Single pessimistic writer should work normally
	auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
	ASSERT_TRUE (guard.is_owned ());
	ASSERT_EQ (guard.type, nano::store::writer::testing);
	ASSERT_EQ (guard.strategy, nano::store::write_strategy::pessimistic);
}

TEST (write_queue, optimistic_single)
{
	nano::store::write_queue queue;

	// Single optimistic writer should work normally
	auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
	ASSERT_TRUE (guard.is_owned ());
	ASSERT_EQ (guard.type, nano::store::writer::testing);
	ASSERT_EQ (guard.strategy, nano::store::write_strategy::optimistic);
}

TEST (write_queue, multiple_optimistic_concurrent)
{
	nano::store::write_queue queue;
	std::atomic<int> concurrent_count{ 0 };
	std::atomic<int> max_concurrent{ 0 };
	constexpr int num_writers = 5;

	std::vector<std::future<void>> futures;

	for (int i = 0; i < num_writers; ++i)
	{
		futures.push_back (std::async (std::launch::async, [&] () {
			auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
			ASSERT_TRUE (guard.is_owned ());
			ASSERT_EQ (guard.strategy, nano::store::write_strategy::optimistic);

			// Count concurrent writers
			int current = ++concurrent_count;
			int expected = max_concurrent.load ();
			while (current > expected && !max_concurrent.compare_exchange_weak (expected, current))
			{
			}

			// Hold lock for a bit to allow others to acquire
			std::this_thread::sleep_for (50ms);

			--concurrent_count;
		}));
	}

	// Wait for all to complete
	for (auto & future : futures)
	{
		future.wait ();
	}

	// Multiple optimistic writers should have run concurrently
	ASSERT_GT (max_concurrent.load (), 1);
	ASSERT_EQ (concurrent_count.load (), 0);
}

TEST (write_queue, pessimistic_blocks_optimistic)
{
	nano::store::write_queue queue;
	std::atomic<bool> pessimistic_started{ false };
	std::atomic<bool> pessimistic_finished{ false };
	std::atomic<bool> optimistic_started{ false };
	std::atomic<bool> optimistic_finished{ false };

	// Start pessimistic writer first
	auto pessimistic_future = std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		pessimistic_started = true;

		// Hold lock while optimistic tries to start
		std::this_thread::sleep_for (100ms);

		// Optimistic should not have started yet
		EXPECT_FALSE (optimistic_started.load ());

		pessimistic_finished = true;
	});

	// Give pessimistic time to acquire lock
	std::this_thread::sleep_for (50ms);

	// Start optimistic writer
	auto optimistic_future = std::async (std::launch::async, [&] () {
		// Should block until pessimistic finishes
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
		optimistic_started = true;

		// Pessimistic should be finished by now
		EXPECT_TRUE (pessimistic_finished.load ());

		optimistic_finished = true;
	});

	pessimistic_future.wait ();
	optimistic_future.wait ();

	ASSERT_TRUE (pessimistic_started);
	ASSERT_TRUE (pessimistic_finished);
	ASSERT_TRUE (optimistic_started);
	ASSERT_TRUE (optimistic_finished);
}

TEST (write_queue, optimistic_waits_for_pessimistic)
{
	nano::store::write_queue queue;
	std::atomic<int> optimistic_count{ 0 };
	std::atomic<bool> pessimistic_can_start{ false };
	std::atomic<bool> pessimistic_started{ false };
	std::atomic<bool> pessimistic_finished{ false };

	constexpr int num_optimistic = 3;
	std::vector<std::future<void>> optimistic_futures;

	// Start multiple optimistic writers
	for (int i = 0; i < num_optimistic; ++i)
	{
		optimistic_futures.push_back (std::async (std::launch::async, [&] () {
			auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
			++optimistic_count;

			// Wait for signal to allow pessimistic to start
			while (!pessimistic_can_start.load ())
			{
				std::this_thread::sleep_for (1ms);
			}

			// Hold for a bit to ensure pessimistic waits
			std::this_thread::sleep_for (50ms);

			// Decrement before releasing the guard
			--optimistic_count;
		}));
	}

	// Wait for optimistic writers to start
	while (optimistic_count.load () < num_optimistic)
	{
		std::this_thread::sleep_for (1ms);
	}

	// Now start pessimistic writer
	pessimistic_can_start = true;
	auto pessimistic_future = std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		pessimistic_started = true;

		// All optimistic should be finished by now
		EXPECT_EQ (optimistic_count.load (), 0);

		pessimistic_finished = true;
	});

	// Wait for all to complete
	for (auto & future : optimistic_futures)
	{
		future.wait ();
	}
	pessimistic_future.wait ();

	ASSERT_TRUE (pessimistic_started);
	ASSERT_TRUE (pessimistic_finished);
	ASSERT_EQ (optimistic_count.load (), 0);
}

TEST (write_queue, fairness_ordering)
{
	nano::store::write_queue queue;
	std::vector<int> execution_order;
	std::mutex order_mutex;

	// This test verifies that the queue maintains fairness
	// even with mixed pessimistic and optimistic transactions

	std::vector<std::future<void>> futures;

	// Pattern: O1, P1, O2, P2, O3
	// Expected execution: O1, P1, O2, P2, O3 (in order)

	auto add_to_order = [&] (int id) {
		std::lock_guard<std::mutex> lock (order_mutex);
		execution_order.push_back (id);
	};

	// Start all transactions with small delays to ensure ordering
	futures.push_back (std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
		add_to_order (1);
		std::this_thread::sleep_for (10ms);
	}));

	std::this_thread::sleep_for (5ms);

	futures.push_back (std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		add_to_order (2);
		std::this_thread::sleep_for (10ms);
	}));

	std::this_thread::sleep_for (5ms);

	futures.push_back (std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
		add_to_order (3);
		std::this_thread::sleep_for (10ms);
	}));

	std::this_thread::sleep_for (5ms);

	futures.push_back (std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		add_to_order (4);
		std::this_thread::sleep_for (10ms);
	}));

	std::this_thread::sleep_for (5ms);

	futures.push_back (std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
		add_to_order (5);
		std::this_thread::sleep_for (10ms);
	}));

	// Wait for all to complete
	for (auto & future : futures)
	{
		future.wait ();
	}

	// Verify execution order maintained fairness
	std::vector<int> expected_order{ 1, 2, 3, 4, 5 };
	ASSERT_EQ (execution_order, expected_order);
}

TEST (write_queue, move_semantics)
{
	nano::store::write_queue queue;

	// Test move constructor
	auto guard1 = queue.wait (nano::store::writer::testing, nano::store::write_strategy::optimistic);
	ASSERT_TRUE (guard1.is_owned ());

	auto guard2 = std::move (guard1);
	ASSERT_FALSE (guard1.is_owned ()); // moved-from object should not own
	ASSERT_TRUE (guard2.is_owned ()); // moved-to object should own
	ASSERT_EQ (guard2.strategy, nano::store::write_strategy::optimistic);
}

TEST (write_queue, release_and_renew)
{
	nano::store::write_queue queue;

	auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
	ASSERT_TRUE (guard.is_owned ());

	// Release should give up ownership
	guard.release ();
	ASSERT_FALSE (guard.is_owned ());

	// Renew should reacquire
	guard.renew ();
	ASSERT_TRUE (guard.is_owned ());
}

TEST (write_queue, raii_cleanup)
{
	nano::store::write_queue queue;
	std::atomic<bool> second_guard_acquired{ false };

	// Start a thread that will wait for the first guard to be released
	auto future = std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		second_guard_acquired = true;
	});

	{
		// First guard should block the second
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		ASSERT_TRUE (guard.is_owned ());

		// Second guard should not have acquired yet
		std::this_thread::sleep_for (50ms);
		ASSERT_FALSE (second_guard_acquired.load ());

		// guard goes out of scope here and should automatically release
	}

	// Wait for second guard to acquire
	future.wait ();
	ASSERT_TRUE (second_guard_acquired.load ());
}

TEST (write_queue, pessimistic_exclusive_access)
{
	nano::store::write_queue queue;
	std::atomic<bool> first_started{ false };
	std::atomic<bool> first_finished{ false };
	std::atomic<bool> second_started{ false };

	// Start first pessimistic transaction
	auto first_future = std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		first_started = true;

		// Hold lock while second tries to acquire
		std::this_thread::sleep_for (100ms);

		// Second should not have started yet
		EXPECT_FALSE (second_started.load ());

		first_finished = true;
	});

	// Give first transaction time to acquire lock
	std::this_thread::sleep_for (25ms);

	// Start second pessimistic transaction
	auto second_future = std::async (std::launch::async, [&] () {
		auto guard = queue.wait (nano::store::writer::testing, nano::store::write_strategy::pessimistic);
		second_started = true;

		// First should be finished by now
		EXPECT_TRUE (first_finished.load ());
	});

	first_future.wait ();
	second_future.wait ();

	ASSERT_TRUE (first_started);
	ASSERT_TRUE (first_finished);
	ASSERT_TRUE (second_started);
}