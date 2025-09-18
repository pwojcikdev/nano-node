#pragma once

#include <nano/lib/relaxed_atomic.hpp>
#include <nano/lib/thread_roles.hpp>
#include <nano/lib/threading.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace nano
{
class thread_pool final
{
public:
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
				worker_thread (i);
			});
		}
	}

	void stop ()
	{
		{
			nano::unique_lock<nano::mutex> lock{ queue_mutex };
			if (stopped.exchange (true))
			{
				return; // Already stopped
			}
		}

		// Wake all threads to process the stop signal
		condition.notify_all ();

		// Join all threads
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
		if (stopped.load (std::memory_order_relaxed))
		{
			return;
		}

		bool should_notify = false;
		{
			nano::unique_lock<nano::mutex> lock{ queue_mutex };
			if (stopped.load (std::memory_order_relaxed))
			{
				return;
			}

			// Add task to queue
			tasks.emplace_back (std::forward<F> (task));
			num_tasks.fetch_add (1, std::memory_order_relaxed);

			// Adaptive signaling: only notify if we have sleeping workers
			// and the queue isn't already large (indicating busy workers)
			auto queue_size = tasks.size ();
			auto sleeping = sleeping_workers.load (std::memory_order_relaxed);

			// Only signal if:
			// 1. We have sleeping workers
			// 2. Queue is not already large (if it's large, workers are busy)
			// This avoids the __psynch_cvsignal overhead under high contention
			if (sleeping > 0 && queue_size <= num_threads * 2)
			{
				should_notify = true;
			}
		}

		// Notify outside of lock to reduce contention
		if (should_notify)
		{
			condition.notify_one ();
		}
	}

	template <typename F>
	void post_delayed (std::chrono::steady_clock::duration const & delay, F && task)
	{
		if (stopped.load (std::memory_order_relaxed))
		{
			return;
		}

		num_delayed.fetch_add (1, std::memory_order_relaxed);

		// Create a detached thread for the delay
		// This avoids boost::asio dependency and its overhead
		std::thread ([this, delay, t = std::forward<F> (task)] () mutable {
			std::this_thread::sleep_for (delay);
			num_delayed.fetch_sub (1, std::memory_order_relaxed);
			if (!stopped.load (std::memory_order_relaxed))
			{
				post (std::move (t));
			}
		}).detach ();
	}

	bool alive () const
	{
		return !stopped.load (std::memory_order_relaxed) && !threads.empty ();
	}

	uint64_t queued_tasks () const
	{
		return num_tasks.load (std::memory_order_relaxed);
	}

	uint64_t delayed_tasks () const
	{
		return num_delayed.load (std::memory_order_relaxed);
	}

	nano::container_info container_info () const
	{
		nano::container_info info;
		info.put ("tasks", num_tasks.load ());
		info.put ("delayed", num_delayed.load ());
		return info;
	}

private:
	void worker_thread (unsigned thread_index)
	{
		// Thread-local task buffer for batch processing
		std::vector<std::function<void()>> local_tasks;
		local_tasks.reserve (32); // Avoid allocations

		while (!stopped.load (std::memory_order_relaxed))
		{
			// Try to get tasks without blocking first (spinning phase)
			// This avoids the sleep/wake overhead under high load
			bool found_work = false;
			for (int spin = 0; spin < 128; ++spin)
			{
				{
					nano::unique_lock<nano::mutex> lock{ queue_mutex };
					if (!tasks.empty ())
					{
						// Batch dequeue: take multiple tasks at once
						// This reduces lock contention and amortizes overhead
						size_t batch_size = std::min (tasks.size (), size_t{ 16 });
						for (size_t i = 0; i < batch_size; ++i)
						{
							local_tasks.push_back (std::move (tasks.front ()));
							tasks.pop_front ();
						}
						found_work = true;
						break;
					}
				}

				if (stopped.load (std::memory_order_relaxed))
				{
					return;
				}

				// Yield to other threads during spin
				if (spin % 32 == 31)
				{
					std::this_thread::yield ();
				}
			}

			// Process any tasks we found
			if (found_work)
			{
				for (auto & task : local_tasks)
				{
					task ();
					num_tasks.fetch_sub (1, std::memory_order_relaxed);
				}
				local_tasks.clear ();
				continue;
			}

			// No work found after spinning, go to sleep
			sleeping_workers.fetch_add (1, std::memory_order_relaxed);
			{
				nano::unique_lock<nano::mutex> lock{ queue_mutex };

				// Double-check for work or stop signal
				if (tasks.empty () && !stopped.load (std::memory_order_relaxed))
				{
					// Use wait_for with timeout to periodically check for stop
					// This also helps prevent missed wakeups
					condition.wait_for (lock, std::chrono::milliseconds (100), [this] {
						return !tasks.empty () || stopped.load (std::memory_order_relaxed);
					});
				}

				// Try to grab work after waking
				if (!tasks.empty ())
				{
					// Batch dequeue after wake
					size_t batch_size = std::min (tasks.size (), size_t{ 16 });
					for (size_t i = 0; i < batch_size; ++i)
					{
						local_tasks.push_back (std::move (tasks.front ()));
						tasks.pop_front ();
					}
				}
			}
			sleeping_workers.fetch_sub (1, std::memory_order_relaxed);

			// Process tasks outside of lock
			for (auto & task : local_tasks)
			{
				task ();
				num_tasks.fetch_sub (1, std::memory_order_relaxed);
			}
			local_tasks.clear ();
		}
	}

private:
	unsigned const num_threads;
	nano::thread_role::name const thread_name;

	// Threading primitives
	std::vector<std::thread> threads;
	mutable nano::mutex queue_mutex;
	nano::condition_variable condition;
	std::atomic<bool> stopped{ false };

	// Task queue - using deque for efficient front/back operations
	std::deque<std::function<void()>> tasks;

	// Statistics
	std::atomic<uint64_t> num_tasks{ 0 };
	std::atomic<uint64_t> num_delayed{ 0 };
	std::atomic<uint32_t> sleeping_workers{ 0 };
};
}