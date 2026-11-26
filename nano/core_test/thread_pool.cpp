#include <nano/lib/thread_pool.hpp>
#include <nano/lib/timer.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <future>

TEST (thread_pool, thread_pool)
{
	std::atomic<bool> passed_sleep{ false };

	auto func = [&passed_sleep] () {
		std::this_thread::sleep_for (std::chrono::seconds (1));
		passed_sleep = true;
	};

	nano::thread_pool workers (1u, nano::thread_role::name::unknown);
	START (workers);
	workers.post (func);
	ASSERT_FALSE (passed_sleep);

	nano::timer<std::chrono::milliseconds> timer_l;
	timer_l.start ();
	while (!passed_sleep)
	{
		if (timer_l.since_start () > std::chrono::seconds (10))
		{
			break;
		}
	}
	ASSERT_TRUE (passed_sleep);
}

TEST (thread_pool, one)
{
	std::atomic<bool> done (false);
	nano::mutex mutex;
	nano::condition_variable condition;
	nano::thread_pool workers (1u, nano::thread_role::name::unknown);
	START (workers);
	workers.post ([&] () {
		{
			nano::lock_guard<nano::mutex> lock{ mutex };
			done = true;
		}
		condition.notify_one ();
	});
	nano::unique_lock<nano::mutex> unique{ mutex };
	condition.wait (unique, [&] () { return !!done; });
}

TEST (thread_pool, many)
{
	std::atomic<int> count (0);
	nano::mutex mutex;
	nano::condition_variable condition;
	nano::thread_pool workers (50u, nano::thread_role::name::unknown);
	START (workers);
	for (auto i (0); i < 50; ++i)
	{
		workers.post ([&] () {
			{
				nano::lock_guard<nano::mutex> lock{ mutex };
				count += 1;
			}
			condition.notify_one ();
		});
	}
	nano::unique_lock<nano::mutex> unique{ mutex };
	condition.wait (unique, [&] () { return count == 50; });
}

TEST (thread_pool, top_execution)
{
	int value1 (0);
	int value2 (0);
	nano::mutex mutex;
	std::promise<bool> promise;
	nano::thread_pool workers (1u, nano::thread_role::name::unknown);
	START (workers);
	workers.post ([&] () {
		nano::lock_guard<nano::mutex> lock{ mutex };
		value1 = 1;
		value2 = 1;
	});
	workers.post_delayed (std::chrono::milliseconds (1), [&] () {
		nano::lock_guard<nano::mutex> lock{ mutex };
		value2 = 2;
		promise.set_value (false);
	});
	promise.get_future ().get ();
	nano::lock_guard<nano::mutex> lock{ mutex };
	ASSERT_EQ (1, value1);
	ASSERT_EQ (2, value2);
}

TEST (thread_pool, discard_pending_tasks_on_stop)
{
	// Leave both tasks pending until stop releases their captures.
	nano::thread_pool workers{ 0, nano::thread_role::name::unknown, true };
	std::future<bool> inspection;
	bool inspected_during_cleanup{ false };
	auto inspect = [&] (void *) {
		inspection = std::async (std::launch::async, [&] { return workers.alive (); });
		inspected_during_cleanup = inspection.wait_for (std::chrono::seconds{ 5 }) == std::future_status::ready;
	};
	auto capture = std::shared_ptr<void>{ nullptr, inspect };

	workers.post ([] {});
	workers.post_delayed (std::chrono::hours{ 1 }, [capture = std::move (capture)] {});
	EXPECT_EQ (1, workers.queued_tasks ());
	EXPECT_EQ (1, workers.delayed_tasks ());

	workers.stop ();
	ASSERT_TRUE (inspection.valid ());
	EXPECT_FALSE (inspection.get ());
	EXPECT_TRUE (inspected_during_cleanup);
	EXPECT_EQ (0, workers.queued_tasks ());
	EXPECT_EQ (0, workers.delayed_tasks ());
}
