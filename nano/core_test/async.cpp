#include <nano/core_test/fakes/strand_blocker.hpp>
#include <nano/lib/async.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/test_common/system.hpp>
#include <nano/test_common/testutil.hpp>

#include <gtest/gtest.h>

#include <boost/asio.hpp>

#include <chrono>

using namespace std::chrono_literals;

namespace
{
class test_context
{
public:
	std::shared_ptr<asio::io_context> io_ctx{ std::make_shared<asio::io_context> () };
	nano::logger logger;
	nano::thread_runner runner{ io_ctx, logger, 1 };
	nano::async::strand strand{ io_ctx->get_executor () };
};
}

TEST (async, sleep)
{
	test_context ctx;

	auto fut = asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (500ms);
	},
	asio::use_future);

	ASSERT_EQ (fut.wait_for (100ms), std::future_status::timeout);
	ASSERT_EQ (fut.wait_for (1s), std::future_status::ready);
}

TEST (async, cancellation)
{
	test_context ctx;

	nano::async::cancellation cancellation{ ctx.strand };

	auto fut = asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (10s);
	},
	asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));

	ASSERT_EQ (fut.wait_for (500ms), std::future_status::timeout);

	cancellation.emit ();

	ASSERT_EQ (fut.wait_for (1s), std::future_status::ready);
	ASSERT_NO_THROW (fut.get ());
}

// Test that cancellation signal behaves well when the cancellation is emitted after the task has completed
TEST (async, cancellation_lifetime)
{
	test_context ctx;

	nano::async::cancellation cancellation{ ctx.strand };
	{
		auto fut = asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			co_await nano::async::sleep_for (100ms);
		},
		asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));
		ASSERT_EQ (fut.wait_for (1s), std::future_status::ready);
		fut.get ();
	}
	auto cancel_fut = cancellation.emit ();
	ASSERT_EQ (cancel_fut.wait_for (1s), std::future_status::ready);
}

TEST (async, task)
{
	nano::test::system system;
	test_context ctx;

	nano::async::task task{ ctx.strand };

	// Default state, empty task
	ASSERT_FALSE (task.joinable ());
	ASSERT_FALSE (task.running ());

	task = nano::async::task (ctx.strand, [&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (500ms);
	});

	// Task should now be joinable, but not ready
	ASSERT_TRUE (task.joinable ());
	ASSERT_TRUE (task.running ());
	ASSERT_FALSE (task.ready ());

	WAIT (50ms);
	ASSERT_TRUE (task.joinable ());
	ASSERT_TRUE (task.running ());
	ASSERT_FALSE (task.ready ());

	WAIT (1s);

	// Task completed, not yet joined
	ASSERT_TRUE (task.joinable ());
	ASSERT_FALSE (task.running ());
	ASSERT_TRUE (task.ready ());

	task.join ();

	// It is allowed to join a task multiple times
	ASSERT_TRUE (task.joinable ());
	task.join ();
}

TEST (async, task_cancel)
{
	nano::test::system system;
	test_context ctx;

	nano::async::task task = nano::async::task (ctx.strand, [&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (10s);
	});

	// Task should be joinable, but not ready
	WAIT (100ms);
	ASSERT_TRUE (task.joinable ());
	ASSERT_TRUE (task.running ());
	ASSERT_FALSE (task.ready ());

	task.cancel ();

	WAIT (500ms);
	ASSERT_TRUE (task.joinable ());
	ASSERT_TRUE (task.ready ());

	// It should not be necessary to join a ready task
}

TEST (async, task_join)
{
	nano::test::system system;
	test_context ctx;

	nano::async::task task = nano::async::task (ctx.strand, [&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (500ms);
	});

	ASSERT_FALSE (task.ready ());
	ASSERT_TRUE (task.running ());
	ASSERT_TRUE (task.joinable ());

	task.join ();
	ASSERT_TRUE (task.ready ());
	ASSERT_FALSE (task.running ());

	// It is allowed to join a task multiple times
	ASSERT_TRUE (task.joinable ());
	task.join ();
}

TEST (async, task_join_multithread)
{
	nano::test::system system;
	test_context ctx;

	nano::async::task task = nano::async::task (ctx.strand, [&] () -> asio::awaitable<void> {
		co_await nano::async::sleep_for (500ms);
	});

	ASSERT_FALSE (task.ready ());
	ASSERT_TRUE (task.joinable ());

	const int howmany_threads = 5;

	std::vector<std::thread> threads;
	for (int i = 0; i < howmany_threads; ++i)
	{
		threads.emplace_back ([&task] () {
			task.join ();
		});
	}

	// Join all threads
	for (auto & thread : threads)
	{
		thread.join ();
	}

	// Threads should be able to join the task without throwing an exception

	// It is allowed to join a task multiple times
	ASSERT_TRUE (task.joinable ());
	task.join ();
}

namespace
{
using string_callback = std::function<void (std::string)>;

// How one `await_callback` wait ended
struct callback_outcome
{
	std::string result;
	boost::system::error_code error;
	bool resumed_on_strand{ false };
	bool cancelled{ false }; // Cancellation state of the coroutine once it resumed
	std::atomic<int> resumptions{ 0 };
};

// Awaits `function` on the strand of `ctx` and records how the wait ended
std::future<void> spawn_await_callback (test_context & ctx, nano::async::cancellation & cancellation, std::function<void (string_callback)> function, callback_outcome & outcome)
{
	return asio::co_spawn (
	ctx.strand,
	[&ctx, &outcome, function = std::move (function)] () -> asio::awaitable<void> {
		try
		{
			outcome.result = co_await nano::async::await_callback<std::string> (ctx.io_ctx->get_executor (), function);
		}
		catch (boost::system::system_error const & ex)
		{
			outcome.error = ex.code ();
		}
		outcome.resumed_on_strand = ctx.strand.running_in_this_thread ();
		outcome.cancelled = (co_await asio::this_coro::cancellation_state).cancelled () != asio::cancellation_type::none;
		++outcome.resumptions;
	},
	asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));
}
}

/**
 * A callback fired from inside the call delivers its result. The call runs off the strand, the coroutine resumes on it
 */
TEST (async, await_callback_inline)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;
	std::atomic<bool> called_on_strand{ true };

	auto fut = spawn_await_callback (
	ctx, cancellation, [&] (string_callback callback) {
		called_on_strand = ctx.strand.running_in_this_thread ();
		callback ("inline");
	},
	outcome);

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ ("inline", outcome.result);
	ASSERT_FALSE (outcome.error);
	ASSERT_FALSE (called_on_strand);
	ASSERT_TRUE (outcome.resumed_on_strand);
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * A callback kept by the callee and fired later from a thread of its own delivers its result on the strand
 */
TEST (async, await_callback_other_thread)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;
	std::promise<string_callback> handed_over;

	auto fut = spawn_await_callback (
	ctx, cancellation, [&] (string_callback callback) {
		handed_over.set_value (std::move (callback));
	},
	outcome);

	auto callback = handed_over.get_future ().get ();
	ASSERT_EQ (fut.wait_for (100ms), std::future_status::timeout);

	std::thread thread{ [&] () { callback ("from a thread"); } };
	thread.join ();

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ ("from a thread", outcome.result);
	ASSERT_FALSE (outcome.error);
	ASSERT_TRUE (outcome.resumed_on_strand);
}

/**
 * Cancellation ends the wait at once with `operation_aborted`, and the callback fired afterwards does nothing
 */
TEST (async, await_callback_cancel)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;
	std::promise<string_callback> handed_over;

	auto fut = spawn_await_callback (
	ctx, cancellation, [&] (string_callback callback) {
		handed_over.set_value (std::move (callback));
	},
	outcome);

	auto callback = handed_over.get_future ().get ();
	cancellation.emit ();

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ (asio::error::operation_aborted, outcome.error);
	ASSERT_TRUE (outcome.resumed_on_strand);

	callback ("too late");
	asio::post (ctx.strand, asio::use_future ([] () {})).wait (); // Whatever the callback posted has run by now
	ASSERT_TRUE (outcome.result.empty ());
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * A callee that drops the callback without ever calling it fails the wait instead of hanging it
 */
TEST (async, await_callback_dropped)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;

	auto fut = spawn_await_callback (
	ctx, cancellation, [] (string_callback) {}, outcome);

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ (asio::error::broken_pipe, outcome.error);
	ASSERT_TRUE (outcome.resumed_on_strand);
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * Only the first call of the callback counts
 */
TEST (async, await_callback_twice)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;

	auto fut = spawn_await_callback (
	ctx, cancellation, [] (string_callback callback) {
		callback ("first");
		callback ("second");
	},
	outcome);

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	asio::post (ctx.strand, asio::use_future ([] () {})).wait (); // The second call has been processed by now
	ASSERT_EQ ("first", outcome.result);
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * Cancellation that reaches the strand ahead of a result already on its way wins: the wait is cancelled and
 * the result is discarded. The strand is blocked while both are queued, which fixes their order.
 */
TEST (async, await_callback_cancel_before_result)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;
	std::promise<string_callback> handed_over;

	auto fut = spawn_await_callback (
	ctx, cancellation, [&] (string_callback callback) {
		handed_over.set_value (std::move (callback));
	},
	outcome);
	auto callback = handed_over.get_future ().get ();

	nano::test::strand_blocker blocker{ ctx.strand };
	cancellation.emit ();
	callback ("behind the cancellation");
	blocker.release ();

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ (asio::error::operation_aborted, outcome.error);
	ASSERT_TRUE (outcome.result.empty ());
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * A result that reaches the strand ahead of the cancellation is delivered, once, and the coroutine resumes
 * knowing it was cancelled, which is what lets a caller decide what a result that late is still worth
 */
TEST (async, await_callback_result_before_cancel)
{
	test_context ctx;
	nano::async::cancellation cancellation{ ctx.strand };
	callback_outcome outcome;
	std::promise<string_callback> handed_over;

	auto fut = spawn_await_callback (
	ctx, cancellation, [&] (string_callback callback) {
		handed_over.set_value (std::move (callback));
	},
	outcome);
	auto callback = handed_over.get_future ().get ();

	nano::test::strand_blocker blocker{ ctx.strand };
	callback ("ahead of the cancellation");
	cancellation.emit ();
	blocker.release ();

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_FALSE (outcome.error);
	ASSERT_EQ ("ahead of the cancellation", outcome.result);
	ASSERT_TRUE (outcome.cancelled);
	ASSERT_EQ (1, outcome.resumptions);
}

/**
 * With the callback and the cancellation released at the same instant from two threads, every wait still ends
 * exactly once, either with the result or cancelled. The two orders are covered one by one above.
 */
TEST (async, await_callback_cancel_race)
{
	test_context ctx;

	for (int round = 0; round < 100; ++round)
	{
		nano::async::cancellation cancellation{ ctx.strand };
		callback_outcome outcome;
		std::promise<string_callback> handed_over;

		auto fut = spawn_await_callback (
		ctx, cancellation, [&] (string_callback callback) {
			handed_over.set_value (std::move (callback));
		},
		outcome);
		auto callback = handed_over.get_future ().get ();

		std::atomic<bool> ready{ false };
		std::atomic<bool> go{ false };
		std::thread thread{ [&] () {
			ready = true;
			while (!go)
			{
			}
			callback ("raced");
		} };
		while (!ready)
		{
		}
		go = true;
		auto emitted = cancellation.emit ();
		thread.join ();
		emitted.wait ();

		ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
		asio::post (ctx.strand, asio::use_future ([] () {})).wait ();
		bool const got_result = outcome.result == "raced" && !outcome.error;
		bool const got_cancelled = outcome.result.empty () && outcome.error == asio::error::operation_aborted;
		ASSERT_TRUE (got_result || got_cancelled);
		ASSERT_TRUE (outcome.resumed_on_strand);
		ASSERT_EQ (1, outcome.resumptions);
	}
}
