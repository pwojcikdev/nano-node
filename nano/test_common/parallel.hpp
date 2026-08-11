#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace nano::test
{
/*
 * Tasks running concurrently on a set of threads, one per task index.
 * Poll finished () to wait without blocking, e.g. under ASSERT_TIMELY.
 * The destructor joins all threads, so tasks must not depend on the joining thread making progress.
 */
class parallel_tasks
{
public:
	parallel_tasks (size_t count, std::function<void (size_t)> task) :
		task{ std::move (task) }
	{
		threads.reserve (count);
		for (size_t i = 0; i < count; ++i)
		{
			threads.emplace_back ([this, i] () {
				this->task (i);
				++completed;
			});
		}
	}

	~parallel_tasks ()
	{
		for (auto & thread : threads)
		{
			thread.join ();
		}
	}

	// Threads capture this, the object must stay put
	parallel_tasks (parallel_tasks const &) = delete;
	parallel_tasks & operator= (parallel_tasks const &) = delete;

	// All tasks have run to completion
	bool finished () const
	{
		return completed == threads.size ();
	}

private:
	std::function<void (size_t)> task;
	std::atomic<size_t> completed{ 0 };
	std::vector<std::thread> threads;
};

// Start task (index) on count threads; poll finished () on the returned handle, threads join on destruction
inline parallel_tasks parallel_spawn (size_t count, std::function<void (size_t)> task)
{
	return parallel_tasks{ count, std::move (task) };
}

// Run task (index) on count threads and wait for all of them to complete
inline void parallel_for (size_t count, std::function<void (size_t)> task)
{
	parallel_tasks tasks{ count, std::move (task) };
}

// Run task (index) on count threads, wait for completion and collect the results in index order
template <typename Task>
auto parallel_map (size_t count, Task && task) -> std::vector<std::invoke_result_t<Task &, size_t>>
{
	using result_type = std::invoke_result_t<Task &, size_t>;
	std::vector<std::optional<result_type>> slots (count);
	parallel_for (count, [&] (size_t index) {
		slots[index] = task (index);
	});
	std::vector<result_type> results;
	results.reserve (count);
	for (auto & slot : slots)
	{
		results.push_back (std::move (*slot));
	}
	return results;
}
}
