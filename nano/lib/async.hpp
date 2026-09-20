#pragma once

#include <nano/lib/utility.hpp>

#include <boost/asio.hpp>

#include <concepts>
#include <functional>
#include <future>
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

namespace detail
{
	/**
	 * State shared by one `await_callback` wait, its cancellation slot and every copy of the callback handed out.
	 * The handler is only touched on the awaiting executor, or in the destructor once nothing else refers to the state.
	 */
	template <typename Result, typename Handler>
	class callback_state final
	{
	public:
		callback_state (Handler handler_a, asio::any_io_executor executor_a) :
			executor{ std::move (executor_a) },
			handler{ std::move (handler_a) },
			work{ asio::make_work_guard (executor) }
		{
		}

		~callback_state ()
		{
			if (handler)
			{
				// Every copy of the callback is gone without a call; this may run on any thread, so it only posts
				asio::post (executor, [handler_l = std::move (*handler)] () mutable {
					std::move (handler_l) (boost::system::error_code{ asio::error::broken_pipe }, Result{});
				});
			}
		}

		// The first completion wins, must run on the awaiting executor
		void complete (boost::system::error_code ec, Result result)
		{
			if (handler)
			{
				// Posted rather than invoked, since this may be running inside a cancellation handler
				asio::post (executor, [handler_l = std::move (*handler), ec, result_l = std::move (result)] () mutable {
					std::move (handler_l) (ec, std::move (result_l));
				});
				handler.reset ();
				work.reset ();
			}
		}

		asio::any_io_executor const executor; // Where the awaiting coroutine runs

	private:
		std::optional<Handler> handler;
		std::optional<asio::executor_work_guard<asio::any_io_executor>> work; // Keeps the io_context running while the result is awaited
	};

	/** The callback `await_callback` hands out: copyable, callable from any thread, any number of times */
	template <typename Result, typename Handler>
	class callback final
	{
	public:
		explicit callback (std::shared_ptr<callback_state<Result, Handler>> state_a) :
			state{ std::move (state_a) }
		{
		}

		void operator() (Result result) const
		{
			asio::post (state->executor, [state_l = state, result_l = std::move (result)] () mutable {
				state_l->complete ({}, std::move (result_l));
			});
		}

	private:
		std::shared_ptr<callback_state<Result, Handler>> state;
	};
}

/**
 * Awaits the result of an API that reports it through a callback, which may fire from inside the call, later from
 * any thread, more than once or never. `function` runs on `invoke_on` and receives a copyable callback taking the
 * result; the awaiting coroutine, which must run on a strand, resumes there with the first result.
 * Fails with `operation_aborted` when cancelled, after which a late callback does nothing, and with `broken_pipe`
 * when every copy of the callback is destroyed without a call. A cancelled wait may end before `function` returns,
 * so `function` must not refer to anything the awaiting coroutine owns.
 */
template <std::default_initializable Result, typename Function, asio::completion_token_for<void (boost::system::error_code, Result)> Token = asio::use_awaitable_t<>>
auto await_callback (asio::any_io_executor invoke_on, Function function, Token && token = Token{})
{
	return asio::async_initiate<Token, void (boost::system::error_code, Result)> (
	[] (auto handler, asio::any_io_executor invoke_on, Function function) {
		using handler_type = std::decay_t<decltype (handler)>;
		asio::any_io_executor executor = asio::get_associated_executor (handler, invoke_on);
		debug_assert (executor.target<nano::async::strand> () != nullptr, "await_callback must be awaited on a strand");

		auto slot = asio::get_associated_cancellation_slot (handler);
		auto state = std::make_shared<detail::callback_state<Result, handler_type>> (std::move (handler), std::move (executor));
		if (slot.is_connected ())
		{
			// Abandoning the wait has no side effects, so any type of cancellation is honoured
			slot.assign ([state_w = std::weak_ptr{ state }] (asio::cancellation_type) {
				if (auto state_l = state_w.lock ())
				{
					state_l->complete (asio::error::operation_aborted, Result{});
				}
			});
		}
		asio::post (invoke_on, [function_l = std::move (function), callback = detail::callback<Result, handler_type>{ std::move (state) }] () mutable {
			function_l (std::move (callback));
		});
	},
	token, std::move (invoke_on), std::move (function));
}
}