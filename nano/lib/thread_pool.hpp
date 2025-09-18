#pragma once

#include <nano/lib/relaxed_atomic.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <latch>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace nano
{
// High-performance thread pool implementation that avoids condition variable overhead under high load
class thread_pool final
{
public:
	static constexpr size_t batch_size = 16;
	static constexpr std::chrono::milliseconds wakeup_interval{ 100 };

	thread_pool (unsigned num_threads, nano::thread_role::name thread_name, bool auto_start = false) :
		num_threads{ num_threads },
		thread_name{ thread_name }
	{
		if (auto_start)
		{
			start ();
		}
	}

	~thread_pool ()
	{
		stop ();
	}

	void start ()
	{
		debug_assert (!stopped.load ());
		debug_assert (threads.empty ());

		for (unsigned i = 0; i < num_threads; ++i)
		{
			threads.emplace_back ([this, i] () {
				nano::thread_role::set (thread_name);
				run_worker (i);
			});
		}
	}

	void stop ()
	{
		{
			std::unique_lock lock{ mutex };
			stopped = true;
		}
		condition.notify_all ();

		for (auto & thread : threads)
		{
			if (thread.joinable ())
			{
				thread.join ();
			}
		}
		threads.clear ();
	}

	template <typename F>
	void post (F && task)
	{
		if (stopped)
		{
			return;
		}

		bool should_notify = false;
		{
			std::lock_guard lock{ mutex };

			tasks.emplace_back (std::forward<F> (task));
			num_tasks.fetch_add (1);

			auto queue_size = tasks.size ();
			auto sleeping = sleeping_workers.load ();

			// Only notify if we have sleeping workers and queue isn't too large
			if (sleeping > 0 && queue_size <= 4)
			{
				should_notify = true;
			}
		}

		if (should_notify)
		{
			condition.notify_one ();
		}
	}

	bool alive () const
	{
		return !stopped.load (std::memory_order_relaxed) && !threads.empty ();
	}

	uint64_t queued_tasks () const
	{
		return num_tasks.load (std::memory_order_relaxed);
	}

	nano::container_info container_info () const
	{
		nano::container_info info;
		info.put ("tasks", num_tasks.load ());
		return info;
	}

private:
	void run_worker (unsigned thread_index)
	{
		// Thread-local task buffer for batch processing
		std::vector<std::function<void ()>> local_tasks;
		local_tasks.reserve (batch_size);

		while (!stopped)
		{
			// Spinning phase before blocking
			bool found_work = false;
			for (int spin = 0; spin < 64; ++spin) // Moderate spin count
			{
				{
					std::lock_guard lock{ mutex };
					if (!tasks.empty ())
					{
						size_t tasks_to_take = std::min (tasks.size (), size_t{ batch_size });
						for (size_t i = 0; i < tasks_to_take; ++i)
						{
							local_tasks.push_back (std::move (tasks.front ()));
							tasks.pop_front ();
						}
						found_work = true;
						break;
					}
				}

				if (stopped)
					return;

				// Yield to avoid starving other threads
				std::this_thread::yield ();
			}

			// Process work found during spinning
			if (found_work)
			{
				for (auto & task : local_tasks)
				{
					task ();
					num_tasks.fetch_sub (1); // Keep default memory ordering
				}
				local_tasks.clear ();

				continue; // Go back to spinning
			}

			// No work found, go to sleep
			sleeping_workers.fetch_add (1);
			{
				std::unique_lock lock{ mutex };

				condition.wait_for (lock, wakeup_interval, [this] {
					return !tasks.empty () || stopped;
				});

				// Grab work after waking while still holding the lock
				if (!tasks.empty ())
				{
					size_t tasks_to_take = std::min (tasks.size (), size_t{ batch_size });
					for (size_t i = 0; i < tasks_to_take; ++i)
					{
						local_tasks.push_back (std::move (tasks.front ()));
						tasks.pop_front ();
					}
				}
			}
			sleeping_workers.fetch_sub (1);

			// Process tasks outside of lock
			for (auto & task : local_tasks)
			{
				task ();
				num_tasks.fetch_sub (1);
			}
			local_tasks.clear ();
		}
	}

private:
	unsigned const num_threads;
	nano::thread_role::name const thread_name;

	std::vector<std::thread> threads;
	mutable std::mutex mutex;
	std::condition_variable condition;
	std::atomic<bool> stopped{ false };

	// Task queue
	std::deque<std::function<void ()>> tasks;

	std::atomic<uint64_t> num_tasks{ 0 };
	std::atomic<uint32_t> sleeping_workers{ 0 };
};

class timed_thread_pool final
{
public:
	// TODO: Auto start should be removed once the node is refactored to start the thread pool explicitly
	timed_thread_pool (unsigned num_threads, nano::thread_role::name thread_name, bool auto_start = false) :
		num_threads{ num_threads },
		thread_name{ thread_name },
		thread_names_latch{ num_threads }
	{
		if (auto_start)
		{
			start ();
		}
	}

	~timed_thread_pool ()
	{
		// Must be stopped before destruction to avoid running takss when node components are being destroyed
		debug_assert (!thread_pool_impl);
	}

	void start ()
	{
		debug_assert (!stopped);
		debug_assert (!thread_pool_impl);
		thread_pool_impl = std::make_unique<boost::asio::thread_pool> (num_threads);
		set_thread_names ();
	}

	void stop ()
	{
		nano::unique_lock<nano::mutex> lock{ mutex };
		if (!stopped && thread_pool_impl)
		{
			stopped = true;

			// TODO: Is this still needed?
#if defined(BOOST_ASIO_HAS_IOCP)
			// A hack needed for Windows to prevent deadlock during destruction, described here: https://github.com/chriskohlhoff/asio/issues/431
			boost::asio::use_service<boost::asio::detail::win_iocp_io_context> (*thread_pool_impl).stop ();
#endif

			lock.unlock ();

			thread_pool_impl->stop ();
			thread_pool_impl->join ();

			lock.lock ();
			thread_pool_impl = nullptr;
		}
	}

	template <typename F>
	void post (F && task)
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (!stopped)
		{
			++num_tasks;
			release_assert (thread_pool_impl);
			boost::asio::post (*thread_pool_impl, [this, t = std::forward<F> (task)] () mutable {
				t ();
				--num_tasks;
			});
		}
	}

	template <typename F>
	void post_delayed (std::chrono::steady_clock::duration const & delay, F && task)
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		if (!stopped)
		{
			++num_delayed;
			release_assert (thread_pool_impl);
			auto timer = std::make_shared<boost::asio::steady_timer> (thread_pool_impl->get_executor ());
			timer->expires_after (delay);
			timer->async_wait ([this, t = std::forward<F> (task), /* preserve lifetime */ timer] (boost::system::error_code const & ec) mutable {
				if (!ec)
				{
					--num_delayed;
					post (std::move (t));
				}
			});
		}
	}

	bool alive () const
	{
		nano::lock_guard<nano::mutex> guard{ mutex };
		return thread_pool_impl != nullptr;
	}

	uint64_t queued_tasks () const
	{
		return num_tasks;
	}

	uint64_t delayed_tasks () const
	{
		return num_delayed;
	}

	nano::container_info container_info () const
	{
		nano::container_info info;
		info.put ("tasks", num_tasks);
		info.put ("delayed", num_delayed);
		return info;
	}

private:
	void set_thread_names ()
	{
		for (auto i = 0u; i < num_threads; ++i)
		{
			boost::asio::post (*thread_pool_impl, [this] () {
				nano::thread_role::set (thread_name);
				thread_names_latch.arrive_and_wait ();
			});
		}
		thread_names_latch.wait ();
	}

private:
	unsigned const num_threads;
	nano::thread_role::name const thread_name;

	std::latch thread_names_latch;
	mutable nano::mutex mutex;
	std::atomic<bool> stopped{ false };
	std::unique_ptr<boost::asio::thread_pool> thread_pool_impl;
	std::atomic<uint64_t> num_tasks{ 0 };
	std::atomic<uint64_t> num_delayed{ 0 };
};
}