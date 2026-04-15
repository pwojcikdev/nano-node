#include <nano/lib/logging.hpp>
#include <nano/lib/stats.hpp>
#include <nano/lib/thread_context.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/lib/threading.hpp>
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
	nano::test::start_stop_guard stop_guard{ workers };
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
	nano::test::start_stop_guard stop_guard{ workers };
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
	nano::test::start_stop_guard stop_guard{ workers };
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
	nano::test::start_stop_guard stop_guard{ workers };
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

TEST (threading, thread_helper_binds_context)
{
	nano::logger logger;
	nano::stats stats{ logger };

	bool logger_bound{ false };
	bool stats_bound{ false };

	auto thread = nano::thread::create (nano::thread_role::name::unknown, nano::thread_context::context{ logger, stats }, [&] () {
		logger_bound = &nano::thread_context::logger () == &logger;
		stats_bound = &nano::thread_context::stats () == &stats;
	});

	thread.join ();

	ASSERT_TRUE (logger_bound);
	ASSERT_TRUE (stats_bound);
}

TEST (thread_pool, binds_context)
{
	nano::logger logger;
	nano::stats stats{ logger };

	std::promise<void> promise;
	bool logger_bound{ false };
	bool stats_bound{ false };

	nano::thread_pool workers (1u, nano::thread_role::name::unknown, nano::thread_context::context{ logger, stats });
	nano::test::start_stop_guard stop_guard{ workers };
	workers.post ([&] () {
		logger_bound = &nano::thread_context::logger () == &logger;
		stats_bound = &nano::thread_context::stats () == &stats;
		promise.set_value ();
	});

	promise.get_future ().get ();

	ASSERT_TRUE (logger_bound);
	ASSERT_TRUE (stats_bound);
}
