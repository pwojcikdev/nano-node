#pragma once

#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <future>
#include <list>
#include <memory>
#include <optional>

namespace asio = boost::asio;

namespace nano::async
{
using strand = asio::strand<asio::io_context::executor_type>;

inline asio::awaitable<void> sleep_for (auto duration)
{
	asio::steady_timer timer{ co_await asio::this_coro::executor };
	timer.expires_after (duration);
	boost::system::error_code ec; // Swallow potential error from coroutine cancellation
	co_await timer.async_wait (asio::redirect_error (asio::use_awaitable, ec));
	debug_assert (!ec || ec == asio::error::operation_aborted);
}

inline asio::awaitable<bool> cancelled ()
{
	auto state = co_await asio::this_coro::cancellation_state;
	co_return state.cancelled () != asio::cancellation_type::none;
}

/**
 * A cancellation signal that can be emitted from any thread.
 * It follows the same semantics as asio::cancellation_signal.
 */
class cancellation
{
public:
	explicit cancellation (nano::async::strand & strand) :
		strand{ strand },
		signal{ std::make_shared<asio::cancellation_signal> () }
	{
	}

	cancellation (cancellation && other) = default;

	cancellation & operator= (cancellation && other)
	{
		// Can only move if the strands are the same
		debug_assert (strand == other.strand);
		if (this != &other)
		{
			signal = std::move (other.signal);
		}
		return *this;
	};

public:
	std::future<void> emit (asio::cancellation_type type = asio::cancellation_type::all)
	{
		return asio::dispatch (strand, asio::use_future ([signal_l = signal, type] () {
			signal_l->emit (type);
		}));
	}

	auto slot ()
	{
		// Ensure that the slot is only connected once
		debug_assert (std::exchange (slotted, true) == false);
		return signal->slot ();
	}

	nano::async::strand & strand;

private:
	std::shared_ptr<asio::cancellation_signal> signal;
	bool slotted{ false }; // For debugging purposes
};

class condition
{
public:
	explicit condition (nano::async::strand & strand) :
		strand{ strand },
		state{ std::make_shared<shared_state> (strand) }
	{
	}

	condition (condition &&) = default;

	condition & operator= (condition && other)
	{
		// Can only move if the strands are the same
		debug_assert (strand == other.strand);
		if (this != &other)
		{
			state = std::move (other.state);
		}
		return *this;
	}

	void notify ()
	{
		// Avoid unnecessary dispatch if already scheduled
		release_assert (state);
		if (state->scheduled.exchange (true) == false)
		{
			asio::dispatch (strand, [state_s = state] () {
				state_s->scheduled = false;
				state_s->timer.cancel ();
			});
		}
	}

	// Spuriously wakes up
	asio::awaitable<void> wait ()
	{
		debug_assert (strand.running_in_this_thread ());
		co_await wait_for (std::chrono::seconds{ 1 });
	}

	asio::awaitable<void> wait_for (auto duration)
	{
		debug_assert (strand.running_in_this_thread ());
		release_assert (state);
		state->timer.expires_after (duration);
		boost::system::error_code ec; // Swallow error from cancellation
		co_await state->timer.async_wait (asio::redirect_error (asio::use_awaitable, ec));
		debug_assert (!ec || ec == asio::error::operation_aborted);
	}

	void cancel ()
	{
		release_assert (state);
		asio::dispatch (strand, [state_s = state] () {
			state_s->scheduled = false;
			state_s->timer.cancel ();
		});
	}

	bool valid () const
	{
		return state != nullptr;
	}

	nano::async::strand & strand;

private:
	struct shared_state
	{
		asio::steady_timer timer;
		std::atomic<bool> scheduled{ false };

		explicit shared_state (nano::async::strand & strand) :
			timer{ strand } {};
	};
	std::shared_ptr<shared_state> state;
};

namespace detail
{
	// A single parked completion handler, completed exactly once by either its wake-up path or cancellation
	struct waiter
	{
		asio::any_completion_handler<void (boost::system::error_code)> handler;
		asio::any_io_executor executor; // Where the completion is delivered, set at park time
		bool completed{ false };

		void complete (boost::system::error_code ec)
		{
			if (std::exchange (completed, true))
			{
				return; // Already completed by the other path (wake-up vs cancellation)
			}
			asio::post (executor, [handler = std::move (handler), ec] () mutable {
				std::move (handler) (ec);
			});
		}
	};

	// Shared state of an offloaded function call, completed by either the result delivery or cancellation
	template <typename T>
	struct offload_state
	{
		asio::any_completion_handler<void (boost::system::error_code)> handler;
		asio::any_io_executor executor;
		std::optional<T> result;
		bool completed{ false };

		void complete (boost::system::error_code ec)
		{
			if (std::exchange (completed, true))
			{
				return;
			}
			asio::post (executor, [handler = std::move (handler), ec] () mutable {
				std::move (handler) (ec);
			});
		}
	};
}

/**
 * A condition variable for coroutines that supports multiple concurrent waiters.
 * Note that `nano::async::condition` only supports a single waiter (its shared timer is re-armed by each waiter,
 * cancelling the others), this class maintains a separate completion handler per waiter instead.
 * wait () must be awaited on the strand; notify_all () can be called from any thread.
 */
class condition_any
{
public:
	explicit condition_any (nano::async::strand & strand) :
		strand{ strand },
		state{ std::make_shared<shared_state> () }
	{
	}

	// Parks the awaiting coroutine until the next notify_all () or cancellation. Must be awaited on the strand.
	asio::awaitable<void> wait ()
	{
		debug_assert (strand.running_in_this_thread ());

		co_await asio::async_initiate<decltype (asio::use_awaitable), void (boost::system::error_code)> (
		[state_l = state, executor = strand] (auto && handler) {
			auto waiter = std::make_shared<detail::waiter> ();
			waiter->handler = std::forward<decltype (handler)> (handler);
			waiter->executor = executor;

			auto slot = asio::get_associated_cancellation_slot (waiter->handler);
			if (slot.is_connected ())
			{
				// The cancelled waiter must also be removed from the list, otherwise conditions that
				// are rarely notified accumulate dead waiters without bound
				slot.assign ([state_l, waiter] (asio::cancellation_type) {
					state_l->waiters.erase (std::remove (state_l->waiters.begin (), state_l->waiters.end (), waiter), state_l->waiters.end ());
					waiter->complete (asio::error::operation_aborted);
				});
			}

			state_l->waiters.push_back (waiter);
		},
		asio::use_awaitable);
	}

	// Wakes up all currently parked waiters. Safe to call from any thread.
	void notify_all ()
	{
		asio::dispatch (strand, [state_l = state] () {
			auto waiters = std::move (state_l->waiters);
			state_l->waiters.clear ();
			for (auto & waiter : waiters)
			{
				waiter->complete ({}); // Success
			}
		});
	}

	// Number of currently parked waiters, for tests; only meaningful between polls of a stepped executor
	std::size_t waiter_count () const
	{
		return state->waiters.size ();
	}

	nano::async::strand & strand;

private:
	struct shared_state
	{
		std::deque<std::shared_ptr<detail::waiter>> waiters;
	};

	std::shared_ptr<shared_state> state;
};

/**
 * A counting semaphore for coroutines with FIFO waiters. All operations must happen on the strand.
 * acquire () returns a move-only RAII token that returns the slot when destroyed.
 */
class semaphore
{
public:
	semaphore (nano::async::strand & strand, std::size_t limit) :
		strand{ strand },
		limit_m{ limit }
	{
	}

	~semaphore ()
	{
		release_assert (in_use == 0, "semaphore destroyed with outstanding tokens");
	}

	class token
	{
	public:
		token () = default;

		explicit token (semaphore & owner) :
			owner_m{ &owner }
		{
		}

		~token ()
		{
			release ();
		}

		token (token && other) noexcept :
			owner_m{ std::exchange (other.owner_m, nullptr) }
		{
		}

		token & operator= (token && other) noexcept
		{
			if (this != &other)
			{
				release ();
				owner_m = std::exchange (other.owner_m, nullptr);
			}
			return *this;
		}

		void release ()
		{
			if (auto owner = std::exchange (owner_m, nullptr))
			{
				owner->release ();
			}
		}

		explicit operator bool () const
		{
			return owner_m != nullptr;
		}

	private:
		semaphore * owner_m{ nullptr };
	};

	// Waits until a slot is available and reserves it. FIFO order between waiters. Must be awaited on the strand.
	asio::awaitable<token> acquire ()
	{
		debug_assert (strand.running_in_this_thread ());

		if (in_use < limit_m)
		{
			++in_use;
			co_return token{ *this };
		}

		co_await asio::async_initiate<decltype (asio::use_awaitable), void (boost::system::error_code)> (
		[this] (auto && handler) {
			auto waiter = std::make_shared<detail::waiter> ();
			waiter->handler = std::forward<decltype (handler)> (handler);
			waiter->executor = strand;

			auto slot = asio::get_associated_cancellation_slot (waiter->handler);
			if (slot.is_connected ())
			{
				slot.assign ([this, waiter] (asio::cancellation_type) {
					waiters.erase (std::remove (waiters.begin (), waiters.end (), waiter), waiters.end ());
					waiter->complete (asio::error::operation_aborted);
				});
			}

			waiters.push_back (waiter);
		},
		asio::use_awaitable);

		// The releasing token handed its slot directly to us, `in_use` is already accounted for
		co_return token{ *this };
	}

	// Read-only queries do not assert strand residence so that stepped tests can observe state between polls;
	// outside of tests they should still be called on the strand
	std::size_t available () const
	{
		return limit_m - std::min (in_use, limit_m);
	}

	std::size_t in_flight () const
	{
		return in_use;
	}

	std::size_t limit () const
	{
		return limit_m;
	}

	nano::async::strand & strand;

private:
	void release ()
	{
		debug_assert (strand.running_in_this_thread ());
		debug_assert (in_use > 0);

		// Hand the slot directly to the first waiter, keeping `in_use` unchanged
		while (!waiters.empty ())
		{
			auto waiter = waiters.front ();
			waiters.pop_front ();
			if (!waiter->completed)
			{
				waiter->complete ({});
				return;
			}
		}
		--in_use;
	}

	std::size_t const limit_m;
	std::size_t in_use{ 0 };
	std::deque<std::shared_ptr<detail::waiter>> waiters;
};

/**
 * A one-shot value event. set () stores the value and wakes the waiter; safe to call from any thread.
 * wait () must be awaited on the strand and completes with the value, or throws operation_aborted on cancellation.
 * Copyable handle to shared state, so the producer side can outlive the awaiting coroutine.
 */
template <typename T>
class oneshot
{
public:
	explicit oneshot (nano::async::strand & strand) :
		strand{ strand },
		state{ std::make_shared<shared_state> () }
	{
	}

	// Stores the value and wakes the waiter. First call wins, later calls are ignored. Safe to call from any thread.
	void set (T value)
	{
		asio::dispatch (strand, [state_l = state, value = std::move (value)] () mutable {
			if (state_l->value.has_value ())
			{
				return; // Already set
			}
			state_l->value = std::move (value);
			if (state_l->waiter)
			{
				state_l->waiter->complete ({});
			}
		});
	}

	// Waits for the value. Must be awaited on the strand, single waiter only.
	asio::awaitable<T> wait ()
	{
		debug_assert (strand.running_in_this_thread ());

		if (!state->value.has_value ())
		{
			co_await asio::async_initiate<decltype (asio::use_awaitable), void (boost::system::error_code)> (
			[state_l = state, executor = strand] (auto && handler) {
				debug_assert (state_l->waiter == nullptr || state_l->waiter->completed, "oneshot supports only a single waiter");

				auto waiter = std::make_shared<detail::waiter> ();
				waiter->handler = std::forward<decltype (handler)> (handler);
				waiter->executor = executor;

				auto slot = asio::get_associated_cancellation_slot (waiter->handler);
				if (slot.is_connected ())
				{
					slot.assign ([waiter] (asio::cancellation_type) {
						waiter->complete (asio::error::operation_aborted);
					});
				}

				state_l->waiter = waiter;
			},
			asio::use_awaitable);

			state->waiter = nullptr;
		}

		release_assert (state->value.has_value ());
		co_return std::move (*state->value);
	}

	bool ready () const
	{
		return state->value.has_value ();
	}

	nano::async::strand & strand;

private:
	struct shared_state
	{
		std::optional<T> value;
		std::shared_ptr<detail::waiter> waiter;
	};

	std::shared_ptr<shared_state> state;
};

/**
 * Time source for coroutines. Injectable so that tests can drive timeouts and cooldowns with virtual time.
 * sleep ops complete with operation_aborted (exception) when cancelled, which makes them usable with
 * asio::experimental::awaitable_operators to race a wait against a deadline.
 */
class clock
{
public:
	virtual ~clock () = default;

	virtual std::chrono::steady_clock::time_point now () const = 0;
	virtual asio::awaitable<void> sleep_until (std::chrono::steady_clock::time_point deadline) = 0;

	asio::awaitable<void> sleep_for (std::chrono::steady_clock::duration duration)
	{
		co_await sleep_until (now () + duration);
	}
};

// Production clock reading std::chrono::steady_clock, sleeping on a plain asio timer
class steady_clock_source final : public clock
{
public:
	std::chrono::steady_clock::time_point now () const override
	{
		return std::chrono::steady_clock::now ();
	}

	asio::awaitable<void> sleep_until (std::chrono::steady_clock::time_point deadline) override
	{
		asio::steady_timer timer{ co_await asio::this_coro::executor, deadline };
		co_await timer.async_wait (asio::use_awaitable); // Throws operation_aborted when cancelled, as intended
	}
};

/**
 * Races two awaitables, completing when the first of them does, cancelling the other.
 * Normalizes the cancellation behavior of awaitable operators: when the race itself is cancelled,
 * BOTH operands fail and operator|| throws asio::multiple_exceptions, which would sail past
 * operation_aborted handlers; the underlying first exception is rethrown instead.
 */
inline asio::awaitable<void> wait_any (asio::awaitable<void> first, asio::awaitable<void> second)
{
	try
	{
		using namespace asio::experimental::awaitable_operators;
		co_await (std::move (first) || std::move (second));
	}
	catch (asio::multiple_exceptions const & ex)
	{
		std::rethrow_exception (ex.first_exception ());
	}
}

// Repeatedly evaluates the predicate, parking on the condition between attempts.
// The periodic recheck acts as a liveness safety net against missed notifications.
template <typename Pred>
asio::awaitable<void> until (condition_any & condition, clock & clock, Pred && predicate, std::chrono::steady_clock::duration recheck_interval = std::chrono::seconds{ 1 })
{
	while (!predicate ())
	{
		co_await wait_any (condition.wait (), clock.sleep_until (clock.now () + recheck_interval));
	}
}

/**
 * A structured group of child coroutines. All operations must happen on the strand.
 * Children are automatically shielded from operation_aborted exceptions; any other exception propagates
 * out of the completion handler and terminates (loudly, by design).
 * The owner must cancel () + join () before destroying the scope.
 */
class scope
{
public:
	explicit scope (nano::async::strand & strand) :
		strand{ strand },
		joined{ strand }
	{
	}

	~scope ()
	{
		release_assert (count == 0, "scope must be joined before destruction");
	}

	scope (scope const &) = delete;
	scope & operator= (scope const &) = delete;

private:
	// Registers the child and builds its completion handler, which is the single authority on the child count
	auto make_completion ()
	{
		auto it = signals.insert (signals.end (), std::make_unique<asio::cancellation_signal> ());
		++count;
		return asio::bind_cancellation_slot ((*it)->slot (), [this, it] (std::exception_ptr exception) {
			signals.erase (it);
			--count;
			joined.notify_all ();
			if (exception)
			{
				std::rethrow_exception (exception); // Unexpected exception in a child, fail loudly
			}
		});
	}

public:
	// Starts a child coroutine owned by this scope. Must be called on the strand.
	// CAUTION: the awaitable must not come from a temporary closure — `spawn (lambda ())` leaves the coroutine
	// frame referencing the dead closure object. Use the callable overload for lambdas; this overload is for
	// member-function coroutines (`spawn (run_something (args))`) whose object outlives the scope.
	void spawn (asio::awaitable<void> task)
	{
		debug_assert (strand.running_in_this_thread ());
		asio::co_spawn (strand, shield (std::move (task)), make_completion ());
	}

	// Starts a child coroutine from a callable returning an awaitable. The callable is moved into the child
	// and kept alive for its whole lifetime, making lambda captures safe.
	template <typename F>
		requires requires (F f) { { f () } -> std::same_as<asio::awaitable<void>>; }
	void spawn (F func)
	{
		debug_assert (strand.running_in_this_thread ());
		asio::co_spawn (
		strand, [func = std::move (func)] () -> asio::awaitable<void> {
			try
			{
				co_await func ();
			}
			catch (boost::system::system_error const & ex)
			{
				debug_assert (ex.code () == asio::error::operation_aborted);
			}
			catch (asio::multiple_exceptions const &)
			{
				// All operands of a raced await were cancelled together
			}
		},
		make_completion ());
	}

	// Requests cancellation of all children. Must be called on the strand.
	void cancel ()
	{
		debug_assert (strand.running_in_this_thread ());
		for (auto & signal : signals)
		{
			signal->emit (asio::cancellation_type::all);
		}
	}

	// Waits until all children have completed. Intended as the last step before the owning coroutine returns.
	// CAUTION: a caller that has itself been cancelled must reset its cancellation state in its own frame first:
	//   co_await asio::this_coro::reset_cancellation_state ();
	// because with the default throw_if_cancelled the `co_await join ()` expression itself throws before this
	// coroutine even starts, unwinding past the caller's cleanup.
	asio::awaitable<void> join ()
	{
		debug_assert (strand.running_in_this_thread ());
		co_await asio::this_coro::reset_cancellation_state ();
		while (count > 0)
		{
			co_await joined.wait ();
		}
	}

	std::size_t size () const
	{
		return count;
	}

	nano::async::strand & strand;

private:
	static asio::awaitable<void> shield (asio::awaitable<void> task)
	{
		try
		{
			co_await std::move (task);
		}
		catch (boost::system::system_error const & ex)
		{
			debug_assert (ex.code () == asio::error::operation_aborted);
		}
		catch (asio::multiple_exceptions const &)
		{
			// All operands of a raced await were cancelled together
		}
	}

	std::list<std::unique_ptr<asio::cancellation_signal>> signals;
	std::size_t count{ 0 };
	condition_any joined;
};

/**
 * Runs a blocking function on the given thread pool and resumes the awaiting coroutine on its own executor
 * with the result. The operation completes with operation_aborted when cancelled while the function is still
 * queued or running; the function itself is never interrupted (it may complete after the cancellation).
 * The pool only needs a `post (F)` member; a stopped pool that drops the task is handled by cancellation.
 */
template <typename Pool, typename F>
asio::awaitable<std::invoke_result_t<F>> offload (Pool & pool, F function)
{
	using result_t = std::invoke_result_t<F>;
	static_assert (!std::is_void_v<result_t>, "offload requires a function returning a value");

	auto executor = co_await asio::this_coro::executor;
	auto state = std::make_shared<detail::offload_state<result_t>> ();
	state->executor = executor;

	co_await asio::async_initiate<decltype (asio::use_awaitable), void (boost::system::error_code)> (
	[&pool, &function, state] (auto && handler) {
		state->handler = std::forward<decltype (handler)> (handler);

		auto slot = asio::get_associated_cancellation_slot (state->handler);
		if (slot.is_connected ())
		{
			slot.assign ([state] (asio::cancellation_type) {
				state->complete (asio::error::operation_aborted);
			});
		}

		pool.post ([state, function = std::move (function)] () mutable {
			state->result = function ();
			// The completion flag is only touched on the awaiting executor to avoid racing the cancellation path
			asio::post (state->executor, [state] () {
				state->complete ({});
			});
		});
	},
	asio::use_awaitable);

	release_assert (state->result.has_value ());
	co_return std::move (*state->result);
}

// Concept for awaitables
template <typename T>
concept async_task = std::same_as<T, asio::awaitable<void>>;

// Concept for callables that return an awaitable
template <typename T>
concept async_factory = requires (T t) {
	{
		t ()
	} -> std::same_as<asio::awaitable<void>>;
};

// Concept for callables that take a condition and return an awaitable
template <typename T>
concept async_factory_with_condition = requires (T t, condition & c) {
	{
		t (c)
	} -> std::same_as<asio::awaitable<void>>;
};

/**
 * Wrapper with convenience functions and safety checks for asynchronous tasks.
 * Aims to provide interface similar to std::thread.
 * Uses std::shared_future for multiple waiters capability.
 */
class task
{
public:
	// Only thread-like void tasks are supported for now
	using value_type = void;

	explicit task (nano::async::strand & strand) :
		strand{ strand },
		cancellation{ strand }
	{
	}

	template <async_task Func>
	task (nano::async::strand & strand, Func && func) :
		strand{ strand },
		cancellation{ strand }
	{
		std::future<value_type> fut = asio::co_spawn (
		strand,
		std::forward<Func> (func),
		asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));

		// Convert to shared_future
		future = fut.share ();
	}

	template <async_factory Func>
	task (nano::async::strand & strand, Func && func) :
		strand{ strand },
		cancellation{ strand }
	{
		auto fut = asio::co_spawn (
		strand,
		func (),
		asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));

		future = fut.share (); // Convert to shared_future
	}

	template <async_factory_with_condition Func>
	task (nano::async::strand & strand, Func && func) :
		strand{ strand },
		cancellation{ strand },
		condition{ std::make_unique<nano::async::condition> (strand) }
	{
		auto awaitable_func = func (*condition);
		auto fut = asio::co_spawn (
		strand,
		func (*condition),
		asio::bind_cancellation_slot (cancellation.slot (), asio::use_future));

		future = fut.share (); // Convert to shared_future
	}

	~task ()
	{
		release_assert (!running (), "async task not joined before destruction");
	}

	task (task && other) = default;

	task & operator= (task && other)
	{
		// Can only move if the strands are the same
		debug_assert (strand == other.strand);
		if (this != &other)
		{
			future = std::move (other.future);
			cancellation = std::move (other.cancellation);
			condition = std::move (other.condition);
		}
		return *this;
	}

public:
	std::shared_future<value_type> get_future () const
	{
		return future; // Give a copy of the shared future
	}

	bool joinable () const
	{
		return future.valid ();
	}

	bool ready () const
	{
		release_assert (future.valid ());
		return get_future ().wait_for (std::chrono::seconds{ 0 }) == std::future_status::ready;
	}

	bool running () const
	{
		return joinable () && !ready ();
	}

	void join ()
	{
		release_assert (future.valid ());
		get_future ().wait ();
		// No need to invalidate shared_future after waiting
	}

	void cancel ()
	{
		debug_assert (joinable ());
		cancellation.emit ();
		if (condition)
		{
			condition->cancel ();
		}
	}

	void notify ()
	{
		if (condition)
		{
			condition->notify ();
		}
	}

	nano::async::strand & strand;

private:
	std::shared_future<value_type> future;
	nano::async::cancellation cancellation;
	std::unique_ptr<nano::async::condition> condition; // Optional, but needs a fixed address
};
}