#include <nano/lib/async.hpp>
#include <nano/lib/logging.hpp>
#include <nano/lib/thread_pool.hpp>
#include <nano/lib/thread_runner.hpp>
#include <nano/test_common/async.hpp>
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
// Deterministic fixture: the test owns and manually steps the io_context, no runner threads.
// Combined with manual_clock this makes coroutine scheduling and timeouts fully deterministic.
class stepped_context
{
public:
	asio::io_context io_ctx;
	nano::async::strand strand{ io_ctx.get_executor () };
	nano::test::manual_clock clock;

	// Runs all handlers that are ready to run
	void flush ()
	{
		io_ctx.restart ();
		io_ctx.poll ();
	}

	// Advances virtual time and runs everything that became ready
	void advance (std::chrono::steady_clock::duration duration)
	{
		clock.advance (duration);
		flush ();
	}
};
}

TEST (async, condition_any_multiple_waiters)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	std::atomic<int> woken{ 0 };
	for (int i = 0; i < 3; ++i)
	{
		asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			co_await condition.wait ();
			++woken;
		},
		asio::detached);
	}

	ctx.flush ();
	ASSERT_EQ (woken, 0); // All parked

	condition.notify_all ();
	ctx.flush ();
	ASSERT_EQ (woken, 3); // The single-waiter nano::async::condition would livelock here

	// Waiters can park again after a notification
	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await condition.wait ();
		++woken;
	},
	asio::detached);

	ctx.flush ();
	ASSERT_EQ (woken, 3);
	condition.notify_all ();
	ctx.flush ();
	ASSERT_EQ (woken, 4);
}

TEST (async, condition_any_cancellation)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	std::atomic<bool> cancelled{ false };
	asio::cancellation_signal signal;

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		try
		{
			co_await condition.wait ();
		}
		catch (boost::system::system_error const & ex)
		{
			debug_assert (ex.code () == asio::error::operation_aborted);
			cancelled = true;
		}
	},
	asio::bind_cancellation_slot (signal.slot (), asio::detached));

	ctx.flush ();
	ASSERT_FALSE (cancelled);

	signal.emit (asio::cancellation_type::all);
	ctx.flush ();
	ASSERT_TRUE (cancelled);

	// A notification after the cancellation must not double-complete the waiter
	condition.notify_all ();
	ctx.flush ();
}

// Waiters cancelled by a raced timeout must not accumulate: a rarely-notified condition inside an
// until () loop parks and cancels one waiter per recheck interval, for the lifetime of the process
TEST (async, condition_any_cancelled_waiters_cleaned)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	bool ready = false;
	std::atomic<bool> done{ false };

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await nano::async::until (condition, ctx.clock, [&] () { return ready; });
		done = true;
	},
	asio::detached);

	// Each recheck interval cancels the parked waiter and parks a new one
	for (int i = 0; i < 10; ++i)
	{
		ctx.advance (std::chrono::seconds{ 1 });
	}
	ASSERT_FALSE (done);
	ASSERT_LE (condition.waiter_count (), 1); // Only the live waiter remains, the cancelled ones were removed

	ready = true;
	ctx.advance (std::chrono::seconds{ 1 });
	ASSERT_TRUE (done);
	ASSERT_EQ (condition.waiter_count (), 0);
}

TEST (async, semaphore_fifo)
{
	stepped_context ctx;
	nano::async::semaphore semaphore{ ctx.strand, 2 };

	std::vector<int> order;
	std::vector<nano::async::semaphore::token> tokens;

	for (int i = 0; i < 4; ++i)
	{
		asio::co_spawn (
		ctx.strand,
		[&, i] () -> asio::awaitable<void> {
			auto token = co_await semaphore.acquire ();
			order.push_back (i);
			tokens.push_back (std::move (token));
		},
		asio::detached);
	}

	ctx.flush ();
	ASSERT_EQ (order, (std::vector<int>{ 0, 1 })); // Two slots, two acquisitions
	ASSERT_EQ (semaphore.available (), 0);

	// Tokens must be released on the strand
	auto on_strand = [&] (auto action) {
		asio::co_spawn (
		ctx.strand,
		[&, action] () -> asio::awaitable<void> {
			action ();
			co_return;
		},
		asio::detached);
		ctx.flush ();
	};

	// Releasing one slot hands it to the first parked waiter, in FIFO order
	on_strand ([&] () { tokens.erase (tokens.begin ()); });
	ASSERT_EQ (order, (std::vector<int>{ 0, 1, 2 }));

	on_strand ([&] () { tokens.erase (tokens.begin ()); });
	ASSERT_EQ (order, (std::vector<int>{ 0, 1, 2, 3 }));

	on_strand ([&] () { tokens.clear (); });
	ASSERT_EQ (semaphore.available (), 2);
}

TEST (async, semaphore_cancellation)
{
	stepped_context ctx;
	nano::async::semaphore semaphore{ ctx.strand, 1 };

	std::optional<nano::async::semaphore::token> held;
	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		held = co_await semaphore.acquire ();
	},
	asio::detached);
	ctx.flush ();
	ASSERT_TRUE (held.has_value ());

	// Park a second acquirer, then cancel it
	std::atomic<bool> cancelled{ false };
	asio::cancellation_signal signal;
	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		try
		{
			auto token = co_await semaphore.acquire ();
		}
		catch (boost::system::system_error const & ex)
		{
			cancelled = true;
		}
	},
	asio::bind_cancellation_slot (signal.slot (), asio::detached));
	ctx.flush ();

	signal.emit (asio::cancellation_type::all);
	ctx.flush ();
	ASSERT_TRUE (cancelled);

	// Releasing the held token must not hand the slot to the cancelled waiter
	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		held.reset ();
		co_return;
	},
	asio::detached);
	ctx.flush ();
	ASSERT_EQ (semaphore.available (), 1);
}

TEST (async, oneshot)
{
	stepped_context ctx;

	// Set before wait completes immediately
	{
		nano::async::oneshot<int> event{ ctx.strand };
		event.set (42);
		ctx.flush ();

		std::optional<int> received;
		asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			received = co_await event.wait ();
		},
		asio::detached);
		ctx.flush ();
		ASSERT_EQ (received, 42);
	}

	// Wait before set parks until the value arrives; later sets are ignored
	{
		nano::async::oneshot<int> event{ ctx.strand };

		std::optional<int> received;
		asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			received = co_await event.wait ();
		},
		asio::detached);
		ctx.flush ();
		ASSERT_FALSE (received.has_value ());

		event.set (1);
		event.set (2); // Ignored, first set wins
		ctx.flush ();
		ASSERT_EQ (received, 1);
	}
}

// Pins the awaitable_operators semantics the request primitive relies on:
// `wait () || sleep ()` completes with the winner's index and cancels the loser
TEST (async, oneshot_with_timeout)
{
	stepped_context ctx;
	using namespace asio::experimental::awaitable_operators;

	// Value arrives before the deadline
	{
		nano::async::oneshot<int> event{ ctx.strand };
		std::optional<std::variant<int, std::monostate>> result;

		asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			result = co_await (event.wait () || ctx.clock.sleep_for (std::chrono::seconds{ 5 }));
		},
		asio::detached);
		ctx.flush ();
		ASSERT_EQ (ctx.clock.pending_count (), 1);

		event.set (7);
		ctx.flush ();
		ASSERT_TRUE (result.has_value ());
		ASSERT_EQ (result->index (), 0);
		ASSERT_EQ (std::get<0> (*result), 7);
		ASSERT_EQ (ctx.clock.pending_count (), 0); // The losing sleep was cancelled
	}

	// Deadline wins
	{
		nano::async::oneshot<int> event{ ctx.strand };
		std::optional<std::variant<int, std::monostate>> result;

		asio::co_spawn (
		ctx.strand,
		[&] () -> asio::awaitable<void> {
			result = co_await (event.wait () || ctx.clock.sleep_for (std::chrono::seconds{ 5 }));
		},
		asio::detached);
		ctx.flush ();
		ASSERT_FALSE (result.has_value ());

		ctx.advance (std::chrono::seconds{ 5 });
		ASSERT_TRUE (result.has_value ());
		ASSERT_EQ (result->index (), 1);
	}
}

TEST (async, scope_cancel_join)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	std::atomic<int> completed{ 0 };
	std::atomic<bool> joined{ false };

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		nano::async::scope scope{ ctx.strand };
		for (int i = 0; i < 3; ++i)
		{
			// The callable overload keeps the closure alive for the child's lifetime
			scope.spawn ([&] () -> asio::awaitable<void> {
				co_await condition.wait ();
				++completed;
			});
		}
		EXPECT_EQ (scope.size (), 3); // ASSERT_* returns void and cannot be used inside a coroutine

		co_await ctx.clock.sleep_for (std::chrono::seconds{ 1 }); // Yield so the children park

		// Wake all children and let them complete naturally
		condition.notify_all ();
		co_await ctx.clock.sleep_for (std::chrono::seconds{ 1 });

		// All children already done, cancel is a no-op and join returns immediately
		scope.cancel ();
		co_await scope.join ();
		joined = true;
	},
	asio::detached);

	ctx.flush ();
	ASSERT_EQ (completed, 0); // Everything parked

	ctx.advance (std::chrono::seconds{ 1 });
	ASSERT_EQ (completed, 3); // Children woken and completed

	ctx.advance (std::chrono::seconds{ 1 });
	ASSERT_TRUE (joined);
}

// Children parked in waits must observe scope cancellation and join must complete
TEST (async, scope_cancels_children)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	std::atomic<int> completed{ 0 };
	std::atomic<bool> joined{ false };

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		nano::async::scope scope{ ctx.strand };
		for (int i = 0; i < 3; ++i)
		{
			scope.spawn ([&] () -> asio::awaitable<void> {
				co_await condition.wait ();
				++completed; // Never reached, the children are cancelled while parked
			});
		}

		co_await ctx.clock.sleep_for (std::chrono::seconds{ 1 }); // Yield so the children park

		scope.cancel ();
		co_await scope.join ();
		joined = true;
	},
	asio::detached);

	ctx.flush ();
	ctx.advance (std::chrono::seconds{ 1 });

	ASSERT_TRUE (joined);
	ASSERT_EQ (completed, 0); // Cancelled children were shielded and never completed their work
}

TEST (async, scope_join_after_caller_cancelled)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	std::atomic<bool> joined{ false };
	asio::cancellation_signal signal;

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		nano::async::scope scope{ ctx.strand };
		scope.spawn ([&] () -> asio::awaitable<void> {
			co_await condition.wait ();
		});
		try
		{
			co_await condition.wait (); // The caller parks here and gets cancelled
		}
		catch (boost::system::system_error const &)
		{
		}
		// Sticky cancellation state: without this reset the `co_await scope.join ()` expression itself
		// throws operation_aborted, unwinding past the cleanup and tripping the scope destructor assert
		co_await asio::this_coro::reset_cancellation_state ();
		scope.cancel ();
		co_await scope.join ();
		joined = true;
	},
	asio::bind_cancellation_slot (signal.slot (), asio::detached));

	ctx.flush ();
	ASSERT_FALSE (joined);

	signal.emit (asio::cancellation_type::all);
	ctx.flush ();
	ASSERT_TRUE (joined);
}

TEST (async, until_predicate)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	bool ready = false;
	std::atomic<bool> done{ false };

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await nano::async::until (condition, ctx.clock, [&] () { return ready; });
		done = true;
	},
	asio::detached);

	ctx.flush ();
	ASSERT_FALSE (done);

	// Notification without the predicate holding parks again
	condition.notify_all ();
	ctx.flush ();
	ASSERT_FALSE (done);

	ready = true;
	condition.notify_all ();
	ctx.flush ();
	ASSERT_TRUE (done);
}

// The periodic recheck must catch a missed notification
TEST (async, until_missed_notify)
{
	stepped_context ctx;
	nano::async::condition_any condition{ ctx.strand };

	bool ready = false;
	std::atomic<bool> done{ false };

	asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<void> {
		co_await nano::async::until (condition, ctx.clock, [&] () { return ready; });
		done = true;
	},
	asio::detached);

	ctx.flush ();
	ready = true; // State changes without a notification

	ctx.advance (std::chrono::seconds{ 1 }); // The safety net recheck fires
	ASSERT_TRUE (done);
}

TEST (async, offload)
{
	test_context ctx; // Real io thread, offload involves a real thread pool

	nano::thread_pool pool{ 1, nano::thread_role::name::unknown, /* auto start */ true };

	auto fut = asio::co_spawn (
	ctx.strand,
	[&] () -> asio::awaitable<int> {
		co_return co_await nano::async::offload (pool, [] () {
			return 42;
		});
	},
	asio::use_future);

	ASSERT_EQ (fut.wait_for (5s), std::future_status::ready);
	ASSERT_EQ (fut.get (), 42);

	pool.stop ();
}
