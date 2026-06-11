#pragma once

#include <nano/lib/async.hpp>

#include <algorithm>
#include <map>
#include <memory>

namespace nano::test
{
/**
 * A virtual time source for deterministic coroutine tests.
 * Sleeps are parked and completed by advance (); now () returns the owned time point.
 * Only safe with a test-owned, manually stepped io_context: advance () must not race a running io thread.
 */
class manual_clock final : public nano::async::clock
{
public:
	std::chrono::steady_clock::time_point now () const override
	{
		return current;
	}

	asio::awaitable<void> sleep_until (std::chrono::steady_clock::time_point deadline) override
	{
		if (deadline <= current)
		{
			co_return;
		}

		auto executor = co_await asio::this_coro::executor;

		co_await asio::async_initiate<decltype (asio::use_awaitable), void (boost::system::error_code)> (
		[this, deadline, executor] (auto && handler) {
			auto waiter = std::make_shared<nano::async::detail::waiter> ();
			waiter->handler = std::forward<decltype (handler)> (handler);
			waiter->executor = executor;

			auto slot = asio::get_associated_cancellation_slot (waiter->handler);
			if (slot.is_connected ())
			{
				slot.assign ([waiter] (asio::cancellation_type) {
					waiter->complete (asio::error::operation_aborted);
				});
			}

			pending.emplace (deadline, waiter);
		},
		asio::use_awaitable);
	}

	// Advances virtual time, completing due sleeps in deadline order.
	// Coroutines park again with already-past deadlines complete synchronously inside sleep_until,
	// so repeated advance + poll cycles drive periodic loops correctly.
	void advance (std::chrono::steady_clock::duration duration)
	{
		current += duration;
		while (!pending.empty () && pending.begin ()->first <= current)
		{
			auto waiter = pending.begin ()->second;
			pending.erase (pending.begin ());
			waiter->complete ({});
		}
	}

	std::size_t pending_count () const
	{
		// Cancelled waiters are removed lazily, do not count them
		return std::count_if (pending.begin (), pending.end (), [] (auto const & item) {
			return !item.second->completed;
		});
	}

private:
	std::chrono::steady_clock::time_point current{ std::chrono::steady_clock::time_point{} + std::chrono::hours{ 1 } };
	std::multimap<std::chrono::steady_clock::time_point, std::shared_ptr<nano::async::detail::waiter>> pending;
};
}
